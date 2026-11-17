#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lines/venue_order_session.h"
#include "lines/venue_order_translator.h"
#include "lines/venue_ws.h"

namespace venue_ws
{
class Stream;
}

// ADR 0006's third slice, the transport half: `OrderSession` is the state machine and this is the socket it runs beside.
// The login, the `orders` subscription, the ping cadence and the translation stay in the session, where a test drives
// them without a venue; what is left here is what a session cannot be: a connection that comes up, is read on a timeout,
// answers the venue's heartbeats, and reconnects -- logging in and subscribing again -- after it drops.
//
// The frames and the TLS stream are `venue_ws`'s, shared with the market leg, so this is the only place the order leg
// knows a socket exists. The engine's side is an `EnginePort`: the relay passes the socket it accepted from the engine,
// and a test passes a recorder, which is why the whole route can be exercised on a loopback socket with no venue.
namespace venue_order_wire
{
// The venue's demo endpoint, measured on 2026-11-13: the login and the `orders` subscription are accepted on port 443,
// and the venue sends a notice on 8443 saying that port will be retired.
inline constexpr const char* kDemoVenueUrl = "wss://wspap.okx.com:443/ws/v5/private?brokerId=9999";

// The three credentials a private channel needs, read from `TOYQUANT_API_KEY`, `TOYQUANT_API_SECRET` and
// `TOYQUANT_PASSPHRASE` and never from a command line, which reaches a run's `[Mode]` line and every recording it writes
// (ADR 0005, decision 3). Refuses with the missing variable named rather than logging in with an empty secret.
OrderSession::Credentials credentials_from_environment();

// Where the session's engine-side lines go.
class EnginePort
{
   public:
    virtual ~EnginePort() = default;
    virtual bool write_line(std::string_view line) = 0;
};

// One endpoint's request budget, as a token bucket: `requests_per_window` calls in `window_ms`, per path. The venue
// publishes a quota per endpoint and answers a request over it with HTTP 429, which this client used to count as a
// venue refusal -- so a strategy that placed faster than the venue allows looked like one whose orders were bad. 0
// disables the bucket, which is what a test of something else uses.
struct RestLimits
{
    std::uint64_t requests_per_window{10};
    std::int64_t window_ms{1000};
};

struct WireConfig
{
    std::string venue_url{kDemoVenueUrl};
    // The placement surface. Empty places over the private socket's `op: order`; a base url places over REST, which is
    // what this venue requires -- its websocket trade operation answers `50026` -- and the socket then carries only the
    // `orders` channel, which is where the state changes and the fills arrive.
    std::string rest_url;
    // The REST route's own budget, per endpoint. Only read when `rest_url` is set.
    RestLimits rest_limits{};
    // The venue's own name for the contract, sent only when it is known: a wrong guess would be an order on the wrong
    // instrument, so it is an operator's reading rather than something derived here.
    std::int64_t inst_code{-1};
    OrderMapping mapping{};
    // The first wait after a drop, doubling to eight seconds -- the schedule the Python relay's half uses.
    double backoff_seconds{1.0};
    // Close and reopen the venue connection after this many seconds of a session, for the reconnect test against a venue
    // that has no reason to drop one; 0 never drops. The market leg has the same hook (`--drop-after-seconds`) for the
    // same reason: the reconnect path is the one a healthy session never exercises.
    double drop_after_seconds{0.0};
    // Every frame both ways, one JSON object a line, appended. Empty disables it. A run that heard nothing cannot say
    // whether the venue was silent or this client was looking somewhere else, and the Python route's dump is what
    // explained the first such session; the login's key, passphrase and signature are masked on the way in.
    std::string dump_path;
};

struct WireCounters
{
    // Atomic because the two directions are counted on two threads: the engine's reader submits, and the venue's reader
    // writes the answers back.
    std::atomic<std::uint64_t> orders{0};
    std::atomic<std::uint64_t> cancels{0};
    // Lines written to the engine: the acknowledgements, fills and state changes the session decoded.
    std::atomic<std::uint64_t> replies{0};
    // Lines the translator refused before anything crossed, answered with `REJECT <id> venue` rather than dropped.
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> write_failures{0};
    // Connections that came up, which is what says a run reconnected rather than lasted.
    std::atomic<std::uint64_t> sessions{0};
    // The placement route's own counts, when it is REST: what the venue accepted, what it refused, and what never
    // reached it. Zero on the websocket route, where a placement has no separate answer.
    std::atomic<std::uint64_t> placed{0};
    std::atomic<std::uint64_t> venue_refusals{0};
    std::atomic<std::uint64_t> transport_failures{0};
    // The placement route's cost, counted rather than guessed: one placement is one connection today, so whether a
    // kept-alive connection is worth designing is answered by these three numbers first -- calls, the milliseconds spent
    // in connect plus TLS per call, and the whole call. Averaged in `to_log_string`.
    std::atomic<std::uint64_t> rest_calls{0};
    std::atomic<std::uint64_t> rest_connect_ms{0};
    std::atomic<std::uint64_t> rest_place_ms{0};
    // What the venue answered with HTTP 429. Counted beside the refusals rather than folded into them: a rate limit
    // is this client's budget, and a refusal is the venue's judgment of the order.
    std::atomic<std::uint64_t> rest_429{0};
    // The two queries the engine can ask over the same route: which orders the venue still holds, and what it says
    // the account is worth. `query_failures` is a query the venue did not answer, which is a different fact from a
    // query that answered "nothing".
    std::atomic<std::uint64_t> pending_queries{0};
    std::atomic<std::uint64_t> equity_queries{0};
    std::atomic<std::uint64_t> query_failures{0};

    WireCounters() = default;
    WireCounters(const WireCounters&) = delete;
    WireCounters& operator=(const WireCounters&) = delete;

    std::string to_log_string() const;
};

// One HTTP/1.1 answer, kept beside the placer because the placer is the only thing in the tree that reads one. The
// status is not interpreted as success or failure: this venue reports a refusal in its own `code`/`sCode`, and a proxy
// in front of it still carries that JSON in the body of a 4xx. 429 is the one status the placer does interpret, because
// it is about this client's budget rather than the order's.
struct HttpResponse
{
    int status{0};
    std::string body;
};

// The venue's placement surface when it is REST rather than a websocket operation, which the demo venue answers with
// `50026`. One HTTP/1.1 request per order over `venue_ws.h`'s stream, signed with `venue_auth::rest_signature` -- whose
// timestamp is ISO 8601 *milliseconds*, the opposite of the login's epoch seconds -- and the venue's answer turned into
// the same lines the websocket route produces: `ACK <id> <units>`, `CANCELED <id> user`, `REJECT <id> venue`.
//
// The body is the translator's own `args[0]`, so there is one translation and two transports. A transport failure is a
// `REJECT` rather than an exception: every submission is owed an answer, and the venue's own words are printed as they
// arrive rather than swallowed.
class RestPlacer
{
   public:
    RestPlacer(std::string base_url, OrderSession::Credentials credentials, RestLimits limits = {});

    std::vector<std::string> place(const OrderTranslator::Request& request, const OrderTranslator& translator);

    // The venue's own list of orders it still holds, in this client's ids (`clOrdId`, which the translator sets from
    // the engine's id). An empty vector is an answer -- nothing is working -- and `nullopt` is a failure, because the
    // two must not look alike to a recovery: one means every inherited order is gone, the other means nobody asked.
    std::optional<std::vector<std::uint64_t>> pending_orders();

    // What the venue says the account's USDT equity is (`details[ccy=USDT].eq`, `cashBal` when `eq` is empty), not
    // the account-wide `totalEq`: a USD-margined strategy's ledger trades one currency, and a demo account's spot
    // BTC/OKB would otherwise move this number with a market the strategy never touched. `nullopt` when the venue did
    // not answer or reported no USDT, and the caller prints `n/a` rather than zero.
    std::optional<double> balance_usd();

    std::uint64_t placed() const { return placed_; }
    std::uint64_t refused() const { return refused_; }
    std::uint64_t transport_failures() const { return failures_; }
    // What a placement cost: calls, the milliseconds inside connect plus TLS, and the whole call. Summed here and
    // averaged by the caller, so the count and the cost are stated by the same object.
    std::uint64_t calls() const { return calls_; }
    std::uint64_t connect_ms() const { return connect_ms_; }
    std::uint64_t place_ms() const { return place_ms_; }
    // HTTP 429 answers from the venue, including the ones a retry recovered from.
    std::uint64_t rate_limited() const { return rate_limited_; }

   private:
    std::vector<std::string> lines_for(std::string_view op, const std::string& order_id, const Json& answer,
                                       const OrderTranslator& translator, double sent_contracts);
    // One signed call, with the endpoint's budget waited for and a 429 retried. `method` is POST for the two order
    // endpoints and GET for the two queries, and the signature is over the method and the path as sent.
    HttpResponse request(std::string_view method, std::string_view path, std::string_view body,
                         std::string& failure, std::int64_t& connect_ms);
    // Blocks until this path has a token. Sleeps outside the lock, so two endpoints do not wait on each other.
    void acquire(std::string_view path);

    struct Bucket
    {
        double tokens{0.0};
        std::int64_t last_ms{0};
        bool started{false};
    };

    venue_ws::Url url_;
    OrderSession::Credentials credentials_;
    RestLimits limits_;
    std::mutex bucket_mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::uint64_t placed_{0};
    std::uint64_t refused_{0};
    std::uint64_t failures_{0};
    std::uint64_t calls_{0};
    std::uint64_t connect_ms_{0};
    std::uint64_t place_ms_{0};
    std::uint64_t rate_limited_{0};
};

class VenueOrderConnection : public OrderSession::Sink
{
   public:
    VenueOrderConnection(std::string instrument, OrderSession::Credentials credentials, WireConfig config,
                         EnginePort& engine, std::function<std::uint64_t()> clock = {});

    VenueOrderConnection(const VenueOrderConnection&) = delete;
    VenueOrderConnection& operator=(const VenueOrderConnection&) = delete;

    // Connects, logs in, subscribes, and reconnects until `stop`; blocks, so the caller owns the thread.
    void run();
    void stop();

    // A line from the engine, on whatever thread reads the engine's socket. Thread-safe: the translation and the venue
    // write it produces share the session with the reader on this object's own thread.
    void on_engine_line(std::string_view line);

    const OrderSession& session() const { return session_; }
    const WireCounters& counters() const { return counters_; }
    // Why the last session ended, for a summary a run can print. Empty when none has failed.
    std::string last_error() const;

    // OrderSession::Sink: the two directions have different peers, and one method could write a frame to the engine.
    void to_venue(std::string_view frame) override;
    void to_engine(std::string_view line) override;

   private:
    // A request written while there is no socket -- before the first login, or in the gap between two sessions -- waits
    // here instead of vanishing: an order the engine believes it sent and the venue never saw is exactly the failure
    // this queue exists to prevent. It is bounded, because a strategy that keeps submitting through a long outage must
    // not grow this process without limit, and a frame that could not be *written* is dropped rather than re-queued --
    // an order sent twice is worse than one nobody answered.
    struct Pending
    {
        std::string payload;
        int opcode{0x1};
    };
    static constexpr std::size_t kMaxPending = 1024;

    bool send(std::string_view payload, int opcode = 0x1);
    // One frame to the dump, whichever thread moved it, with the login's three credential fields replaced. A file that
    // exists to explain a silent run must not become the copy of the key that leaked.
    void dump_frame(std::string_view direction, std::string_view frame);
    // One connection, login to drop. It sets `up_` once the handshake has completed, which is what makes counting a drop
    // honest: a socket that never came up is a failed attempt, not a session that ended.
    void session_once();
    // A placement, on whichever surface this connection was configured with. Both routes answer the engine with the same
    // lines, which is why they are one method with a branch rather than two callers.
    void place(std::string_view line);
    // The two queries the engine can ask over this route. Both answer `unavailable` when the placement surface is the
    // socket rather than REST, because a simulated venue has neither list and a silence would be read as an empty one.
    void answer_pending();
    void answer_equity();
    // Copies the placer's own counters into the summary after any REST call, so the line a run prints and the object
    // that paid for the calls state the same numbers.
    void publish_rest_counters();

    WireConfig config_;
    OrderSession session_;
    EnginePort& engine_;
    std::function<std::uint64_t()> clock_;
    // Present only when the placement route is REST; the socket then carries the `orders` channel alone.
    std::unique_ptr<RestPlacer> placer_;
    std::mutex session_mutex_;
    std::mutex write_mutex_;
    venue_ws::Stream* stream_{nullptr};
    std::deque<Pending> pending_;
    std::atomic<bool> stop_{false};
    // Whether the attempt now running got as far as a connection. Read after it ends, which is why it is not a local.
    std::atomic<bool> up_{false};
    std::mutex wait_mutex_;
    std::condition_variable wait_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::mutex dump_mutex_;
    std::ofstream dump_;
    WireCounters counters_;
};
}  // namespace venue_order_wire
