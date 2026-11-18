#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "exchange/execution_report.h"
#include "exchange/matching_engine.h"
#include "exchange/order.h"
#include "common/instrument_spec.h"

// The wire a venue is reached over. This is the seam ADR 0005 turns on: the adapter below is written
// against this interface, so the shape of "an order goes out and a report comes back" is proved by a test
// with a recorder in place of a socket, before any socket exists.
class IVenueTransport
{
  public:
    virtual ~IVenueTransport() = default;

    // False when the frame could not be written. The adapter counts that and does not pretend the order was
    // sent: a silent success on a dead wire is the one failure a demonstration cannot afford.
    virtual bool send(std::string_view frame) = 0;
};

struct LiveVenueConfig
{
    // The adapter's own bound, in orders. The pre-trade pipeline already caps notional, rate and size, and
    // those protect against a strategy's behaviour; this one protects against the adapter and the strategy
    // disagreeing about what was sent. 0 disables it, which is the configuration a test uses.
    uint64_t max_orders{0};
    // The same bound on one order's size, in the instrument's own quantity units. 0 disables it.
    uint64_t max_order_quantity{0};
    // The bound that can be priced: one order's USD notional, computed with `fill_notional_usd` -- the helper
    // the ledger books with -- so the number this refuses is the number the accounting would have recorded.
    // 0 disables it. A count cap says nothing about a strategy that sends few, large orders, and that is the
    // shape a size mistake takes.
    double max_order_notional_usd{0.0};
    // What that computation needs from the run's instrument: engine units per base unit, and the two fields
    // that turn a price into a USD value per unit. Defaults are the single-unit, price-quoted case.
    uint64_t quantity_scale{1};
    double unit_notional_usd{0.0};
    double multiplier{1.0};
    // How long `query_order` waits for the relay's answer to its `PENDING` request, in milliseconds. The query is
    // the one blocking call this adapter makes, and it exists for a session that has to ask the venue rather than
    // trust its own table: `--snapshot-in` on a live venue. 0 answers from what the channel has already said, which
    // is what a test uses and what a venue with no query endpoint gets.
    uint64_t query_timeout_ms{1500};
    // How many orders this adapter may place in any one second; 0, the default, is off. The session cap above bounds
    // what a whole run may place, which is the wrong shape for a strategy that replaces its quotes: a maker reaches
    // a twenty-order session cap in seconds and then spends the rest of the run being refused -- 383 quotes and 362
    // refusals in one 60 s demo session, whose exposure was whatever those twenty filled. The rate bounds the risk
    // that matters (orders per second) without ending the session's quoting. A close is exempt, like the session cap.
    uint64_t max_orders_per_second{0};
};

struct LiveVenueCounters
{
    uint64_t orders_sent{0};
    uint64_t cancels_sent{0};
    // Refused by this adapter rather than by the pipeline: a cap, or a transport that would not take it.
    uint64_t refused_by_adapter{0};
    // The subset of those the rate bound refused, kept apart because it says something different: the order was
    // well-formed and the venue reachable, it was too soon after the last one. A session cap exhausted says the run
    // is over; a rate refusal says the next window is.
    uint64_t rate_limited{0};
    uint64_t transport_failures{0};
    uint64_t reports_decoded{0};
    // An ending the adapter had already reported for that order. The relay answers a cancel over REST and the
    // private `orders` channel reports the same cancellation, so the engine heard one end twice; the second is
    // dropped here rather than counted as a state-machine fault downstream, where it would also release the
    // order's pending quantity a second time.
    uint64_t duplicate_reports{0};
    // A frame the decoder did not understand. Counted rather than ignored, because a venue's new field must
    // not look like a venue that said nothing.
    uint64_t frames_dropped{0};
    // The two queries a recovery and a reconciliation need. `queries` counts what was sent, `answers` what came
    // back, so a queue with no answer is a failure rather than a silence: `pending_failures` and `equity_failures`
    // are the runs that ended without the number they asked for.
    uint64_t pending_queries{0};
    uint64_t pending_answers{0};
    uint64_t pending_failures{0};
    uint64_t equity_queries{0};
    uint64_t equity_answers{0};
    uint64_t equity_failures{0};
};

// A venue on the other end of a wire. The same decorator shape as `PaperBroker` and `OrderGateway`: the
// pipeline knows only `IMatchingEngine`, so this class is the whole change, and it lives in `src/lines/`
// because it is the layer allowed to know a venue's shapes.
//
// The frames below are this adapter's own protocol, not a venue's JSON. Translating to a venue's message
// shapes is the relay's job -- the layer that already knows them -- so step two of ADR 0005 replaces the
// transport and leaves this file alone.
//
//     out:  ORDER <id> <symbol> <B|S> <price> <qty> <Gtc|Ioc|Fok>      CANCEL <id>
//           CLOSE <id> <symbol> <B|S> <qty>      PENDING               EQUITY
//     in:   ACK <id> <remaining>   FILL <id> <price> <qty> [<fee_usd>]
//           CANCELED <id> <reason>  REJECT <id> <reason>
//           PENDING <n> <id>...     PENDING unavailable
//           EQUITY <usd>            EQUITY unavailable
//
// `CLOSE` is the end-of-session exit on a live run: a market order with `reduceOnly`, whose units the relay rounds up
// to the venue's contract grid, so a fractional position (0.95 of a contract) is closed by one contract and the venue
// caps it at the position instead of flipping it. It carries no price; the venue's book decides the fill.
//
// The last four are the two questions a long-lived process cannot answer from its own table: which orders the
// venue still holds (`PENDING`, the venue's `orders-pending` list) and what the venue says the account is worth
// (`EQUITY`, its `totalEq`). A `PENDING` answer replaces the set rather than adding to it, so an order that filled
// while nobody was listening leaves it. `unavailable` is an answer and not a silence: the relay's placement
// surface is REST, and a simulated venue has neither list.
//
// The fee on a fill is the venue's own, in USD, positive when the venue took money: the relay reads it from the
// venue's `fillFee` and negates it. Absent, the report carries zero -- a venue that reports no fee is not one this
// adapter may charge for, and the in-process matcher is where a modelled fee comes from.
class LiveVenue : public IMatchingEngine
{
  public:
    explicit LiveVenue(IVenueTransport& transport, LiveVenueConfig config = {})
        : transport_(transport), config_(config)
    {
    }

    void send_order(const exchange::Order& order) override;
    void cancel_order(uint64_t order_id) override;

    // A live venue matches its own orders, so the two calls the in-process engine needs to simulate fills do
    // nothing here. That is the one place this interface mixes "be a market" with "be a venue", and ADR 0005
    // records it as the smell to watch rather than as a reason to widen the interface today.
    void process_bbo(const BboQuote& quote) override { (void)quote; }
    void process_market_trade(const MarketTrade& trade) override { (void)trade; }

    void set_report_callback(ReportCallback cb) override { callback_ = std::move(cb); }

    // A refusal before the wire emits no report, so without this count the pipeline cannot tell a refused order
    // from a resting one: the demo's 60 s session had 37 of its 58 orders refused by `--venue-max-orders`, the
    // strategy went on quoting against all of them, and the venue received a cancel for every one. The pipeline
    // watches this number across a submission and turns the change into a terminal rejection, which is what
    // `IMatchingEngine::rejected_orders` is for.
    uint64_t rejected_orders() const override { return counters_.refused_by_adapter; }

    // The clock a report is stamped with when the frame carries no time, which is every frame: a venue
    // answers on its own clock and the wire has no field for the run's. A session recorded through this
    // adapter needs the stamp, because `RecordedVenue` releases each answer once the replay's clock passes
    // the timestamp it was recorded with -- and a file whose answers are all due at zero is a file that
    // replays every one of them at the first event, which is not the session that happened. The
    // application points this at the pipeline's market clock; a test leaves it empty, and the timestamp
    // stays zero as it did before the stamp existed.
    void set_report_clock(std::function<uint64_t()> clock) { report_clock_ = std::move(clock); }

    // The adapter's own clock for the rate window, in microseconds. It is a second clock on purpose: the one above
    // is the market's, and this one measures how fast this adapter is placing. A test sets it instead of sleeping a
    // second, which is what makes the bound measurable rather than timing-dependent.
    void set_order_clock(std::function<uint64_t()> clock) { order_clock_ = std::move(clock); }

    // One frame from the venue. Returns false when it was not understood, so the caller can count it.
    bool on_frame(std::string_view frame);

    // Asks the venue which of this adapter's orders are still working, and waits for the answer up to
    // `config_.query_timeout_ms`. The answer is what `query_order` reads, and a failure leaves the previous
    // answer in place rather than replacing a list with nothing: a recovery that cannot ask must not conclude
    // that the venue holds no orders.
    bool refresh_pending();

    // The same for the account's equity, in USD. False when the venue did not answer; the value is then left as
    // it was, which is why a caller makes this call for its answer rather than for its return value alone.
    bool refresh_equity();
    std::optional<double> venue_equity_usd() const;

    // Where the wait pumps the transport: the receive thread queues a line rather than applying it, so a caller
    // that blocks has to hand the adapter a way to drain it. The application's pump applies only `PENDING`/`EQUITY`
    // answers and leaves reports queued for the pipeline's own drain, so a query asked before the pipeline exists
    // (a resume) cannot book a fill with no callback.
    void set_query_pump(std::function<void()> pump) { query_pump_ = std::move(pump); }

    // A query, not a report: it says whether the venue still holds the order. The answer comes from the last
    // complete `PENDING` answer, which `refresh_pending` replaces; this call never touches the wire, because a
    // caller may be inside the gateway's own walk over its in-flight table and a report arriving there would erase
    // from it. A recovery that has just sent a cancel calls `settle_queries()` before asking again.
    OrderQuery query_order(uint64_t order_id) const override;

    // A venue answers a cancel on its own connection, so a recovery that wants the new state has to wait for it.
    // The in-process engine has nothing to settle, which is why the interface's default is empty.
    void settle_queries() override { (void)refresh_pending(); }

    const LiveVenueCounters& counters() const { return counters_; }

  private:
    IVenueTransport& transport_;
    LiveVenueConfig config_;
    LiveVenueCounters counters_;
    ReportCallback callback_;
    // What the venue echoed back is only the order's id, so everything the accounting reads from a report has
    // to come from the order that was sent: its owner (the pipeline books a fill only for its own), its side (the
    // report's default is Buy, which would turn every sell fill into a buy), and its liquidity role (a Gtc order
    // rests and its fills are maker, an Ioc or Fok order is a taker by definition -- a venue whose Gtc orders can
    // cross on arrival would need the role on the wire, which is the field to add when a venue reports one).
    struct SentOrder
    {
        std::string owner;
        std::string symbol;
        exchange::Side side{exchange::Side::Buy};
        TimeInForce time_in_force{TimeInForce::Gtc};
        // Whether an ending has already been reported for this order. The entry itself is kept to the end of the
        // session -- a fill can follow the report that ended the order -- but the ending is only reported once.
        bool ended{false};
    };
    std::unordered_map<uint64_t, SentOrder> sent_;
    std::function<uint64_t()> report_clock_;
    // The times of the orders this adapter placed in the last second, for the rate bound. At most
    // `max_orders_per_second` entries, so it is bounded by the configuration rather than by the session.
    std::deque<uint64_t> recent_orders_;
    std::function<uint64_t()> order_clock_;
    // The venue's own working-order set, and whether it is a complete answer rather than what the channel has
    // happened to say. `complete_` is set only by a `PENDING` list; a submission or a cancel changes the set at the
    // venue, and the recovery refreshes it before the next question.
    mutable std::mutex query_mutex_;
    std::unordered_set<uint64_t> pending_;
    bool pending_complete_{false};
    // Whether the last `PENDING` answer was a list rather than `unavailable`: a refresh returns this, while
    // `query_order` keeps reading the last complete list, because "could not ask" must not mean "none working".
    bool pending_available_{false};
    // Each answer bumps its generation, so a wait can tell a fresh answer from the one it already had. `unavailable`
    // bumps it too: it is an answer, and the caller should stop waiting for one that will not come.
    uint64_t pending_generation_{0};
    uint64_t equity_generation_{0};
    std::optional<double> venue_equity_usd_;
    std::function<void()> query_pump_;
};
