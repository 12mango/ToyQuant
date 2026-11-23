// The non-market-making strategy has one contract worth checking: a signal sets a target position, the
// target is reached in clips at the touch, and the two risk exits (a stop and a time stop) take priority
// over a signal that is still pointing the other way. The test drives the strategy directly; the pipeline
// assigns order ids in a run, so the harness does the same here.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "strategy/trend_l2.h"

namespace
{
L2MarketView view_at(double mid)
{
    L2MarketView view;
    view.symbol = "TEST";
    view.top = TopOfBook{mid - 0.01, 100, mid + 0.01, 100};
    view.micro_price = mid;
    return view;
}

// A view with the spread chosen by the test, for the entries that are priced inside it.
L2MarketView view_between(double bid, double ask)
{
    L2MarketView view;
    view.symbol = "TEST";
    view.top = TopOfBook{bid, 100, ask, 100};
    view.micro_price = (bid + ask) / 2.0;
    return view;
}

struct Harness
{
    explicit Harness(TrendL2Config config) : strategy(config) {}

    std::vector<StrategyOrder> on(double mid)
    {
        auto orders = strategy.on_l2_market_view(view_at(mid));
        for (StrategyOrder& order : orders)
        {
            order.order_id = next_id++;
            strategy.on_order_submitted(order);
        }
        return orders;
    }

    std::vector<StrategyOrder> on_view(const L2MarketView& view)
    {
        auto orders = strategy.on_l2_market_view(view);
        for (StrategyOrder& order : orders)
        {
            order.order_id = next_id++;
            strategy.on_order_submitted(order);
        }
        return orders;
    }

    void fill(const StrategyOrder& order, uint64_t quantity)
    {
        strategy.on_order_update(ExecutionReport{
            .order_id = order.order_id,
            .side = order.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell,
            .exec_type = ExecType::Trade,
            .symbol = order.symbol,
            .price = order.price,
            .quantity = quantity,
            .ts = 0,
            .owner = "MarketMaker"});
    }

    TrendL2Strategy strategy;
    uint64_t next_id{1};
};
}  // namespace

int main()
{
    {
        // A rising mid: warm up, then three clips take the position to the target, and a falling mid
        // through the exit band sends the first reducing clip.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 30,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100});

        for (int index = 0; index < 4; ++index) assert(h.on(100.00).empty());

        auto orders = h.on(100.05);
        assert(orders.size() == 1);
        assert(orders[0].side == Side::Buy);
        assert(orders[0].quantity == 10);
        assert(std::abs(orders[0].price - 100.06) < 1e-9);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 10);

        orders = h.on(100.10);
        assert(orders.size() == 1);
        h.fill(orders[0], 10);
        orders = h.on(100.15);
        assert(orders.size() == 1);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 30);
        assert(h.strategy.target_position() == 30);

        // At the target there is nothing to do.
        assert(h.on(100.20).empty());

        // The gap falls inside the exit band: the target is flat and the first reducing clip goes out.
        orders = h.on(100.00);
        assert(orders.size() == 1);
        assert(orders[0].side == Side::Sell);
        assert(orders[0].quantity == 10);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 20);
    }
    {
        // A stop takes priority over the signal: a long entered at 100.11 is exited when the mid reaches
        // 100.04, five ticks against it, and the risk pause is counted.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 3});

        for (int index = 0; index < 4; ++index) h.on(100.00);
        auto orders = h.on(100.10);
        assert(orders.size() == 1);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 10);

        orders = h.on(100.04);
        assert(orders.size() == 1);
        assert(orders[0].side == Side::Sell);
        assert(h.strategy.metrics().risk_pause_count >= 1);

        // The exit fills, and the cooldown then refuses to reopen the same position for three views
        // even though the signal is rising again.
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 0);
        for (int index = 0; index < 3; ++index) assert(h.on(100.30).empty());
        assert(!h.on(100.30).empty());
    }
    {
        // A refused entry is not retried on the next view: the wait starts at two views, doubles on each
        // consecutive refusal, and a successful submission resets it.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 0,
                                .rejection_backoff_views = 2,
                                .max_rejection_backoff_views = 8});
        for (int index = 0; index < 4; ++index) h.on(100.00);

        // The pipeline would refuse this order instead of submitting it, so it is not in open_orders.
        auto orders = h.strategy.on_l2_market_view(view_at(100.10));
        assert(orders.size() == 1);
        h.strategy.on_order_rejected(orders[0], "rate_limit");
        h.strategy.on_order_rejected(orders[0], "rate_limit");
        // The rejection was on view 5 and the doubled wait is four views: 6, 7 and 8 are blocked, 9 retries.
        assert(h.on(100.15).empty());
        assert(h.on(100.20).empty());
        assert(h.on(100.25).empty());
        assert(!h.on(100.30).empty());
    }

    {
        // A venue refusal arrives as a cancelled report plus the rejection callback: the order leaves the open
        // set, and the next attempt waits rather than resubmitting on the next view.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 0,
                                .rejection_backoff_views = 2,
                                .max_rejection_backoff_views = 8});
        for (int index = 0; index < 4; ++index) h.on(100.00);
        auto orders = h.on(100.10);
        assert(orders.size() == 1);
        assert(h.strategy.working_order_count() == 1);

        h.strategy.on_order_update(ExecutionReport{.order_id = orders[0].order_id,
                                                   .side = exchange::Side::Buy,
                                                   .exec_type = ExecType::Rejected,
                                                   .symbol = "TEST",
                                                   .price = orders[0].price,
                                                   .quantity = orders[0].quantity,
                                                   .ts = 0,
                                                   .owner = "MarketMaker",
                                                   .reject_reason = RejectReason::Venue});
        assert(h.strategy.working_order_count() == 0);
        assert(h.on(100.15).empty());
        assert(!h.on(100.20).empty());
    }

    {
        // A user cancel is expected: the order leaves the open set and the next view may re-quote at once.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 0,
                                .rejection_backoff_views = 2,
                                .max_rejection_backoff_views = 8});
        for (int index = 0; index < 4; ++index) h.on(100.00);
        auto orders = h.on(100.10);
        assert(orders.size() == 1);
        h.strategy.on_order_update(ExecutionReport{.order_id = orders[0].order_id,
                                                   .side = exchange::Side::Buy,
                                                   .exec_type = ExecType::Cancelled,
                                                   .symbol = "TEST",
                                                   .price = orders[0].price,
                                                   .quantity = orders[0].quantity,
                                                   .ts = 0,
                                                   .owner = "MarketMaker",
                                                   .cancel_reason = CancelReason::User});
        assert(h.strategy.working_order_count() == 0);
        assert(!h.on(100.15).empty());
    }
    {
        // A venue cancel is not expected: it backs off like a rejection.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 0,
                                .rejection_backoff_views = 2,
                                .max_rejection_backoff_views = 8});
        for (int index = 0; index < 4; ++index) h.on(100.00);
        auto orders = h.on(100.10);
        assert(orders.size() == 1);
        h.strategy.on_order_update(ExecutionReport{.order_id = orders[0].order_id,
                                                   .side = exchange::Side::Buy,
                                                   .exec_type = ExecType::Cancelled,
                                                   .symbol = "TEST",
                                                   .price = orders[0].price,
                                                   .quantity = orders[0].quantity,
                                                   .ts = 0,
                                                   .owner = "MarketMaker",
                                                   .cancel_reason = CancelReason::SelfTrade});
        assert(h.strategy.working_order_count() == 0);
        assert(h.on(100.15).empty());
        assert(!h.on(100.20).empty());
    }

    {
        // A passive entry is priced on our own side, one tick inside a wide spread, and it never crosses; when the
        // spread is a single tick there is no room inside it, so the clip sits on the bid.
        const auto warm = [](Harness& h, double spread)
        {
            for (int index = 0; index < 4; ++index)
                h.on_view(view_between(100.0 - spread / 2.0, 100.0 + spread / 2.0));
        };
        Harness wide(TrendL2Config{.order_size = 10,
                                   .max_position = 10,
                                   .tick_size = 0.5,
                                   .fast_views = 2,
                                   .slow_views = 4,
                                   .entry_ticks = 0.5,
                                   .exit_ticks = 0.2,
                                   .stop_ticks = 5.0,
                                   .max_holding_views = 100,
                                   .own_side_entry = true,
                                   .entry_improve_ticks = 1.0});
        warm(wide, 1.0);
        const auto inside = wide.on_view(view_between(100.0, 102.0));
        assert(inside.size() == 1 && inside[0].side == Side::Buy);
        assert(std::abs(inside[0].price - 100.5) < 1e-9);
        assert(inside[0].price < 102.0);

        Harness tight(TrendL2Config{.order_size = 10,
                                    .max_position = 10,
                                    .tick_size = 0.5,
                                    .fast_views = 2,
                                    .slow_views = 4,
                                    .entry_ticks = 0.5,
                                    .exit_ticks = 0.2,
                                    .stop_ticks = 5.0,
                                    .max_holding_views = 100,
                                    .own_side_entry = true,
                                    .entry_improve_ticks = 1.0});
        warm(tight, 0.5);
        const auto clamped = tight.on_view(view_between(100.75, 101.25));
        assert(clamped.size() == 1);
        assert(std::abs(clamped[0].price - 100.75) < 1e-9);
    }
    {
        // An entry that nobody hit is cancelled once it reaches the age limit, and the cancel is the age rule's rather
        // than the book moving: the same view is repeated, so the quote still matches the book.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.5,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 0.05,
                                .exit_ticks = 0.02,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .own_side_entry = true,
                                .entry_improve_ticks = 1.0,
                                .max_order_age_views = 1,
                                .entry_cooldown_views = 0});
        for (int index = 0; index < 4; ++index) h.on_view(view_between(99.5, 100.5));
        const auto first = h.on_view(view_between(100.0, 102.0));
        assert(first.size() == 1);
        assert(h.strategy.cancel_requests().empty());          // the view that placed it does not cancel it
        assert(h.on_view(view_between(100.0, 102.0)).empty()); // working, so no second clip
        assert(h.strategy.cancel_requests().size() == 1);      // one view later it has aged out
        assert(h.strategy.working_order_count() == 1);         // the cancel is a request, not a state change
    }
    {
        // After an entry ages out, the next one waits the cooldown rather than chasing the same book.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.5,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 0.05,
                                .exit_ticks = 0.02,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .own_side_entry = true,
                                .entry_improve_ticks = 1.0,
                                .max_order_age_views = 1,
                                .entry_cooldown_views = 1});
        for (int index = 0; index < 4; ++index) h.on_view(view_between(99.5, 100.5));
        const auto first = h.on_view(view_between(100.0, 102.0));
        assert(first.size() == 1);
        assert(h.on_view(view_between(100.0, 102.0)).empty());
        assert(h.strategy.cancel_requests().size() == 1);
        h.strategy.on_order_update(ExecutionReport{.order_id = first[0].order_id,
                                                   .side = exchange::Side::Buy,
                                                   .exec_type = ExecType::Cancelled,
                                                   .symbol = "TEST",
                                                   .price = first[0].price,
                                                   .quantity = first[0].quantity,
                                                   .ts = 0,
                                                   .owner = "MarketMaker",
                                                   .cancel_reason = CancelReason::User});
        assert(h.strategy.working_order_count() == 0);
        assert(h.on_view(view_between(100.0, 102.0)).empty());   // still cooling down
        assert(!h.on_view(view_between(100.0, 102.0)).empty());  // and then the next clip goes out
    }

    return 0;
}
