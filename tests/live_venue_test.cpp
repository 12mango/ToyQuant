// `LiveVenue` is the adapter ADR 0005 describes: an `IMatchingEngine` whose orders leave over a wire. The wire
// is injected, so everything below runs with a recorder in its place -- no socket, no venue, no market data --
// which is what makes this the first step rather than the last.
//
// What it pins: the frame an order becomes, the bounds the adapter enforces itself (the session cap, the rate, the
// order size and the notional), a wire that refuses, the four replies a venue can send mapped onto the reports the rest
// of the tree already understands, and the two cases the live venue produced later: an ending it reports twice and a
// fill that arrives after the ending.

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

    // A reduce-only order is a close: the frame carries no price, because the venue's book decides the fill, and the
    // round-up to a whole contract and the `reduceOnly` flag are the relay's translation rather than this adapter's.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        exchange::Order close = make_order(11, exchange::Side::Sell, 0.0, 9500);
        close.reduce_only = true;
        venue.send_order(close);
        assert(wire.frames.size() == 1);
        assert(wire.frames[0] == "CLOSE 11 BTC-PERPETUAL S 9500");
        assert(venue.counters().orders_sent == 1);
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
        // A close is not refused by the count cap: it is the exit, and a refused exit leaves the position open.
        exchange::Order close = make_order(3, exchange::Side::Buy, 0.0, 10);
        close.reduce_only = true;
        venue.send_order(close);
        assert(wire.frames.size() == 2);
        assert(wire.frames.back() == "CLOSE 3 BTC-PERPETUAL B 10");
        assert(venue.counters().orders_sent == 2);
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

    // A refusal is told to the caller as well as counted. The pipeline compares `rejected_orders` across a
    // submission to tell a refused order from a working one, so an adapter that only incremented its own counter left
    // the strategy quoting against orders the venue never saw: the demo's 60 s session refused 37 of its 58 orders at
    // the cap, then sent the venue a cancel for every one of them.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.max_orders = 1});
        venue.send_order(make_order(60, exchange::Side::Buy, 10.0, 1));
        assert(venue.rejected_orders() == 0);
        venue.send_order(make_order(61, exchange::Side::Buy, 10.0, 1));
        assert(wire.frames.size() == 1);
        assert(venue.rejected_orders() == 1);
        // A dead wire refuses too: the order did not reach the venue either way.
        wire.fail_next = true;
        venue.send_order(make_order(62, exchange::Side::Buy, 10.0, 1));
        assert(venue.rejected_orders() == 2);
        assert(venue.counters().refused_by_adapter == 2);
    }

    // The rate bound, which is the shape of the risk a maker carries: the session cap bounds a whole run, so a
    // strategy that replaces its quotes meets it in seconds and is refused for the rest of the session -- 383 quotes
    // against a 20-order cap in one 60 s demo run. The window is one second, read from the adapter's own clock so the
    // bound is measurable rather than timing-dependent, and a close is exempt from it as it is from the session cap.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.max_orders_per_second = 2});
        uint64_t now_us = 1'000'000;
        venue.set_order_clock([&now_us] { return now_us; });
        venue.send_order(make_order(70, exchange::Side::Buy, 10.0, 1));
        venue.send_order(make_order(71, exchange::Side::Buy, 10.0, 1));
        assert(wire.frames.size() == 2);
        assert(venue.rejected_orders() == 0);
        venue.send_order(make_order(72, exchange::Side::Buy, 10.0, 1));
        assert(wire.frames.size() == 2);
        assert(venue.counters().rate_limited == 1);
        assert(venue.rejected_orders() == 1);
        // A second later the window has moved on and the next order goes out.
        now_us += 1'000'000;
        venue.send_order(make_order(73, exchange::Side::Buy, 10.0, 1));
        assert(wire.frames.size() == 3);
        assert(venue.counters().rate_limited == 1);
        // A close is the one order the bound must not throttle.
        exchange::Order close = make_order(74, exchange::Side::Sell, 10.0, 1);
        close.reduce_only = true;
        venue.send_order(close);
        assert(wire.frames.size() == 4);
        assert(venue.counters().rate_limited == 1);
        assert(venue.rejected_orders() == 1);
    }

    // The four replies a venue can send, mapped onto the reports the pipeline already consumes.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });

        // The relay's replies carry the id and nothing else, so the adapter's table has to hold the orders they are
        // about: a fill for an order it never sent is dropped (there is no side to book it in), and this block is the
        // four replies' mapping, so the two orders are sent first.
        venue.send_order(make_order(7, exchange::Side::Buy, 6421.5, 10));
        venue.send_order(make_order(9, exchange::Side::Buy, 6421.5, 10));
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
        // The fill, then the ending: the order was five units and the fill was all of it. The owner, the symbol, the
        // side and the role are the sent order's, not the frame's -- an adapter that dropped the owner booked no fill
        // at all, which is what the first app-level run did.
        assert(reports.size() == 2);
        assert(reports[0].owner == "MarketMaker");
        // The symbol is the position's key: a report without it books the fill under the empty string, where no mid
        // is ever found, and the account reports its cash as equity while the position is worth nothing.
        assert(reports[0].symbol == "BTC-PERPETUAL");
        assert(reports[0].side == exchange::Side::Buy && reports[0].liquidity_role == LiquidityRole::Maker);
        assert(reports[1].exec_type == ExecType::Filled);
        // The entry survives the ending: the venue fills what it can at the moment it cancels the remainder, and that
        // report still belongs to this order -- its owner, side and symbol are the order's, not the report's defaults.
        // A cancel after the ending is a duplicate, so the late fill is the third report rather than the second.
        assert(venue.on_frame("CANCELED 11 user"));
        assert(reports.size() == 2);
        assert(venue.counters().duplicate_reports == 1);
        assert(venue.on_frame("FILL 11 10 1"));
        assert(reports.size() == 3 && reports[2].owner == "MarketMaker");
        assert(reports[2].side == exchange::Side::Buy && reports[2].exec_type == ExecType::Trade);

        // A sell order's fill is a sell, and an immediate order's fill is a taker: both come from the sent order,
        // because the frame carries neither and the report's defaults are the wrong answer for both.
        exchange::Order sell = make_order(12, exchange::Side::Sell, 10.0, 4, TimeInForce::Ioc);
        sell.owner = "MarketMaker";
        venue.send_order(sell);
        assert(venue.on_frame("FILL 12 10 4"));
        assert(reports.size() == 5);
        assert(reports[3].side == exchange::Side::Sell && reports[3].liquidity_role == LiquidityRole::Taker);
        assert(reports[4].exec_type == ExecType::Filled);
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

    // The venue's own fee rides on a fill's frame when it reports one, and a frame without it leaves the report at
    // zero: this adapter charges nothing of its own, because a fee it invented would be a number the venue never
    // billed. A fee field the parser cannot read drops the frame rather than booking a zero.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
        venue.send_order(make_order(7, exchange::Side::Buy, 6421.5, 4));
        venue.send_order(make_order(8, exchange::Side::Buy, 6421.5, 4));
        venue.send_order(make_order(9, exchange::Side::Buy, 6421.5, 4));
        venue.send_order(make_order(10, exchange::Side::Buy, 6421.5, 4));
        assert(venue.on_frame("FILL 7 6421.5 4 0.02"));
        assert(venue.on_frame("FILL 8 6421.5 4 -0.01"));
        assert(venue.on_frame("FILL 9 6421.5 4"));
        // Three fills and three endings: each order was four units and its fill completed it.
        assert(reports.size() == 6);
        assert(reports[0].fee == 0.02);
        assert(reports[2].fee == -0.01);
        assert(reports[4].fee == 0.0);
        assert(reports[1].exec_type == ExecType::Filled && reports[3].exec_type == ExecType::Filled &&
               reports[5].exec_type == ExecType::Filled);
        assert(!venue.on_frame("FILL 10 6421.5 4 not-a-fee"));
        assert(venue.counters().frames_dropped == 1);
    }

    // A fill can arrive after the cancel that raced it: the venue fills what it can at the moment it cancels the
    // remainder, and that report still has to be booked in the order's own side. The entry is kept through the
    // cancel for exactly this, and a fill for an order the adapter never sent is dropped rather than booked with the
    // report's defaults -- which is how a short venue position was booked as a long one.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
        venue.send_order(make_order(40, exchange::Side::Sell, 6421.5, 10));
        assert(venue.on_frame("CANCELED 40 user"));
        assert(venue.on_frame("FILL 40 6421.5 10"));
        // A cancel the venue refuses because the order filled (`51400`) arrives as `REJECT`, and the fill follows it:
        // the entry survives that too, or the fill is dropped and the position misses what the venue executed.
        venue.send_order(make_order(42, exchange::Side::Buy, 6421.5, 10));
        assert(venue.on_frame("REJECT 42 venue"));
        assert(venue.on_frame("FILL 42 6421.5 10"));
        assert(reports.size() == 4);
        assert(reports[1].exec_type == ExecType::Trade);
        assert(reports[1].side == exchange::Side::Sell);
        assert(reports[1].symbol == "BTC-PERPETUAL");
        assert(reports[3].exec_type == ExecType::Trade);
        assert(reports[3].side == exchange::Side::Buy);
        assert(!venue.on_frame("FILL 41 6421.5 10"));
        assert(venue.counters().frames_dropped == 1);
    }

    // One ending, reported twice. The relay answers a cancel on its REST path and the venue reports the same
    // cancellation on the private `orders` channel, so the adapter received two terminal frames for one order: the
    // ledger counted the second as a report for an order it had already closed (13 of them in one 60 s demo session)
    // and released the order's pending quantity a second time. The first ending is forwarded, the second is dropped
    // and counted, and a fill after the ending is still forwarded -- that is the order's quantity, not another end.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
        venue.send_order(make_order(50, exchange::Side::Sell, 6421.5, 10));
        assert(venue.on_frame("CANCELED 50 user"));
        assert(venue.on_frame("CANCELED 50 venue"));
        assert(reports.size() == 1);
        assert(venue.counters().duplicate_reports == 1);
        assert(venue.on_frame("FILL 50 6421.5 10"));
        assert(reports.size() == 2 && reports[1].exec_type == ExecType::Trade);
        assert(venue.counters().duplicate_reports == 1);

        // A terminal report for an id this adapter never sent is still forwarded: the pipeline is what has to say
        // it could not attribute it, and the adapter has no order to compare it with.
        assert(venue.on_frame("CANCELED 51 user"));
        assert(reports.size() == 3);
        assert(venue.counters().duplicate_reports == 1);
    }

    // A fill that completes the order ends it, because neither relay maps the venue's `state: filled` to a line: every
    // live run read `audited_filled_orders=0`, and the only thing that closed a filled order was a cancel the venue
    // refused. The adapter knows the size it sent, so the fills reaching it is the fact -- the fill first, then the
    // ending, so the account has the quantity before the audit closes.
    {
        RecordingTransport wire;
        LiveVenue venue(wire);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
        venue.send_order(make_order(80, exchange::Side::Buy, 10.0, 10));
        assert(venue.on_frame("FILL 80 10 4"));
        assert(reports.size() == 1 && reports[0].exec_type == ExecType::Trade);
        assert(venue.on_frame("FILL 80 10 6"));
        assert(reports.size() == 3);
        assert(reports[1].exec_type == ExecType::Trade);
        assert(reports[2].exec_type == ExecType::Filled);
        // A `Filled` report carries the order's remaining quantity, which is none, so the account books nothing twice.
        assert(reports[2].quantity == 0 && reports[2].executed_quantity() == 0);
        assert(reports[2].side == exchange::Side::Buy && reports[2].symbol == "BTC-PERPETUAL");
        // The ending is reported once: the venue's answer to a later cancel is a duplicate, not a second end.
        assert(venue.on_frame("REJECT 80 venue"));
        assert(reports.size() == 3);
        assert(venue.counters().duplicate_reports == 1);
        // A fill the venue rounds past the size sent is clamped, and a fill after the end is still forwarded -- that
        // is the order's quantity, not another ending.
        venue.send_order(make_order(81, exchange::Side::Sell, 10.0, 10));
        assert(venue.on_frame("FILL 81 10 12"));
        assert(reports.size() == 5 && reports[4].exec_type == ExecType::Filled);
        assert(venue.on_frame("FILL 81 10 2"));
        assert(reports.size() == 6 && reports[5].exec_type == ExecType::Trade);
        // A fill that does not complete the order leaves it open, and the cancel that ends it still does.
        venue.send_order(make_order(82, exchange::Side::Buy, 10.0, 10));
        assert(venue.on_frame("FILL 82 10 4"));
        assert(venue.on_frame("CANCELED 82 user"));
        assert(reports.size() == 8 && reports[7].exec_type == ExecType::Cancelled);
    }

    // The venue's own working-order set: `PENDING` replaces it, an acknowledgement adds to it, a cancel marks it stale
    // and an end-of-order removes one. `query_order` reads that set, and the second query after a cancel is a question
    // about the venue now -- which is what `[RESUME]`'s `cancelled` count needs on a live venue.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.query_timeout_ms = 0});
        // No snapshot yet: nobody has asked the venue, so the answer is that it has not said -- not that the order is
        // gone. Asking does not go to the wire either; a caller may be inside the gateway's own walk over its table.
        assert(venue.query_order(7) == OrderQuery::Unanswered);
        assert(wire.frames.empty());
        assert(venue.on_frame("PENDING 2 7 9"));
        assert(venue.counters().pending_answers == 1);
        assert(venue.query_order(7) == OrderQuery::Working);
        assert(venue.query_order(8) == OrderQuery::Unknown);
        // A cancel changes the set at the venue, and only a new query can confirm it: until then the last complete
        // list is what `query_order` has.
        venue.cancel_order(7);
        assert(wire.frames.back() == "CANCEL 7");
        assert(venue.query_order(7) == OrderQuery::Working);
        venue.settle_queries();
        assert(wire.frames.back() == "PENDING");
        assert(venue.on_frame("PENDING 1 9"));
        // The venue's own end-of-order removes it without waiting for a query.
        assert(venue.on_frame("CANCELED 7 user"));
        assert(venue.on_frame("PENDING 1 9"));
        assert(venue.query_order(7) == OrderQuery::Unknown);
        assert(venue.query_order(9) == OrderQuery::Working);
    }

    // An answer that cannot be given is not an empty set, and a malformed query answer is dropped rather than read as
    // one: `PENDING unavailable` leaves the previous list in place and marks it stale -- and the staleness is what
    // `query_order` reports, so a recovery refuses to start instead of counting every inherited order as gone.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.query_timeout_ms = 0});
        assert(venue.on_frame("PENDING 1 7"));
        assert(venue.query_order(7) == OrderQuery::Working);
        assert(venue.on_frame("PENDING unavailable"));
        assert(venue.query_order(7) == OrderQuery::Unanswered);
        // A complete list is what clears it, and only a list: the dropped frames below are not answers.
        assert(!venue.on_frame("PENDING 3 7"));
        assert(!venue.on_frame("PENDING 1 not-a-number"));
        assert(venue.query_order(7) == OrderQuery::Unanswered);
        assert(venue.counters().frames_dropped == 2);
        assert(venue.on_frame("PENDING 1 7"));
        assert(venue.query_order(7) == OrderQuery::Working);
        assert(venue.on_frame("EQUITY unavailable"));
        assert(!venue.venue_equity_usd().has_value());
        assert(venue.on_frame("EQUITY 998.5"));
        assert(venue.venue_equity_usd().has_value() && *venue.venue_equity_usd() == 998.5);
        assert(venue.counters().equity_answers == 1);
        // The position answer is signed, because a short is the ledger's negative quantity, and `unavailable` clears it
        // rather than leaving an older reading in place. The four-field shape states what the number is counted in --
        // engine units per coin and the coin -- which is what lets the reconciliation refuse a comparison across two
        // conventions; the two-field shape is an older relay and leaves the unit unstated rather than assumed.
        assert(venue.on_frame("POSITION -900"));
        assert(venue.venue_position().has_value() && *venue.venue_position() == -900);
        assert(!venue.venue_units_per_coin() && venue.venue_coin().empty());
        assert(venue.counters().position_answers == 1);
        assert(venue.on_frame("POSITION 10000 1000000 BTC"));
        assert(venue.venue_position().has_value() && *venue.venue_position() == 10000);
        assert(venue.venue_units_per_coin() == 1000000 && venue.venue_coin() == "BTC");
        assert(venue.counters().position_answers == 2);
        // A stated unit has to be usable: zero units per coin, or no coin, is dropped rather than believed.
        assert(!venue.on_frame("POSITION 10000 0 BTC"));
        assert(!venue.on_frame("POSITION 10000 1000000"));
        assert(!venue.on_frame("POSITION 10000 1000000 BTC extra"));
        assert(venue.counters().frames_dropped == 5);
        assert(venue.on_frame("POSITION unavailable"));
        assert(!venue.venue_position().has_value());
        assert(!venue.venue_units_per_coin() && venue.venue_coin().empty());
        assert(!venue.on_frame("POSITION not-a-number"));
        assert(venue.counters().frames_dropped == 6);
        // The balance and position answers are order-less frames: they must not become reports about an order.
        assert(venue.counters().reports_decoded == 0);
    }

    // A lost equity request is retried inside the budget: a run asks at the start and at the end, and the end's one
    // answer is the whole comparison, so a single send is not enough evidence that the venue cannot answer. An answer
    // that says `unavailable` is not retried -- that one is the venue's word, not a lost frame.
    {
        RecordingTransport wire;
        LiveVenue venue(wire, LiveVenueConfig{.query_timeout_ms = 40});
        venue.set_query_pump([&venue, &wire]
        {
            if (wire.frames.size() < 2 || venue.venue_equity_usd().has_value()) return;
            venue.on_frame("EQUITY 998.5");
        });
        assert(venue.refresh_equity());
        assert(venue.venue_equity_usd().has_value() && *venue.venue_equity_usd() == 998.5);
        assert(wire.frames.size() == 2);
        assert(wire.frames[0] == "EQUITY" && wire.frames[1] == "EQUITY");
        assert(venue.counters().equity_queries == 1);
        assert(venue.counters().equity_failures == 0);
    }

    return 0;
}
