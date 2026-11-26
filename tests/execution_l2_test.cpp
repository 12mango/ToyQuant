#include <cassert>
#include <cmath>
#include <cstdint>

#include "strategy/execution/execution_l2.h"

namespace
{
L2MarketView view_at(uint64_t ts, double mid, uint64_t size = 1000)
{
    L2MarketView view;
    view.symbol = "X";
    view.ts = ts;
    view.top = TopOfBook{mid - 0.5, size, mid + 0.5, size};
    return view;
}

ExecutionL2Config parent(ExecutionAlgo algo, uint64_t quantity = 100, uint64_t horizon_s = 10)
{
    return ExecutionL2Config{.side = Side::Buy,
                             .quantity = quantity,
                             .horizon_us = horizon_s * 1'000'000,
                             .algo = algo,
                             .slices = 10,
                             .participation = 0.1};
}

// The pipeline assigns the id before it tells the strategy the order exists, and reports carry it back. The
// tests have to do the same, because a strategy that keyed its working child on 0 would mistake "no child" for
// "the child it just sent".
StrategyOrder identified(StrategyOrder order, uint64_t id)
{
    order.order_id = id;
    return order;
}

// Deliver what the pipeline would: the order is submitted, its fill is reported, and the terminal report that
// closes it follows. Without the terminal report the child would still look like it is working.
void fill(ExecutionL2Strategy& strategy, const StrategyOrder& order)
{
    strategy.on_order_submitted(order);
    ExecutionReport report;
    report.order_id = order.order_id;
    report.exec_type = ExecType::Trade;
    report.side = order.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell;
    report.price = order.price;
    report.quantity = order.quantity;
    strategy.on_order_update(report);
    report.exec_type = ExecType::Filled;
    report.quantity = 0;
    strategy.on_order_update(report);
}
}  // namespace

int main()
{
    // Market: one child for the whole parent, at the offer, with a time in force that cancels the remainder.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Market));
        const auto first = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(first.size() == 1);
        assert(first[0].side == Side::Buy);
        assert(first[0].price == 100.5);
        assert(first[0].quantity == 100);
        assert(first[0].time_in_force == TimeInForce::Ioc);
        fill(strategy, identified(first[0], 1));
        // Nothing left to do: the parent is complete.
        assert(strategy.on_l2_market_view(view_at(2'000'000, 100.0)).empty());
        assert(strategy.filled_quantity() == 100);
    }

    // Twap: ten children of ten across a ten-second horizon, and none after the deadline.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Twap));
        uint64_t children = 0;
        uint64_t total = 0;
        uint64_t next_id = 0;
        for (uint64_t second = 1; second <= 14; ++second)
        {
            for (const auto& order : strategy.on_l2_market_view(view_at(second * 1'000'000, 100.0)))
            {
                ++children;
                assert(order.quantity == 10);
                total += order.quantity;
                fill(strategy, identified(order, ++next_id));
            }
        }
        assert(children == 10);
        assert(total == 100);
        assert(strategy.child_orders() == 10);
    }

    // Pov: sized to a share of the volume that has traded since the parent started, not to a clock.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Pov));
        assert(strategy.on_l2_market_view(view_at(1'000'000, 100.0)).empty());
        // 1,000 contracts print, so 10% is 100: one child for the whole parent.
        strategy.on_market_trade(MarketTrade{1'100'000, "X", 100.0, 1000, Side::Sell, 1, "x"});
        const auto orders = strategy.on_l2_market_view(view_at(1'200'000, 100.0));
        assert(orders.size() == 1);
        assert(orders[0].quantity == 100);
    }

    // The view clock decides what a second is worth. An OKX book cut prints milliseconds (`1794834900007`), so a
    // ten-second horizon on ten slices is one child per thousand milliseconds; the same parent on a microsecond clock
    // is one child per million. Before this the horizon was compared against the raw clock and a two-hour parent sent
    // exactly one child (`child_orders=1 span_seconds=0`, measured 2026-11-26), because 7,200 s of microseconds read
    // as 8.3 days of milliseconds.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Twap));
        const uint64_t start_ms = 1'794'834'900'007ULL;  // the OKX cut's own first timestamp
        uint64_t children = 0;
        uint64_t next_id = 0;
        for (uint64_t ms = 0; ms <= 10'000; ms += 250)
        {
            for (const auto& order : strategy.on_l2_market_view(view_at(start_ms + ms, 100.0)))
            {
                ++children;
                fill(strategy, identified(order, ++next_id));
            }
        }
        assert(children == 10);
        assert(strategy.child_orders() == 10);
        assert(strategy.filled_quantity() == 100);
    }

    // A crossing child blocks the next child until its own report arrives, and is not pulled meanwhile: an IOC either
    // fills or dies at the venue, so a pull-and-resend while it is in flight double-submits the slice. This block used
    // to assert the opposite -- "pulled on the next view" -- and that rule executed 5.1 times the parent in the first
    // replay (51 children for a ten-slice schedule at a 10 ms order latency), which is why the assertion moved.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Market));
        const auto first = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(first.size() == 1);
        const auto sent = identified(first[0], 7);
        strategy.on_order_submitted(sent);
        assert(strategy.cancel_requests().empty());
        // The next view finds the child still working: nothing is sent and nothing is pulled.
        assert(strategy.on_l2_market_view(view_at(2'000'000, 100.0)).empty());
        assert(strategy.cancel_requests().empty());
        // Its own report releases the slot, and because the parent is then complete no further child appears.
        ExecutionReport trade;
        trade.order_id = sent.order_id;
        trade.exec_type = ExecType::Trade;
        trade.side = exchange::Side::Sell;
        trade.price = sent.price;
        trade.quantity = sent.quantity;
        strategy.on_order_update(trade);
        ExecutionReport done;
        done.order_id = sent.order_id;
        done.exec_type = ExecType::Filled;
        strategy.on_order_update(done);
        assert(strategy.on_l2_market_view(view_at(3'000'000, 100.0)).empty());
        assert(strategy.child_orders() == 1);
        assert(strategy.filled_quantity() == 100);
    }

    // Post-first: the child joins the near touch and waits. When its slice's timeout passes it is pulled, and
    // what the slice still owes is crossed on the same view rather than waiting for the next slice.
    {
        auto config = parent(ExecutionAlgo::Market);
        config.post_first = true;
        config.post_timeout_us = 5'000'000;
        ExecutionL2Strategy strategy(config);
        const auto posted = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(posted.size() == 1);
        assert(posted[0].price == 99.5);
        assert(posted[0].time_in_force == TimeInForce::Gtc);
        const auto sent_post = identified(posted[0], 11);
        strategy.on_order_submitted(sent_post);

        // Three seconds in, the child is still waiting: nothing new is sent.
        assert(strategy.on_l2_market_view(view_at(4'000'000, 100.0)).empty());
        assert(strategy.posted_children() == 1);

        // Past the timeout the child is pulled and the remainder crosses in the same view.
        const auto fallback = strategy.on_l2_market_view(view_at(7'000'000, 100.0));
        assert(fallback.size() == 1);
        assert(fallback[0].price == 100.5);
        assert(fallback[0].time_in_force == TimeInForce::Ioc);
        assert(fallback[0].quantity == 100);
        const auto pulled = strategy.cancel_requests();
        assert(pulled.size() == 1);
        assert(pulled[0] == sent_post.order_id);
        assert(strategy.crossed_children() == 0);
    }

    // A posted child that fills part of its slice hands only the rest to the crossing fallback.
    {
        auto config = parent(ExecutionAlgo::Market);
        config.post_first = true;
        config.post_timeout_us = 5'000'000;
        ExecutionL2Strategy strategy(config);
        const auto posted = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(posted.size() == 1);
        const auto sent_post = identified(posted[0], 21);
        strategy.on_order_submitted(sent_post);
        ExecutionReport partial;
        partial.order_id = sent_post.order_id;
        partial.exec_type = ExecType::Trade;
        partial.side = exchange::Side::Buy;
        partial.price = sent_post.price;
        partial.quantity = 40;
        strategy.on_order_update(partial);
        assert(strategy.filled_quantity() == 40);

        const auto fallback = strategy.on_l2_market_view(view_at(7'000'000, 100.0));
        assert(fallback.size() == 1);
        assert(fallback[0].quantity == 60);
    }

    // The follow, which is what `--amend-instead-of-cancel` means on this line: a resting child moves with the near
    // touch in place, for its whole quantity, and only once the touch has moved at least the deadband. An engine with
    // an amend takes the request in place; one without receives the cancel it replaced, and the slice is then posted
    // again at the new price rather than crossed -- an amend is not a deadline, so turning it into a taker fill would
    // price the follow as the timeout it is not.
    {
        auto config = parent(ExecutionAlgo::Market);
        config.post_first = true;
        config.post_timeout_us = 60'000'000;
        config.amend_instead_of_cancel = true;
        config.amend_deadband_ticks = 1.0;
        config.tick_size = 0.5;
        ExecutionL2Strategy strategy(config);
        const auto posted = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(posted.size() == 1);
        assert(posted[0].price == 99.5);  // joins the bid
        assert(posted[0].time_in_force == TimeInForce::Gtc);
        const auto child = identified(posted[0], 31);
        strategy.on_order_submitted(child);
        assert(strategy.amend_requests().empty());

        // A move inside the deadband is not a move: half a tick leaves the child where it is.
        assert(strategy.on_l2_market_view(view_at(2'000'000, 100.25)).empty());
        assert(strategy.amend_requests().empty());
        assert(strategy.amend_count() == 0);

        // The bid rises one tick: the child follows it in place, carrying the quantity it was sent for.
        assert(strategy.on_l2_market_view(view_at(3'000'000, 100.5)).empty());
        const auto amends = strategy.amend_requests();
        assert(amends.size() == 1);
        assert(amends[0].order_id == child.order_id);
        assert(std::abs(amends[0].price - 100.0) < 1e-9);
        assert(amends[0].quantity == 100);
        assert(strategy.amend_count() == 1);
        // The same view asked once: the local record took the new price, so the next view is quiet.
        assert(strategy.on_l2_market_view(view_at(4'000'000, 100.5)).empty());
        assert(strategy.amend_requests().empty());
        assert(strategy.amend_count() == 1);

        // An engine without an amend answers the request with a cancel. The child ends, and the slice goes back out
        // as a post at the new price, not as an IOC.
        ExecutionReport pulled;
        pulled.order_id = child.order_id;
        pulled.exec_type = ExecType::Cancelled;
        pulled.side = exchange::Side::Buy;
        pulled.price = 100.0;
        pulled.quantity = 100;
        strategy.on_order_update(pulled);
        const auto reposted = strategy.on_l2_market_view(view_at(5'000'000, 100.5));
        assert(reposted.size() == 1);
        assert(reposted[0].price == 100.0);
        assert(reposted[0].quantity == 100);
        assert(reposted[0].time_in_force == TimeInForce::Gtc);
        assert(strategy.crossed_children() == 0);
    }

    // Off, the same move is nothing at all: the child waits at the price it joined, which is the arm every recorded
    // run before the flag took.
    {
        auto config = parent(ExecutionAlgo::Market);
        config.post_first = true;
        config.post_timeout_us = 60'000'000;
        config.tick_size = 0.5;
        ExecutionL2Strategy strategy(config);
        const auto posted = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(posted.size() == 1);
        strategy.on_order_submitted(identified(posted[0], 41));
        assert(strategy.on_l2_market_view(view_at(3'000'000, 100.5)).empty());
        assert(strategy.amend_requests().empty());
        assert(strategy.amend_count() == 0);
    }

    // Without a parent the strategy is inert rather than guessing one.
    {
        ExecutionL2Strategy strategy(ExecutionL2Config{});
        assert(strategy.on_l2_market_view(view_at(1'000'000, 100.0)).empty());
    }
}
