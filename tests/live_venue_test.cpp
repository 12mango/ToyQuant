// `LiveVenue` is the adapter ADR 0005 describes: an `IMatchingEngine` whose orders leave over a wire. The wire
// is injected, so everything below runs with a recorder in its place -- no socket, no venue, no market data --
// which is what makes this the first step rather than the last.
//
// What it pins: the frame an order becomes, the two bounds the adapter enforces itself, a wire that refuses,
// and the four replies a venue can send mapped onto the reports the rest of the tree already understands.

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "lines/live_venue.h"

namespace
{
class RecordingTransport : public IVenueTransport
{
  public:
    bool send(std::string_view frame) override
    {
        if (fail_next)
        {
            fail_next = false;
            return false;
        }
        frames.emplace_back(frame);
        return true;
    }

    std::vector<std::string> frames;
    bool fail_next{false};
};

exchange::Order make_order(uint64_t id, exchange::Side side, double price, uint64_t qty,
                           TimeInForce tif = TimeInForce::Gtc)
{
    exchange::Order order;
    order.id = id;
    order.symbol = "BTC-PERPETUAL";
    order.side = side;
    order.type = exchange::OrderType::Limit;
    order.price = price;
    order.qty = qty;
    order.remaining = qty;
    order.ts = 1;
    order.time_in_force = tif;
    return order;
}
}  // namespace

int main()
{
    // An order becomes one frame, in the adapter's own protocol, and it is counted once.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        venue.send_order(make_order(7, exchange::Side::Buy, 6421.5, 10));
        assert(wire.frames.size() == 1);
        assert(wire.frames[0] == "ORDER 7 BTC-PERPETUAL B 6421.5 10 Gtc");
        assert(venue.counters().orders_sent == 1);
    }

    // The time in force travels with the order, because a venue that honours IOC and one that ignores it are
    // two different venues and the strategy's behaviour depends on which it got.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        venue.send_order(make_order(8, exchange::Side::Sell, 99.5, 3, TimeInForce::Ioc));
        assert(wire.frames.size() == 1);
        assert(wire.frames[0] == "ORDER 8 BTC-PERPETUAL S 99.5 3 Ioc");
    }

    // A cancel is one frame and its own counter: a cancel the venue never received and an order that was never
    // sent are different failures, and a single counter would hide one behind the other.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        venue.cancel_order(7);
        assert(wire.frames.size() == 1 && wire.frames[0] == "CANCEL 7");
        assert(venue.counters().cancels_sent == 1);
        assert(venue.counters().orders_sent == 0);
    }

    // The adapter's own order bound refuses before the wire, and it refuses silently: the point is that the
    // order never leaves this process.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.max_orders = 1});
        venue.send_order(make_order(1, exchange::Side::Buy, 10.0, 1));
        venue.send_order(make_order(2, exchange::Side::Buy, 10.0, 1));
        assert(wire.frames.size() == 1);
        assert(venue.counters().orders_sent == 1);
        assert(venue.counters().refused_by_adapter == 1);
    }

    // The size bound is separate from the count bound: a strategy that sends three orders of a thousand units
    // each is inside the first and outside this one.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.max_order_quantity = 100});
        venue.send_order(make_order(1, exchange::Side::Buy, 10.0, 100));
        venue.send_order(make_order(2, exchange::Side::Buy, 10.0, 101));
        assert(wire.frames.size() == 1);
        assert(venue.counters().orders_sent == 1);
        assert(venue.counters().refused_by_adapter == 1);
    }

    // The notional bound is the one a reader can price, and it is computed with the helper the ledger books with: a
    // thousand dollars of cap takes an order worth exactly that and refuses the next unit of size.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.max_order_notional_usd = 1000.0});
        venue.send_order(make_order(1, exchange::Side::Buy, 10.0, 100));
        venue.send_order(make_order(2, exchange::Side::Buy, 10.0, 101));
        assert(wire.frames.size() == 1);
        assert(venue.counters().orders_sent == 1);
        assert(venue.counters().refused_by_adapter == 1);
    }

    // ...and the engine's own scale is part of that arithmetic, which is what `price * qty` would have got wrong:
    // twenty million units of a one-millionth-scale instrument at a price of ten is 200 USD, not 200 million.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.max_order_notional_usd = 100.0, .quantity_scale = 1000000});
        venue.send_order(make_order(3, exchange::Side::Buy, 10.0, 20000000));
        assert(wire.frames.empty());
        assert(venue.counters().refused_by_adapter == 1);
    }

    // A wire that refuses is counted as a failure and not as a sent order. An adapter that reported success on
    // a dead wire would make the next reconciliation the first evidence that anything was wrong.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        wire.fail_next = true;
        venue.send_order(make_order(1, exchange::Side::Buy, 10.0, 1));
        assert(wire.frames.empty());
        assert(venue.counters().orders_sent == 0);
        assert(venue.counters().transport_failures == 1);
        assert(venue.counters().refused_by_adapter == 1);
    }

    // The four replies a venue can send, mapped onto the reports the pipeline already consumes.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });

        assert(venue.on_frame("ACK 7 10"));  // accepted and working, ten units still resting
        assert(venue.on_frame("FILL 7 6421.5 4"));
        assert(venue.on_frame("CANCELED 7 user"));
        assert(venue.on_frame("REJECT 9 venue"));
        assert(reports.size() == 4);

        // An acknowledgement is a resting order carrying what is left, not a fill.
        assert(reports[0].order_id == 7 && reports[0].exec_type == ExecType::Resting);
        assert(reports[0].quantity == 10 && reports[0].executed_quantity() == 0);

        // A fill is a trade: its quantity is the fill, not the remainder, and its price is the one that traded.
        assert(reports[1].exec_type == ExecType::Trade);
        assert(reports[1].price == 6421.5 && reports[1].quantity == 4);
        assert(reports[1].executed_quantity() == 4 && reports[1].remaining_quantity() == 0);

        // A cancel carries who asked for it, because a user cancel may be re-quoted at once and a venue cancel
        // deserves the same backoff as a rejection.
        assert(reports[2].exec_type == ExecType::Cancelled);
        assert(reports[2].cancel_reason == CancelReason::User);

        // A refusal is the venue's, which is what distinguishes it from a pre-trade control's refusal.
        assert(reports[3].exec_type == ExecType::Rejected);
        assert(reports[3].reject_reason == RejectReason::Venue);
        assert(venue.counters().reports_decoded == 4);
        assert(venue.counters().frames_dropped == 0);
    }

    // A frame the decoder does not understand is counted and reported as unhandled, not swallowed: a venue that
    // adds a field must not look like a venue that said nothing.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });

        assert(!venue.on_frame("HEARTBEAT 12"));
        assert(!venue.on_frame("FILL 7 6421.5"));
        assert(!venue.on_frame("ACK not-a-number 10"));
        assert(reports.empty());
        assert(venue.counters().frames_dropped == 3);
    }

    // The two calls the in-process engine uses to simulate a market are no-ops here, and the counters say so:
    // a live venue matches its own orders, so a book update cannot fill anything.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        BboQuote quote{};
        MarketTrade trade{};
        venue.process_bbo(quote);
        venue.process_market_trade(trade);
        assert(wire.frames.empty());
        assert(venue.counters().orders_sent == 0);
        assert(venue.counters().reports_decoded == 0);
    }

    // A report carries the owner its order was sent with, and only while the order lives. The pipeline books a
    // fill against its own owner and ignores every other, so an adapter that dropped the field reported every fill
    // and accounted for none of them -- which is exactly what the first app-level run did.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });

        exchange::Order order = make_order(11, exchange::Side::Buy, 10.0, 5);
        order.owner = "MarketMaker";
        venue.send_order(order);

        assert(venue.on_frame("FILL 11 10 5"));
        assert(reports.size() == 1 && reports[0].owner == "MarketMaker");
        assert(reports[0].side == exchange::Side::Buy && reports[0].liquidity_role == LiquidityRole::Maker);
        assert(venue.on_frame("CANCELED 11 user"));
        assert(reports.size() == 2 && reports[1].owner == "MarketMaker");
        // The order has ended, so its owner is forgotten: a frame for that id afterwards carries none, and the
        // side falls back to the report's default rather than being guessed from the frame.
        assert(venue.on_frame("FILL 11 10 1"));
        assert(reports.size() == 3 && reports[2].owner.empty());

        // A sell order's fill is a sell, and an immediate order's fill is a taker: both come from the sent order,
        // because the frame carries neither and the report's defaults are the wrong answer for both.
        exchange::Order sell = make_order(12, exchange::Side::Sell, 10.0, 4, TimeInForce::Ioc);
        sell.owner = "MarketMaker";
        venue.send_order(sell);
        assert(venue.on_frame("FILL 12 10 4"));
        assert(reports.size() == 4);
        assert(reports[3].side == exchange::Side::Sell && reports[3].liquidity_role == LiquidityRole::Taker);
    }

    // A report's timestamp is the market clock the run was on when it heard the answer, and without a clock it
    // stays zero. The stamp is what makes a session recorded through this adapter replayable: `RecordedVenue`
    // releases each answer once the replay's clock passes its timestamp, so zero means "every answer at the
    // first event", which is not the session that happened.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
        assert(venue.on_frame("ACK 7 10"));
        assert(reports.size() == 1 && reports[0].ts == 0);
    }
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
        uint64_t market_time = 1585699220749000;
        venue.set_report_clock([&market_time] { return market_time; });
        assert(venue.on_frame("ACK 7 10"));
        market_time += 9000;  // the next event, nine microseconds of market later
        assert(venue.on_frame("CANCELED 7 user"));
        assert(reports.size() == 2);
        assert(reports[0].ts == 1585699220749000);
        assert(reports[1].ts == 1585699220758000);
    }

    return 0;
}
