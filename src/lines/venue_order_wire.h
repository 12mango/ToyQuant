#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "lines/venue_order_session.h"
#include "lines/venue_order_translator.h"

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

struct WireConfig
{
    std::string venue_url{kDemoVenueUrl};
    // The venue's own name for the contract, sent only when it is known: a wrong guess would be an order on the wrong
    // instrument, so it is an operator's reading rather than something derived here.
    std::int64_t inst_code{-1};
    OrderMapping mapping{};
    // The first wait after a drop, doubling to eight seconds -- the schedule the Python relay's half uses.
    double backoff_seconds{1.0};
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

    WireCounters() = default;
    WireCounters(const WireCounters&) = delete;
    WireCounters& operator=(const WireCounters&) = delete;

    std::string to_log_string() const;
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
    // One connection, login to drop. It sets `up_` once the handshake has completed, which is what makes counting a drop
    // honest: a socket that never came up is a failed attempt, not a session that ended.
    void session_once();

    WireConfig config_;
    OrderSession session_;
    EnginePort& engine_;
    std::function<std::uint64_t()> clock_;
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
    WireCounters counters_;
};
}  // namespace venue_order_wire
