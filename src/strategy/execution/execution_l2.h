#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/clock_units.h"
#include "exchange/execution_report.h"
#include "strategy/strategy.h"

/*
-------------------------------------------
 ExecutionL2Strategy
-------------------------------------------

The execution line, the mirror image of the signal line: there is no view about direction at all. A parent
order -- a side, a quantity and a deadline -- arrives from the run's configuration, and the job is to work it
out as cheaply as the venue allows.

Two decisions, independent of each other:

  the schedule   when a slice is due: `market` sends the whole parent once, `twap` splits it into equal slices
                 across the horizon, `pov` sizes each slice to a share of the volume that has traded.
  the entry      how a slice is sent: `cross` takes the far touch with an IOC, which pays the spread and the
                 taker fee; `post` rests at the near touch with a limit order and crosses only what the slice
                 has not filled when its own timeout expires.

  the follow     with `--amend-instead-of-cancel`, a resting child is moved with the near touch instead of
                 waiting at the price it joined: one request in place where the pair of pull-and-place is two,
                 and no moment with the level empty. It is the peg the amend's 83.2 ms round trip exists for,
                 and the strategy asks unconditionally because an engine without an amend receives the cancel
                 the request replaced.

The horizon is market time, not a view count: a view is 7 ms on the recorded slices and a different number on
any other feed, and a schedule that meant "ten views" would mean something different on each. One child works
at a time, because two would make the slice accounting a second schedule.
-------------------------------------------
*/

enum class ExecutionAlgo : uint8_t
{
    Market = 0,
    Twap,
    Pov
};

struct ExecutionL2Config
{
    // The parent. A side of Unknown or a zero quantity is an inert strategy, which app_config refuses to
    // build: an execution run without a parent has nothing to execute.
    Side side{Side::Unknown};
    uint64_t quantity{0};
    uint64_t horizon_us{0};
    ExecutionAlgo algo{ExecutionAlgo::Market};
    // TWAP: the number of equal children. POV: the share of traded volume to follow.
    uint64_t slices{10};
    double participation{0.10};
    // Post-first entry: rest at the near touch, and cross whatever a slice has not filled after this long.
    // `post_first` false is the crossing entry every run before the flag used.
    bool post_first{false};
    uint64_t post_timeout_us{0};
    // The follow, from --amend-instead-of-cancel: a resting child moves with the near touch in place rather than
    // waiting at the price it joined. Off by default, so every recorded arm keeps the child it had.
    bool amend_instead_of_cancel{false};
    // How far the touch must move before the follow moves the child, in ticks. One is the peg's own floor: a child
    // that does not follow a tick is not following the book. The maker's 2.0 is a different rule -- its amend is for
    // a retreat from the mid, where the touch is this line's whole reference.
    double amend_deadband_ticks{1.0};
    // The instrument's tick, for that deadband. A zero refuses to follow rather than guessing a grid: an amend priced
    // off a guessed tick is a request the venue declines, and a silent no-op is worse than no flag.
    double tick_size{0.0};
};

class ExecutionL2Strategy : public Strategy
{
   public:
    explicit ExecutionL2Strategy(ExecutionL2Config config = {}) : config_(config) {}

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        L2MarketView view;
        view.symbol = symbol;
        view.top = top;
        view.micro_price = (top.bid_price + top.ask_price) / 2.0;
        return on_l2_market_view(view);
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override;
    void on_market_trade(const MarketTrade& trade) override;
    void on_order_submitted(const StrategyOrder& order) override;
    std::vector<uint64_t> cancel_requests() override;
    std::vector<AmendRequest> amend_requests() override;
    void on_order_update(const ExecutionReport& report) override;

    int64_t net_position() const override
    {
        return position_;
    }

    std::size_t working_order_count() const override
    {
        return working_order_id_ == 0 ? 0 : 1;
    }

    StrategyMetrics metrics() const override
    {
        auto result = metrics_;
        result.available = true;
        return result;
    }

    // What the schedule has done so far, for the unit test and for a reader of the summary.
    uint64_t submitted_quantity() const
    {
        return submitted_quantity_;
    }

    uint64_t filled_quantity() const
    {
        return filled_quantity_;
    }

    uint64_t child_orders() const
    {
        return child_orders_;
    }

    uint64_t posted_children() const
    {
        return posted_children_;
    }

    uint64_t crossed_children() const
    {
        return crossed_children_;
    }

    // How many times the follow moved the child in place. On an engine that amends this is the only tell that the
    // flag did anything: the child keeps its id, so no new child appears in the count.
    uint64_t amend_count() const
    {
        return amend_count_;
    }

   private:
    // What is still owed to the parent: the quantity less what has filled and what is still working. A child
    // that dies unfilled leaves this number where it was, so the schedule retries rather than silently
    // declaring the parent done.
    uint64_t remaining_quantity() const;
    bool usable(const TopOfBook& top) const;
    double crossing_price(const TopOfBook& top) const;
    double resting_price(const TopOfBook& top) const;
    uint64_t slice_quantity() const;
    uint64_t pov_quantity() const;
    // The quantity the next child should carry, or zero when nothing is due yet. `crossed` says whether this
    // slice fell back to crossing.
    uint64_t next_slice_quantity(uint64_t ts, bool& crossed);
    // Move the resting child to the near touch when the touch has moved at least the deadband away from it.
    void follow_touch(const TopOfBook& top);

    ExecutionL2Config config_;
    bool started_{false};
    bool sent_market_child_{false};
    uint64_t end_ts_{0};
    uint64_t next_slice_ts_{0};
    uint64_t filled_quantity_{0};
    uint64_t submitted_quantity_{0};
    uint64_t child_orders_{0};
    uint64_t posted_children_{0};
    uint64_t crossed_children_{0};
    uint64_t market_volume_{0};
    uint64_t last_view_ts_{0};
    int64_t position_{0};
    // The slice being worked: what it still owes, and whether it has already fallen back to crossing.
    uint64_t slice_remaining_{0};
    bool slice_must_cross_{false};
    // The one child in flight. Its start is the market time the timeout is measured from.
    uint64_t working_order_id_{0};
    uint64_t working_started_ts_{0};
    bool working_is_post_{false};
    // Where the working child rests and what it was sent for. An amend carries the child's *total* quantity, because
    // the venue recomputes the remainder from what it has already filled; `open_quantities_` holds the remainder, so
    // passing that would shrink the child by what it had filled and the venue would decline the request.
    double working_price_{0.0};
    uint64_t working_quantity_{0};
    // An amend is outstanding. A terminal report on the working child that is a cancel is then the request arriving as
    // the cancel it replaced, and the slice is posted again at the new price rather than crossed: nothing expired it,
    // and crossing is what a timeout asks for.
    bool amend_pending_{false};
    bool slice_repost_{false};
    uint64_t amend_count_{0};
    std::vector<AmendRequest> amend_requests_;
    std::unordered_map<uint64_t, uint64_t> open_quantities_;
    std::vector<uint64_t> cancel_queue_;
    StrategyMetrics metrics_{};
};

// The view clock's unit comes from `common/clock_units.h`: milliseconds on an OKX book cut (`1794834900007`) and
// microseconds on the Deribit fixtures. The horizon arrives in seconds, so without that conversion a 7,200-second
// parent became an 8.3-day slice interval on the OKX cut and the run sent exactly one child (measured 2026-11-26:
// `child_orders=1 span_seconds=0` on a two-hour window).

inline uint64_t ExecutionL2Strategy::remaining_quantity() const
{
    uint64_t working = 0;
    for (const auto& [order_id, quantity] : open_quantities_) working += quantity;
    const uint64_t committed = filled_quantity_ + working;
    return config_.quantity > committed ? config_.quantity - committed : 0;
}

inline bool ExecutionL2Strategy::usable(const TopOfBook& top) const
{
    return top.bid_price > 0.0 && top.ask_price > 0.0;
}

inline double ExecutionL2Strategy::crossing_price(const TopOfBook& top) const
{
    // Buying takes the offer and selling hits the bid: the child pays the spread and the taker fee.
    return config_.side == Side::Buy ? top.ask_price : top.bid_price;
}

inline double ExecutionL2Strategy::resting_price(const TopOfBook& top) const
{
    // Buying joins the bid and selling joins the offer: the child earns the spread instead of paying it, and
    // the queue models decide whether it fills.
    return config_.side == Side::Buy ? top.bid_price : top.ask_price;
}

inline uint64_t ExecutionL2Strategy::slice_quantity() const
{
    const uint64_t slices = std::max<uint64_t>(1, config_.slices);
    const uint64_t even = config_.quantity / slices;
    return std::max<uint64_t>(1, even);
}

inline uint64_t ExecutionL2Strategy::pov_quantity() const
{
    // A share of the volume that has traded since the parent started, less what has already filled; a child
    // that died unfilled leaves the allowance intact rather than consuming it.
    const double target = config_.participation * static_cast<double>(market_volume_);
    const double allowance = target - static_cast<double>(filled_quantity_);
    return allowance >= 1.0 ? static_cast<uint64_t>(allowance) : 0;
}

inline uint64_t ExecutionL2Strategy::next_slice_quantity(uint64_t ts, bool& crossed)
{
    // A slice that lost its resting child is crossed now, whatever the schedule would have done next.
    crossed = !config_.post_first || slice_must_cross_;
    switch (config_.algo)
    {
        case ExecutionAlgo::Market:
            if (sent_market_child_) return 0;
            sent_market_child_ = true;
            return remaining_quantity();
        case ExecutionAlgo::Twap:
        {
            if (ts < next_slice_ts_) return 0;
            const uint64_t quantity = std::min(slice_quantity(), remaining_quantity());
            // Advance by the slice interval rather than by a view count, so a slow view rate stretches the
            // schedule instead of piling several slices onto one view. The interval is seconds of horizon turned
            // into the view clock's own unit, which is milliseconds on an OKX book cut and microseconds on the
            // Deribit fixtures -- getting that wrong is what made the first run send one child in two hours.
            const uint64_t slices = std::max<uint64_t>(1, config_.slices);
            const uint64_t units = clock_units_per_second(ts);
            const uint64_t seconds = std::max<uint64_t>(1, config_.horizon_us / 1'000'000ULL);
            const uint64_t interval = std::max<uint64_t>(1, seconds / slices) * units;
            next_slice_ts_ = ts + interval;
            return quantity;
        }
        case ExecutionAlgo::Pov:
            return std::min(pov_quantity(), remaining_quantity());
    }
    return 0;
}


inline void ExecutionL2Strategy::follow_touch(const TopOfBook& top)
{
    // The peg. A resting child is only worth the queue it holds while it is at the touch: once the touch has moved
    // a deadband away the child is either behind the market (no fill) or through it (a stale quote the sweep takes),
    // and one amend puts it back with one round trip instead of the pair's two. Both directions are followed, unlike
    // the maker's retreat-only asymmetry: that rule was measured for a quote whose payoff is capture, and this line
    // has no direction of its own -- being at the touch is the whole of what it can ask for.
    if (!usable(top) || config_.tick_size <= 0.0 || working_quantity_ == 0) return;
    const double target = resting_price(top);
    const double deadband = std::max(1.0, config_.amend_deadband_ticks) * config_.tick_size;
    if (!std::isfinite(target) || std::abs(target - working_price_) < deadband - 1e-12) return;
    amend_requests_.push_back(AmendRequest{working_order_id_, target, working_quantity_});
    // The local record takes the new price at once, because the next view compares against it; without that the same
    // move is amended again on every view and the child never settles.
    working_price_ = target;
    amend_pending_ = true;
    ++amend_count_;
}

inline std::vector<Strategy::AmendRequest> ExecutionL2Strategy::amend_requests()
{
    std::vector<AmendRequest> pending;
    pending.swap(amend_requests_);
    return pending;
}
inline std::vector<StrategyOrder> ExecutionL2Strategy::on_l2_market_view(const L2MarketView& view)
{
    last_view_ts_ = view.ts;
    std::vector<StrategyOrder> orders;
    if (config_.side == Side::Unknown || config_.quantity == 0 || !usable(view.top)) return orders;
    if (!started_)
    {
        started_ = true;
        const uint64_t units = clock_units_per_second(view.ts);
        const uint64_t seconds = std::max<uint64_t>(1, config_.horizon_us / 1'000'000ULL);
        end_ts_ = view.ts + seconds * units;
        next_slice_ts_ = view.ts;
    }
    if (view.ts > end_ts_ || filled_quantity_ >= config_.quantity) return orders;

    // A resting child is pulled once its slice's timeout expires, and what it has not filled is crossed on this
    // view: the fallback is the whole point of post-first, so it must not wait for the next slice. A crossing
    // child should die with its own IOC, so one still working after a view is pulled too -- that is the
    // time-in-force override case rather than the post case.
    // A working child blocks the next one, and a **crossing** child blocks it until its own report arrives: an IOC
    // either fills or dies at the venue, so pulling it and re-sending the same slice while it is in flight
    // double-submits. Measured 2026-11-26, the first execution run: at a 10 ms order latency the pull-and-resend
    // path executed **5.1 times the parent** in 51 child orders for a ten-slice schedule. A posted child is still
    // pulled on its timeout -- that is what the fallback is for -- and a crossing child's report is what releases
    // the slot.
    if (working_order_id_ != 0)
    {
        const bool posted_timed_out = working_is_post_ && config_.post_timeout_us > 0 &&
                                      view.ts >= working_started_ts_ +
                                                     (config_.post_timeout_us / 1'000'000ULL) *
                                                         clock_units_per_second(view.ts);
        if (!posted_timed_out)
        {
            // A resting child follows the touch in place while it waits. Without the follow it sits at the price it
            // joined for the whole timeout, which on a moving book is a quote the market left behind.
            if (config_.amend_instead_of_cancel && working_is_post_) follow_touch(view.top);
            return orders;
        }
        cancel_queue_.push_back(working_order_id_);
        open_quantities_.erase(working_order_id_);
        working_order_id_ = 0;
        working_is_post_ = false;
        amend_pending_ = false;
        slice_must_cross_ = true;
    }

    bool crossed = false;
    if (slice_remaining_ == 0)
    {
        const uint64_t quantity = next_slice_quantity(view.ts, crossed);
        if (quantity == 0) return orders;
        slice_remaining_ = quantity;
    }
    else
    {
        // The slice already exists and lost its child: cross what it still owes if a timeout released it, and post it
        // again if the follow's own amend did. An amend is not a deadline, so it must not turn into a taker fill.
        crossed = !slice_repost_;
        slice_repost_ = false;
    }

    const double price = crossed ? crossing_price(view.top) : resting_price(view.top);
    orders.emplace_back(config_.side, view.symbol.str(), price, slice_remaining_, 0,
                        crossed ? TimeInForce::Ioc : TimeInForce::Gtc);
    slice_must_cross_ = false;
    return orders;
}

inline void ExecutionL2Strategy::on_market_trade(const MarketTrade& trade)
{
    // POV follows the market's own pace, so it needs the venue's printed volume rather than a clock. Trades
    // before the parent starts belong to somebody else's order and are not counted.
    if (!started_) return;
    market_volume_ += trade.quantity;
}

inline void ExecutionL2Strategy::on_order_submitted(const StrategyOrder& order)
{
    open_quantities_.insert_or_assign(order.order_id, order.quantity);
    working_order_id_ = order.order_id;
    working_started_ts_ = last_view_ts_;
    working_is_post_ = order.time_in_force != TimeInForce::Ioc;
    // Where the child rests and what it was sent for, kept for the follow: an amend carries the total quantity, and
    // the price it is compared against is the one the child actually joined.
    working_price_ = order.price;
    working_quantity_ = order.quantity;
    amend_pending_ = false;
    ++child_orders_;
    if (working_is_post_)
        ++posted_children_;
    else
        ++crossed_children_;
    submitted_quantity_ += order.quantity;
    ++metrics_.quote_count;
    metrics_.submitted_quantity += order.quantity;
}

inline std::vector<uint64_t> ExecutionL2Strategy::cancel_requests()
{
    auto ids = cancel_queue_;
    cancel_queue_.clear();
    metrics_.cancel_count += ids.size();
    return ids;
}

inline void ExecutionL2Strategy::on_order_update(const ExecutionReport& report)
{
    if (report.exec_type == ExecType::Trade)
    {
        const uint64_t executed = report.executed_quantity();
        const int64_t signed_quantity = static_cast<int64_t>(executed);
        position_ += report.side == exchange::Side::Buy ? signed_quantity : -signed_quantity;
        filled_quantity_ += executed;
        slice_remaining_ = slice_remaining_ > executed ? slice_remaining_ - executed : 0;
        const auto working = open_quantities_.find(report.order_id);
        if (working != open_quantities_.end())
            working->second = working->second > executed ? working->second - executed : 0;
        ++metrics_.fill_count;
        metrics_.filled_quantity += executed;
        metrics_.fees_paid += report.fee;
    }
    if (is_terminal(report.exec_type))
    {
        open_quantities_.erase(report.order_id);
        if (report.order_id == working_order_id_)
        {
            // An amend the engine could not take arrives as the cancel it replaced. Nothing expired this child, so
            // the slice is posted again at the new price instead of crossed -- and a cancel the venue sent for its own
            // reasons is answered the same way, because the parent still has to be worked.
            slice_repost_ = amend_pending_ && report.exec_type == ExecType::Cancelled;
            amend_pending_ = false;
            working_order_id_ = 0;
            working_is_post_ = false;
        }
    }
}
