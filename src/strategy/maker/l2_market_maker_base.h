#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/clock_units.h"
#include "exchange/execution_report.h"
#include "strategy/strategy.h"

// The root of the L2 family. It holds the whole quoting engine -- the fair-price signal, the quote
// lifecycle, the queue-hold refresh policy and the counters -- and the two `final` classes beside it
// each add one behaviour to it: `inventory_aware_l2_market_maker.h` scales and shifts a quote by the
// position, and `active_l2_market_maker.h` halts quoting while the mid is moving fast. Both derive
// from this class rather than wrapping it, because a wrapper has to forward every virtual by hand and
// a forgotten forward changes behaviour silently instead of failing to compile.
//
// The class and its file both carry `Base` because the family is not readable from a directory listing
// otherwise: three names ending in `_l2_market_maker.h` look like siblings, and the one every other one
// extends is not the one a reader would pick.

enum class L2SignalMode
{
    Baseline,
    Depth,
    Micro,
    Flow
};

struct L2MarketMakerBaseConfig
{
    uint64_t order_size{1};
    // The instrument's minimum tradable quantity. A quote scaled down by the weak-flow guard is raised
    // back to this rather than sent at a size the venue will refuse; 1 leaves the old behaviour.
    uint64_t min_order_quantity{1};
    double base_spread{1.0};
    int64_t inventory_limit{100};
    double tick_size{0.5};
    double imbalance_shift{1.0};
    double trade_imbalance_shift{1.0};
    L2SignalMode signal_mode{L2SignalMode::Depth};
    // Turns on the two flow guards. It sits next to the mode because it belongs to the signal choice,
    // and it used to be implied by `toxicity_flow_threshold <= 1.0`, a comparison that reads like
    // configuration and is not: the guards were off for every mode whose threshold sits above one, and
    // no setting of the config made that visible. The factory enables it for the flow strategies,
    // which is exactly the set that had it, so behaviour is unchanged and the switch is now readable.
    bool flow_guard{false};
    uint64_t trade_window{32};
    uint64_t refresh_price_ticks{1};
    uint64_t max_quote_age{20};
    double toxicity_flow_threshold{2.0};
    double weak_flow_threshold{0.9};
    double weak_flow_quote_scale{0.5};
    double weak_flow_spread_shift_ticks{1.0};
    bool quote_at_best_when_neutral{false};
    // How many quote cycles a stale quote is held while its own queue is being consumed, and the
    // inventory ratio the maker treats as neutral when quote_at_best_when_neutral is on.
    uint64_t max_queue_hold_count{3};
    double neutral_inventory_band{0.25};
    // The expected-value gate of the decision layer. A side is quoted only when the edge that fill would
    // capture, measured from the reference price in ticks, covers the maker fee plus this many extra ticks.
    // The fee in ticks is rate * price / tick_size, which is the same arithmetic the book's measurements
    // report: on the Deribit contract at 2020 prices that is 2.55 ticks, and it is the reason a quote at the
    // touch cannot pay for itself. A zero rate or zero extra ticks leaves the gate off, so every documented
    // run that does not ask for the gate keeps its numbers.
    double maker_fee_rate{0.0};
    // A commission charged for the instrument's own unit, in USD, and the USD value of one price unit in one
    // quantity unit. Both are here because the gate has to see the whole fee: a US share and a listed future
    // are billed per share and per contract with a zero rate, so a gate that only knew the rate would treat
    // those markets as free and quote everything. The tick's USD value is tick_size * multiplier, so the
    // commission in ticks is fee_per_unit / (tick_size * multiplier), which for a hundred dollar share is a
    // third of a tick and for a micro E-mini is half of one.
    double fee_per_unit{0.0};
    double multiplier{1.0};
    double edge_cover_ticks{0.0};
    // The adverse-selection gate: a side is quoted only when the edge that fill would capture also covers what
    // passive fills at that distance from the midpoint have actually cost in this window, as the pipeline
    // measures it from the market's own prints. It is the second term of the inequality a maker lives by —
    // capture >= fee + adverse selection — and unlike the fee it cannot be read off a schedule, so it arrives
    // as an estimate and it is the reason this gate exists rather than a wider `edge_cover_ticks` setting.
    // False leaves it off, and a run that does not ask must behave exactly as it did before the gate existed.
    bool markout_cover{false};
    // Fills of our own that have to resolve before the measured cover is used. Below it the gate covers the fee
    // only, which is what lets it learn: a gate that tightened from the first decision, on no evidence, would
    // refuse everything and never measure anything.
    uint64_t markout_cover_min_samples{20};
    // Sides refused by the adverse-selection gate, which is the attribution of the same decision.
    uint64_t markout_gate_skips{0};
    // Inventory discipline: above this many units the position is reduced instead of carried, by an order priced
    // at the touch on the reducing side, which the engine fills as a taker. Zero, the default, leaves it off, so
    // every run that does not ask keeps the position it used to carry into the final mark.
    int64_t flatten_band_units{0};
    uint64_t flatten_orders{0};
    uint64_t flatten_quantity{0};
    // A condition rather than a quote parameter: quote only when the top of book is at least this many ticks
    // wide. The bucketed markout is why. When the spread is one tick, the best a quote can be is the touch,
    // which sits half a tick from the midpoint, and those are the fills that resolve negative, so the rule
    // refuses the situation instead of the price. Zero leaves it off, as every recorded run asks.
    double min_spread_ticks{0.0};
    // Decision attribution: sides not quoted because the gate refused them.
    uint64_t edge_gate_skips{0};
    uint64_t min_spread_skips{0};
    // The asymmetric refresh: when a requote would move a side *towards* the market -- the bid up or the ask down,
    // which is the quote that gets filled badly on a slow link -- that side waits until the fair price has been
    // unchanged for this long, while the retreating side goes out at once. Zero, the default, leaves the symmetric
    // behaviour every recorded run was measured with.
    uint64_t advance_debounce_ms{0};
    // The inventory skew that closes rather than adds: while the position is long the ask may come in to this many
    // ticks from the reference price, and while it is short the bid may, so the side that reduces the position is the
    // one that gets filled first. The adding side keeps `base_spread`, and both stay outside the touch. Zero, the
    // default, leaves the symmetric distance alone.
    double close_side_ticks{0.0};
    // Extra passive layers behind the primary quote, each one tick further out. The fill *count* a window carries is
    // bounded by the number of orders rather than by their size -- measured 2026-11-25, doubling the clip raised the
    // notional and left the count near twenty-one -- so a second layer at the same size is the only way to quote more
    // often at the same price. Zero, the default, leaves one layer a side.
    uint64_t extra_layers{0};
    // Sides held back by the rule above, so a run can say how often it refused to chase.
    uint64_t advance_skips{0};
    // The quote's half width as basis points of the reference price: `max(2, round(bps / tick_bps))` ticks, where one
    // tick is worth `tick_size / price` of the price. A fixed tick count is a different economic distance on every
    // instrument -- UP's tick is 5.2 bps and DNA's 7.3 -- which is the whole reason the rule exists. Zero, the default,
    // leaves `base_spread` alone and every recorded number where it was.
    double base_spread_bps{0.0};
    // The adding side stops at this fraction of the inventory cap, so the position is worked down by the closing side
    // alone instead of being carried into the mark; `inventory_limit` is the same rule at 1.0. Zero, the default,
    // quotes both sides until the cap.
    double onesided_inventory_frac{0.0};
    // The size throttle, which is the *state* version of the rule above: once the carried position is marked against us
    // by more than `throttle_adverse_bps` (a negative `open_pnl_bps`), the side that would add to it quotes
    // `throttle_units` -- zero pauses that side -- while the closing side keeps its full size. It removes adverse flow
    // without moving a price, which is what the wind-down's price skew was refused for. Off at zero.
    double throttle_adverse_bps{0.0};
    uint64_t throttle_units{0};
    // Quotes that took the throttle, so an arm can say the rule fired rather than inferring it from the fills.
    uint64_t throttled_quotes{0};
    // The distance between extra layers, in basis points of the reference price, when a fixed tick would collapse them:
    // at a 0.76 bps tick "one more tick" puts the second layer on almost the same price as the first. Rounded outward to
    // the tick grid and floored at one tick, so a layer is never closer than the run asked for. Zero, the default,
    // leaves one tick a layer and every recorded number where it was.
    double layer_step_bps{0.0};
    // Move a primary quote in place instead of pulling it and placing again, when the venue can amend. Off by default;
    // the pipeline sends the cancel an engine without an amend would have taken, so a run that does not ask is unchanged.
    bool amend_instead_of_cancel{false};
    // The deadband an amend must clear, in ticks. The measured symmetric version amended on a single tick and put 239 to
    // 296 quotes at the back of a level that had just been swept, against 21 to 53 for the cancel path; two ticks is the
    // smallest move that is a decision rather than the book breathing.
    double amend_deadband_ticks{2.0};
    // The state switch: instead of amending every refresh, amend only while the book is toxic -- the mid ranging over a
    // short window, or the flow one-sided -- and keep the cancel path while it is calm. The two arms were measured
    // separately (26 fills / +9.83 closed with the cancel path, 19 / +14.87 with the two-tick retreat), and what this
    // switch buys is the fills of the first during calm intervals plus the capture of the second during sweeps. Off by
    // default, so every recorded number stands.
    bool amend_on_toxicity{false};
    // How one-sided the recent flow has to be for that state. The imbalance is `2 * buy / (buy + sell) - 1` over the
    // trade window, so 0.9 is a book whose last thirty-two prints were 95 per cent one side.
    double toxicity_imbalance{0.9};
    // Refreshes taken on each branch, so a run can say how much of its window was toxic rather than only what it did.
    uint64_t toxicity_amends{0};
    uint64_t toxicity_refreshes{0};
    // The wind-down: over the last `wind_down_skew_minutes` before `wind_down_end_minute` (minutes of the UTC day, e.g.
    // 930 for 15:30), the side that closes the position walks up to `wind_down_skew_ticks` towards the inside and the
    // adding side stops opening, so the market's own noise works the residual instead of a taker cross at the end. The
    // end has to be named because the strategy cannot see the session's end: a replay's last event is only known at the
    // end of the replay. Zero minutes, the default, leaves both sides quoting as they did.
    uint64_t wind_down_skew_minutes{0};
    double wind_down_end_minute{0.0};
    double wind_down_skew_ticks{1.0};
    // Views the wind-down was active on, so an arm can be told from a window that never reached its last minutes.
    uint64_t wind_down_views{0};
    // The wind-down's condition: start the skew only when the carried position is marked **against** us by at least this
    // many bps of the reference price. Zero, the default, is the unconditional skew every arm before 2026-11-26 ran -- and
    // on UP at ten contracts that arm cost +11.33 to -5.43 bps of settled median by clearing a tail the week had marked in
    // our favour. A run that names this keeps the favourable tail and works only the adverse one.
    double wind_down_adverse_bps{0.0};
    // --max-depth-ratio: no rung may be larger than this share of the same side's displayed touch size, and a size above
    // the cap is split into `depth_ladder_max` rungs one tick behind each other. A fixed size on a thin book is adverse
    // selection by construction -- measured 2026-11-26, BLUR at thirteen ticks reads +31.74 bps closed at one contract and
    // +9.32 at a hundred -- so the size is what to size to the book, not the price. Zero, the default, leaves every
    // recorded number where it was.
    double max_depth_ratio{0.0};
    uint64_t depth_ladder_max{4};
    // Rungs placed behind the primary by the cap, and the quantity it refused to send at all, for attribution.
    uint64_t depth_capped_rungs{0};
    uint64_t depth_unquoted_quantity{0};
};

// The L2 market maker. Strategies that add behaviour to it derive from it rather than holding one:
// a wrapper has to hand-forward every callback, and a forgotten one changes behaviour silently.
class L2MarketMakerBase : public Strategy
{
   public:
    explicit L2MarketMakerBase(L2MarketMakerBaseConfig config = {}) : config_(config) {}

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        const double mid = (top.bid_price + top.ask_price) / 2.0;
        last_view_mid_ = mid;
        return quote(symbol, top, mid);
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        // The view's clock is the one the debounce measures with: a strategy only acts on a market event, so the
        // event's own timestamp is when this decision was taken.
        last_view_ts_ = view.ts;
        last_view_mid_ = (view.top.bid_price + view.top.ask_price) / 2.0;
        if (view.top.bid_price <= 0.0 || view.top.ask_price <= 0.0)
            return quote(view.symbol.str(), view.top, 0.0);
        return quote(view.symbol.str(), view.top, fair_price(view));
    }

    void on_market_trade(const MarketTrade& trade) override
    {
        if (trade.aggressor_side != Side::Buy && trade.aggressor_side != Side::Sell) return;
        recent_trades_.push_back({trade.aggressor_side, trade.quantity});
        if (trade.aggressor_side == Side::Buy)
            buy_volume_ += trade.quantity;
        else
            sell_volume_ += trade.quantity;
        while (recent_trades_.size() > config_.trade_window)
        {
            const auto stale_trade = recent_trades_.front();
            recent_trades_.pop_front();
            if (stale_trade.side == Side::Buy)
                buy_volume_ = buy_volume_ > stale_trade.quantity
                                   ? buy_volume_ - stale_trade.quantity
                                   : 0;
            else
                sell_volume_ = sell_volume_ > stale_trade.quantity
                                   ? sell_volume_ - stale_trade.quantity
                                   : 0;
        }
    }

    void on_queue_activity(Side side, uint64_t consumed_quantity) override
    {
        queue_activity_ += consumed_quantity;
        if (side == Side::Buy)
        {
            metrics_.buy_queue_consumed += consumed_quantity;
        }
        else if (side == Side::Sell)
        {
            metrics_.sell_queue_consumed += consumed_quantity;
        }
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders_[order.order_id] = order;
        // What the order was sent for, which the working record no longer knows once it has filled: an amend carries the
        // *total* quantity, because the venue recomputes the remainder from what it has already filled. Sending the
        // remainder instead makes `quantity - filled` zero or negative at the venue, which declines the amend -- and the
        // refusal is a terminal report, so the strategy would forget an order that is still resting.
        order_totals_[order.order_id] = order.quantity;
        submitted_orders_.note(order.order_id);
        metrics_.submitted_quantity += order.quantity;
        ++metrics_.quote_count;
        if (order.side == Side::Buy)
            ++metrics_.buy_quote_count;
        else if (order.side == Side::Sell)
            ++metrics_.sell_quote_count;
    }

    std::vector<uint64_t> cancel_requests() override
    {
        if (!cancel_pending_) return {};
        std::vector<uint64_t> order_ids;
        order_ids.reserve(open_orders_.size());
        for (const auto& entry : open_orders_)
        {
            // An order that is being moved in place is not pulled: the amend is the requote.
            const bool amended = std::any_of(amend_requests_.begin(), amend_requests_.end(),
                                             [&entry](const AmendRequest& request)
                                             { return request.order_id == entry.first; });
            if (!amended) order_ids.push_back(entry.first);
        }
        cancel_pending_ = false;
        metrics_.cancel_count += order_ids.size();
        return order_ids;
    }

    std::vector<AmendRequest> amend_requests() override
    {
        std::vector<AmendRequest> pending;
        pending.swap(amend_requests_);
        return pending;
    }

    void request_cancel_all()
    {
        cancel_pending_ = !open_orders_.empty();
    }

    int64_t net_position() const override
    {
        return position_;
    }

    std::size_t working_order_count() const override
    {
        return open_orders_.size();
    }

    StrategyMetrics metrics() const override
    {
        auto result = metrics_;
        result.available = true;
        // The switch's two branches, so a run can say how much of its window was toxic rather than only what it did.
        result.toxicity_amends = config_.toxicity_amends;
        result.toxicity_refreshes = config_.toxicity_refreshes;
        return result;
    }

    // The pipeline resolves what the market's own prints were worth to a passive fill and pushes the running
    // estimate here. Storing it is all this does: the decision reads it when it prices a quote, which is the
    // moment the distance from the midpoint is known.
    void on_markout_estimate(const MarkoutEstimate& estimate) override
    {
        markout_estimate_ = estimate;
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (is_terminal(report.exec_type))
        {
            open_orders_.erase(report.order_id);
            order_totals_.erase(report.order_id);
            return;
        }
        if (report.exec_type != ExecType::Trade) return;

        auto order = open_orders_.find(report.order_id);
        // A fill can arrive once its order has left the working set: a live venue answers a cancel for an order it
        // has already filled with a rejection and sends the fill after it, and the close-out's cancel sweep is where
        // the demo hit it. The id the order was submitted under is what makes the fill ours -- the account books it
        // either way -- so the position moves and only the working-quantity bookkeeping is skipped. A fill for an id
        // this process never submitted is still ignored, which is what a resumed session sees.
        if (order == open_orders_.end() && !submitted_orders_.knows(report.order_id)) return;
        // The volume-weighted entry of the carried position, kept beside the position itself: the wind-down's condition
        // asks whether this tail is marked against us, and a position without its entry cannot answer. The arithmetic is
        // trend_l2's -- a crossing fill resets it, a reducing fill leaves the entry of what remains alone.
        const int64_t execution = static_cast<int64_t>(report.executed_quantity());
        const int64_t signed_quantity = report.side == exchange::Side::Buy ? execution : -execution;
        const int64_t previous = position_;
        position_ += signed_quantity;
        if (previous == 0 || (previous > 0) != (position_ > 0))
            average_entry_ = report.price;
        else if ((previous > 0 && signed_quantity > 0) || (previous < 0 && signed_quantity < 0))
        {
            const double before = static_cast<double>(previous < 0 ? -previous : previous);
            const double added = static_cast<double>(execution);
            average_entry_ = (average_entry_ * before + report.price * added) / (before + added);
        }
        if (position_ == 0) average_entry_ = 0.0;
        metrics_.filled_quantity += report.executed_quantity();
        ++metrics_.fill_count;
        metrics_.fees_paid += report.fee;
        if (report.side == exchange::Side::Buy)
            ++metrics_.buy_fill_count;
        else
            ++metrics_.sell_fill_count;
        if (order == open_orders_.end()) return;
        order->second.quantity = order->second.quantity > report.executed_quantity()
                         ? order->second.quantity - report.executed_quantity()
                                     : 0;
        if (order->second.quantity == 0) open_orders_.erase(order);
    }

   private:
    double trade_imbalance() const
    {
        const uint64_t total_volume = buy_volume_ + sell_volume_;
        if (total_volume == 0) return 0.0;
        return static_cast<double>(buy_volume_) / static_cast<double>(total_volume) * 2.0 - 1.0;
    }

   protected:
    // The state the amend's toxicity switch reads. The flow half lives here because the base owns the trade window; a
    // derived strategy that has a faster signal of its own -- `ActiveL2MarketMaker`'s short-window range -- adds it to
    // this one rather than replacing it, so a run gets both triggers with one flag.
    virtual bool toxicity_state() const
    {
        return config_.toxicity_imbalance <= 1.0 &&
               std::abs(trade_imbalance()) >= config_.toxicity_imbalance;
    }

    // How far into the wind-down the view is: 0 before it starts, 1 at `wind_down_end_minute`, linear between the two.
    // The clock is the view's own -- milliseconds on an OKX cut, microseconds on the Deribit fixtures -- and the
    // wall-clock second is what the end is named in, so the conversion goes through `clock_units_per_second` rather than
    // a constant: the last time a duration was compared against a raw timestamp in this tree, a 40 ms debounce became
    // forty seconds of book time (measured 2026-11-26).
    double wind_down_progress() const
    {
        if (config_.wind_down_skew_minutes == 0 || config_.wind_down_end_minute <= 0.0 ||
            last_view_ts_ == 0)
            return 0.0;
        // The condition, before the clock: a tail the market has marked in our favour is left to settle, because closing
        // it gives up that mark -- the difference the ten-contract arm measured (settled +11.33 with no skew, -5.43 with
        // it). Only an adverse tail is worked, and "adverse" is the position's own mark in bps of the mid.
        if (config_.wind_down_adverse_bps > 0.0 &&
            open_pnl_bps() > -config_.wind_down_adverse_bps)
            return 0.0;
        const uint64_t units = clock_units_per_second(last_view_ts_);
        if (units == 0) return 0.0;
        const double second_of_day =
            static_cast<double>((last_view_ts_ / units) % 86'400ULL);
        const double end = config_.wind_down_end_minute * 60.0;
        const double start = end - 60.0 * static_cast<double>(config_.wind_down_skew_minutes);
        if (second_of_day <= start) return 0.0;
        if (second_of_day >= end) return 1.0;
        return (second_of_day - start) / (end - start);
    }

    // How the carried position is marked, in bps of the last mid: positive is in our favour. Zero when flat, which makes
    // the condition above false for a run with nothing to work.
    double open_pnl_bps() const
    {
        const double mid = last_view_mid_ > 0.0 ? last_view_mid_ : last_fair_price_;
        if (position_ == 0 || average_entry_ <= 0.0 || mid <= 0.0) return 0.0;
        const double move = position_ > 0 ? mid - average_entry_ : average_entry_ - mid;
        return move / mid * 1e4;
    }

   private:

    // The mid, displaced by whichever signal this configuration selected. Each signal is a small
    // displacement of the same reference point, so the quoting logic below does not have to know
    // which one is active, and adding one no longer means editing the double-quote path.
    double fair_price(const L2MarketView& view) const
    {
        const double mid = (view.top.bid_price + view.top.ask_price) / 2.0;
        const double micro_price = view.micro_price > 0.0 ? view.micro_price : mid;
        switch (config_.signal_mode)
        {
            case L2SignalMode::Baseline:
                break;
            case L2SignalMode::Depth:
                return mid + view.depth_imbalance * config_.imbalance_shift;
            case L2SignalMode::Micro:
                return micro_price;
            case L2SignalMode::Flow:
                return micro_price + view.depth_imbalance * config_.imbalance_shift +
                       trade_imbalance() * config_.trade_imbalance_shift;
        }
        return mid;
    }

    // Position as a fraction of the inventory limit, clamped so a limit of zero cannot divide by it.
    double position_ratio() const
    {
        if (config_.inventory_limit <= 0) return 0.0;
        return std::clamp(static_cast<double>(position_) /
                              static_cast<double>(config_.inventory_limit),
                          -1.0, 1.0);
    }

    // Whether the resting quotes must be pulled before new ones go out, and which of the two reasons
    // it was. This is the whole risk decision of the maker: a quote that is never refreshed is a free
    // option, and one that is refreshed on every tick pays the queue again every tick.
    bool needs_refresh(const TopOfBook& top, double fair_price)
    {
        const double market_mid = (top.bid_price + top.ask_price) / 2.0;
        const bool market_moved = last_market_mid_ <= 0.0 ||
                                  std::abs(market_mid - last_market_mid_) >= config_.tick_size;
        if (market_moved) ++quote_age_;
        const bool price_changed =
            last_fair_price_ <= 0.0 ||
            std::abs(fair_price - last_fair_price_) >=
                static_cast<double>(config_.refresh_price_ticks) * config_.tick_size;
        // Holding a stale quote while the queue in front of it is being consumed is deliberate: that
        // consumption is the only mechanism that moves the order towards the front, so pulling the
        // quote to chase the price throws away the position it was accumulated for.
        if (quote_age_ >= config_.max_quote_age && queue_activity_ > 0 && !price_changed &&
            queue_hold_count_ < config_.max_queue_hold_count)
        {
            ++queue_hold_count_;
            quote_age_ = 0;
            queue_activity_ = 0;
            return false;
        }
        if (quote_age_ < config_.max_quote_age && !price_changed) return false;
        if (price_changed)
        {
            ++metrics_.price_refresh_count;
            // When the fair price last moved, in the view clock the debounce reads.
            fair_change_ts_ = last_view_ts_;
        }
        else
            ++metrics_.age_refresh_count;
        cancel_pending_ = true;
        last_market_mid_ = market_mid;
        return true;
    }

    // The two prices a quote rests at, and the only copy of that arithmetic: the half width (a tick count or an
    // economic distance), the inventory skew, the weak-flow shift, the clamp outside the touch, the neutral quote at
    // the touch and the closing side. `quote()` used to compute the same block inline, because its gates mutate the
    // *quantities* between the prices and the placement; they still do and they still run there, but the prices no
    // longer have a second copy to drift from.
    //
    // The half width when the run asked for a basis-point distance: one tick is worth `tick_size / fair_price` of the
    // price, so a target of `D` basis points is `max(2, round(D / tick_bps))` ticks -- the same economic distance on
    // every instrument, which a fixed count is not. The floor of two ticks is the narrowest book the rule can rest in
    // without quoting the touch.
    struct QuoteTargets
    {
        double bid{0.0};
        double ask{0.0};
        bool valid{false};
    };

    QuoteTargets target_prices(const TopOfBook& top, double fair_price, double inventory_ratio,
                               double flow) const
    {
        if (fair_price <= 0.0) fair_price = (top.bid_price + top.ask_price) / 2.0;
        double half_width = config_.base_spread;
        if (config_.base_spread_bps > 0.0 && config_.tick_size > 0.0 && fair_price > 0.0)
        {
            const double tick_bps = config_.tick_size / fair_price * 1e4;
            half_width = std::max(2.0, std::round(config_.base_spread_bps / tick_bps)) * config_.tick_size;
        }
        const double inventory_shift = inventory_ratio * half_width;
        double weak_flow_spread_shift = 0.0;
        if (config_.flow_guard && std::abs(flow) > config_.weak_flow_threshold &&
            std::abs(inventory_ratio) > config_.neutral_inventory_band)
            weak_flow_spread_shift = config_.weak_flow_spread_shift_ticks * config_.tick_size;
        const double raw_bid = std::floor((fair_price - half_width - inventory_shift - weak_flow_spread_shift) /
                                          config_.tick_size) * config_.tick_size;
        const double raw_ask = std::ceil((fair_price + half_width - inventory_shift + weak_flow_spread_shift) /
                                         config_.tick_size) * config_.tick_size;
        const double passive_bid = std::floor(top.bid_price / config_.tick_size) * config_.tick_size;
        const double passive_ask = std::ceil(top.ask_price / config_.tick_size) * config_.tick_size;
        QuoteTargets targets{std::min(raw_bid, passive_bid), std::max(raw_ask, passive_ask), false};
        if (config_.quote_at_best_when_neutral && std::abs(inventory_ratio) < 0.25 &&
            std::abs(flow) < config_.toxicity_flow_threshold)
        {
            targets.bid = passive_bid;
            targets.ask = passive_ask;
        }
        if (config_.close_side_ticks > 0.0 && config_.tick_size > 0.0 && position_ != 0)
        {
            const double close_offset = config_.close_side_ticks * config_.tick_size;
            if (position_ > 0)
            {
                const double closing_ask =
                    std::ceil((fair_price + close_offset) / config_.tick_size) * config_.tick_size;
                targets.ask = std::max(passive_ask, std::min(targets.ask, closing_ask));
            }
            else
            {
                const double closing_bid =
                    std::floor((fair_price - close_offset) / config_.tick_size) * config_.tick_size;
                targets.bid = std::min(passive_bid, std::max(targets.bid, closing_bid));
            }
        }
        // The wind-down's price half: while it runs, the side that closes the position walks in towards the touch in
        // whole ticks as the clock advances -- up to `wind_down_skew_ticks` over the last minutes -- so the market meets
        // the reducing quote first. Whole ticks rather than a continuous distance because a price is on the grid: with a
        // one-tick budget on a three-tick quote the step lands at the halfway point, and a wider budget ramps in more
        // steps. The clamp keeps the quote outside the touch, and zero progress leaves the quote where it was.
        const double wind_down = wind_down_progress();
        if (wind_down > 0.0 && config_.wind_down_skew_ticks > 0.0 && config_.tick_size > 0.0 &&
            position_ != 0)
        {
            const auto moved = static_cast<uint64_t>(
                std::lround(wind_down * config_.wind_down_skew_ticks));
            if (moved > 0)
            {
                const double step = static_cast<double>(moved) * config_.tick_size;
                if (position_ > 0)
                    targets.ask = std::max(passive_ask, targets.ask - step);
                else
                    targets.bid = std::min(passive_bid, targets.bid + step);
            }
        }
        targets.valid = std::isfinite(targets.bid) && std::isfinite(targets.ask) && targets.bid < targets.ask;
        return targets;
    }

    // Move each side's primary quote **away** from the touch, in place, and never towards it.
    //
    // The asymmetry is the whole result, and it was measured the wrong way first: with every refresh amended, fills went
    // from 26 to 49 a window and the closed median from +9.83 to -5.92 bps, because the amended quote is always at the
    // fresh price and lands at the back of the level that price just cleared -- it is the mechanism that catches every
    // adverse print. The cancel path's short absence is a filter, and an amend removes it. Retreating is the case the
    // amend is for: one round trip, no gap at the level, and the quote is out of the way before the sweep arrives. An
    // advance keeps the cancel path, where the view it costs is a filter rather than a loss.
    //
    // A layer behind a primary is always left to the cancel path: it sits at a distance of its own, and moving it to the
    // primary's price would fold the ladder into one level. The local record takes the new price at once, because the
    // next view compares against it.
    void amend_resting_quotes(const TopOfBook& top, double fair_price)
    {
        const auto targets = target_prices(top, fair_price, position_ratio(), trade_imbalance());
        if (!targets.valid) return;
        const double deadband = std::max(1.0, config_.amend_deadband_ticks) * config_.tick_size;
        for (auto& entry : open_orders_)
        {
            StrategyOrder& order = entry.second;
            const bool buy = order.side == Side::Buy;
            const double target = buy ? targets.bid : targets.ask;
            const double primary_price = buy ? last_placed_bid_ : last_placed_ask_;
            if (std::abs(order.price - primary_price) > 0.5 * config_.tick_size) continue;
            if ((buy ? order.price - target : target - order.price) < deadband - 1e-12) continue;
            const auto total = order_totals_.find(entry.first);
            amend_requests_.push_back(AmendRequest{entry.first, target,
                                                   total != order_totals_.end() ? total->second
                                                                               : order.quantity});
            order.price = target;
            // The primary price the next refresh compares against is this one. Without this the second refresh in a row
            // cannot recognise the order it just moved -- `order.price` and `last_placed_*` disagree by the amend itself --
            // and the mechanism silently degrades to the cancels it replaced. Measured 2026-11-25 on UP 11-16: the
            // strategy's cancel count fell by 261 over 550 placements and the execution summary was identical to the
            // baseline, which is how a one-line omission reads as "the flag does nothing".
            if (buy)
                last_placed_bid_ = target;
            else
                last_placed_ask_ = target;
        }
    }

    std::vector<StrategyOrder> quote(const std::string& symbol, const TopOfBook& top,
                                     double fair_price)
    {
        std::vector<StrategyOrder> orders;
        if (top.bid_price <= 0.0 || top.ask_price <= 0.0 || top.bid_price >= top.ask_price ||
            config_.order_size == 0 || config_.tick_size <= 0.0)
        {
            cancel_pending_ = !open_orders_.empty();
            return orders;
        }

        // Inventory discipline, before anything else: a position above the band is reduced rather than carried.
        // The order is priced at the touch on the reducing side, which the engine fills as a taker at that price,
        // so the run pays the spread and the taker fee for the risk it is removing instead of keeping both. It is
        // sent even while quotes rest, because the position it is fixing is the one that already exists.
        if (config_.flatten_band_units > 0 && std::abs(position_) > config_.flatten_band_units)
        {
            const int64_t excess = std::abs(position_) - config_.flatten_band_units;
            const bool buy_to_reduce = position_ < 0;
            orders.emplace_back(buy_to_reduce ? Side::Buy : Side::Sell, symbol,
                                buy_to_reduce ? top.ask_price : top.bid_price,
                                static_cast<uint64_t>(excess), 0);
            ++config_.flatten_orders;
            config_.flatten_quantity += static_cast<uint64_t>(excess);
            last_fair_price_ = (top.bid_price + top.ask_price) / 2.0;
            last_market_mid_ = last_fair_price_;
            quote_age_ = 0;
            queue_activity_ = 0;
            queue_hold_count_ = 0;
            return orders;
        }

        if (!open_orders_.empty())
        {
            // needs_refresh records the reason and arms the cancel; the new quotes go out on the next
            // view, once the cancels have been acknowledged. With amend on, the primary of each side whose price simply
            // moved is moved in place instead, and the cancel list leaves those orders alone.
            const bool refresh = needs_refresh(top, fair_price);
            if (refresh)
            {
                // The state switch, when the run asked for it: amend only while the book is toxic, and keep the cancel
                // path while it is calm. The two branches are counted so an arm can say which one it actually ran.
                const bool amend_now = config_.amend_instead_of_cancel ||
                                       (config_.amend_on_toxicity && toxicity_state());
                if (config_.amend_on_toxicity)
                {
                    if (amend_now)
                        ++config_.toxicity_amends;
                    else
                        ++config_.toxicity_refreshes;
                }
                if (amend_now) amend_resting_quotes(top, fair_price);
            }
            return orders;
        }
        if (fair_price <= 0.0) fair_price = (top.bid_price + top.ask_price) / 2.0;

        const double inventory_ratio = position_ratio();
        uint64_t buy_quantity = config_.order_size;
        uint64_t sell_quantity = config_.order_size;
        if (position_ >= config_.inventory_limit) buy_quantity = 0;
        if (position_ <= -config_.inventory_limit) sell_quantity = 0;
        // The one-sided retreat: from a fraction of the cap the adding side stops opening, so the inventory is worked
        // down by the closing side alone instead of being carried into the mark. The hard cap above is this rule at
        // 1.0 and the fraction acts earlier; it is off at zero, which is what every recorded run asks for.
        if (config_.onesided_inventory_frac > 0.0 && config_.inventory_limit > 0)
        {
            const double opening_at =
                config_.onesided_inventory_frac * static_cast<double>(config_.inventory_limit);
            if (static_cast<double>(position_) >= opening_at)
                buy_quantity = 0;
            else if (static_cast<double>(position_) <= -opening_at)
                sell_quantity = 0;
        }
        // The size throttle: while the carried position is marked against us by at least `throttle_adverse_bps`, the
        // side that would ADD to it quotes `throttle_units` -- zero pauses that side entirely -- and the closing side
        // keeps its full size. It is the non-price answer to the question the wind-down's skew was refused for: remove
        // adverse flow without walking the exit into the book. Off at zero, so every recorded run stands.
        if (config_.throttle_adverse_bps > 0.0 && position_ != 0 &&
            -open_pnl_bps() >= config_.throttle_adverse_bps)
        {
            if (position_ > 0)
                buy_quantity = std::min(buy_quantity, config_.throttle_units);
            else
                sell_quantity = std::min(sell_quantity, config_.throttle_units);
            ++config_.throttled_quotes;
        }
        // The wind-down, before the other quantity rules so nothing further down can re-open the side it closes: in the
        // last minutes before the named end the position is worked by the closing side alone. Zero progress -- the
        // default -- leaves both sides exactly as they were.
        const double wind_down = wind_down_progress();
        if (wind_down > 0.0)
        {
            ++config_.wind_down_views;
            if (position_ > 0)
                buy_quantity = 0;
            else if (position_ < 0)
                sell_quantity = 0;
        }
        if (config_.flow_guard)
        {
            const double flow = trade_imbalance();
            if (flow > config_.toxicity_flow_threshold) sell_quantity = 0;
            if (flow < -config_.toxicity_flow_threshold) buy_quantity = 0;
            if (std::abs(flow) > config_.weak_flow_threshold &&
                std::abs(inventory_ratio) > config_.neutral_inventory_band)
            {
                const double weak_scale = std::max(0.0, config_.weak_flow_quote_scale);
                buy_quantity = static_cast<uint64_t>(std::max(
                    0.0, static_cast<double>(buy_quantity) * weak_scale));
                sell_quantity = static_cast<uint64_t>(std::max(
                    0.0, static_cast<double>(sell_quantity) * weak_scale));
            }
        }
        // A scaled-down quote still has to be a size the venue accepts. Zero stays zero: that side is
        // deliberately paused, not shrunk.
        const auto at_least_minimum = [this](uint64_t quantity)
        {
            return quantity == 0 ? uint64_t{0} : std::max(quantity, config_.min_order_quantity);
        };
        buy_quantity = at_least_minimum(buy_quantity);
        sell_quantity = at_least_minimum(sell_quantity);
        // The prices come from the one function that computes them, and every gate below reads them without moving
        // them: the closing side, the clamp outside the touch and the neutral quote are inside `target_prices`.
        const auto targets = target_prices(top, fair_price, inventory_ratio, trade_imbalance());
        if (!targets.valid) return orders;
        double bid_price = targets.bid;
        double ask_price = targets.ask;

        // The conditional rule, applied before the price gate because it is about the situation rather than
        // about the quote: a one tick book cannot offer anything but the touch, and the touch is the fill that
        // resolves negative. Refusing it is a decision, and it is counted so the refusal can be attributed.
        if (config_.min_spread_ticks > 0.0 &&
            (top.ask_price - top.bid_price) / config_.tick_size < config_.min_spread_ticks)
        {
            if (buy_quantity > 0 || sell_quantity > 0) ++config_.min_spread_skips;
            buy_quantity = 0;
            sell_quantity = 0;
        }

        // The expected-value gate, applied per side. The edge a fill would capture is the distance from the
        // reference price, and the cost it has to cover is the maker fee expressed in ticks at that price,
        // plus, when the run asked for it, what passive fills at that distance have actually cost.
        const double reference_price = 0.5 * (top.bid_price + top.ask_price);
        // The fee a fill has to cover, in ticks: the rate on the reference notional, plus a commission
        // charged for the unit itself, which is how a share and a listed future are billed and which a rate
        // cannot express. The tick is worth tick_size * multiplier in USD, so dividing the commission by that
        // puts it in the same units as the edge. An instrument with no commission leaves the second term
        // exactly zero, so no recorded run moves.
        const double tick_usd =
            config_.tick_size * (config_.multiplier > 0.0 ? config_.multiplier : 1.0);
        const double fee_ticks = reference_price > 0.0 && config_.tick_size > 0.0
                                     ? config_.maker_fee_rate * reference_price / config_.tick_size +
                                           (tick_usd > 0.0 ? config_.fee_per_unit / tick_usd : 0.0)
                                     : 0.0;
        // A measured markout is a signed number, and the gate only cares about the cost: a distance whose
        // passive fills have been profitable so far needs no cover beyond the fee, and one whose fills have
        // lost money needs the loss covered. The bucket is the same one the execution quality line reports, so
        // the two numbers can be read against each other.
        const auto adverse_ticks_at = [&](double distance_ticks, bool buy) {
            const bool far = distance_ticks >= 1.0;
            const double measured = buy ? (far ? markout_estimate_.buy_far_ticks
                                               : markout_estimate_.buy_near_ticks)
                                        : (far ? markout_estimate_.sell_far_ticks
                                               : markout_estimate_.sell_near_ticks);
            return std::max(0.0, -measured);
        };
        const double buy_adverse_ticks =
            config_.markout_cover && markout_estimate_.samples >= config_.markout_cover_min_samples
                ? adverse_ticks_at((reference_price - bid_price) / config_.tick_size, true)
                : 0.0;
        const double sell_adverse_ticks =
            config_.markout_cover && markout_estimate_.samples >= config_.markout_cover_min_samples
                ? adverse_ticks_at((ask_price - reference_price) / config_.tick_size, false)
                : 0.0;
        const double required_buy_edge_ticks =
            fee_ticks + config_.edge_cover_ticks + buy_adverse_ticks;
        const double required_sell_edge_ticks =
            fee_ticks + config_.edge_cover_ticks + sell_adverse_ticks;
        // Either setting turns the gate on; both at zero leave it off, which is what every recorded run asks
        // for, so the documented numbers do not move.
        if (config_.edge_cover_ticks > 0.0 || config_.markout_cover)
        {
            if ((reference_price - bid_price) / config_.tick_size < required_buy_edge_ticks)
            {
                if (buy_quantity > 0) ++config_.edge_gate_skips;
                if (buy_quantity > 0 && buy_adverse_ticks > 0.0) ++config_.markout_gate_skips;
                buy_quantity = 0;
            }
            if ((ask_price - reference_price) / config_.tick_size < required_sell_edge_ticks)
            {
                if (sell_quantity > 0) ++config_.edge_gate_skips;
                if (sell_quantity > 0 && sell_adverse_ticks > 0.0) ++config_.markout_gate_skips;
                sell_quantity = 0;
            }
        }

        // The asymmetric refresh, applied after every other gate because it is about which side may go out now
        // rather than about whether to quote: a side that would move towards the market is held back until the fair
        // price has been still for the debounce, and a side that retreats is sent at once. Zero leaves the symmetric
        // behaviour every recorded run was measured with.
        if (config_.advance_debounce_ms > 0 && last_view_ts_ > 0 &&
            (buy_quantity > 0 || sell_quantity > 0))
        {
            // The debounce is milliseconds of *book* time: the view clock is milliseconds on an OKX cut and
            // microseconds elsewhere, so the setting is turned into the clock's own unit. Measured 2026-11-26: without
            // this, a `--advance-debounce-ms=40` on the OKX cuts waited forty *seconds* for the fair price to settle,
            // which is the whole frozen cell's asymmetric-refresh arm reading a rule it was not labelled with.
            const uint64_t debounce_ts =
                config_.advance_debounce_ms * clock_units_per_second(last_view_ts_) / 1'000ULL;
            const bool settled =
                fair_change_ts_ == 0 || last_view_ts_ - fair_change_ts_ >= debounce_ts;
            const double half_tick = 0.5 * config_.tick_size;
            // The wind-down's closing side is exempt: its move towards the touch is not a chase, it is a deadline, and the
            // debounce would otherwise hold the skew back for as long as the fair price keeps moving -- which on a
            // two-hour book is most of the last fifteen minutes. Only a run that asked for the wind-down is affected, so
            // every arm measured without it reads exactly what it did.
            const bool closing_buy = wind_down > 0.0 && position_ < 0;
            const bool closing_sell = wind_down > 0.0 && position_ > 0;
            if (!settled)
            {
                if (!closing_buy && buy_quantity > 0 && last_placed_bid_ > 0.0 &&
                    bid_price > last_placed_bid_ + half_tick)
                {
                    ++config_.advance_skips;
                    buy_quantity = 0;
                }
                if (!closing_sell && sell_quantity > 0 && last_placed_ask_ > 0.0 &&
                    ask_price < last_placed_ask_ - half_tick)
                {
                    ++config_.advance_skips;
                    sell_quantity = 0;
                }
            }
        }

        // The depth cap (--max-depth-ratio): no rung may be larger than a share of the same side's displayed touch size,
        // and a size above the cap is split into rungs one step behind each other rather than sent as one large order the
        // book has to absorb. `rungs_for` returns one rung of the whole size when the ratio is off, so a run that does
        // not ask reads exactly what it did.
        const auto depth_cap = [&](uint64_t want, bool buy) -> uint64_t
        {
            if (config_.max_depth_ratio <= 0.0) return want;
            const uint64_t displayed = buy ? top.bid_size : top.ask_size;
            const auto share =
                static_cast<uint64_t>(config_.max_depth_ratio * static_cast<double>(displayed));
            return std::max(config_.min_order_quantity, share);
        };
        const auto rungs_for = [&](uint64_t want, uint64_t cap) -> std::pair<uint64_t, uint64_t>
        {
            if (cap == 0 || want <= cap) return {1, want};
            const uint64_t needed = (want + cap - 1) / cap;
            const uint64_t rungs = std::min(std::max<uint64_t>(1, config_.depth_ladder_max), needed);
            return {rungs, cap};
        };
        const auto [buy_rungs, buy_rung_quantity] = rungs_for(buy_quantity, depth_cap(buy_quantity, true));
        const auto [sell_rungs, sell_rung_quantity] = rungs_for(sell_quantity, depth_cap(sell_quantity, false));
        if (config_.max_depth_ratio > 0.0)
        {
            const uint64_t buy_placed = buy_quantity == 0 ? 0 : std::min(buy_quantity, buy_rungs * buy_rung_quantity);
            const uint64_t sell_placed =
                sell_quantity == 0 ? 0 : std::min(sell_quantity, sell_rungs * sell_rung_quantity);
            config_.depth_capped_rungs += (buy_quantity > 0 ? buy_rungs : 0) + (sell_quantity > 0 ? sell_rungs : 0);
            config_.depth_unquoted_quantity += (buy_quantity - buy_placed) + (sell_quantity - sell_placed);
        }
        const double depth_step = config_.tick_size;

        if (buy_quantity > 0)
        {
            for (uint64_t rung = 0; rung < buy_rungs; ++rung)
            {
                // Never more than the cap a rung: when the allowed rungs cannot hold the whole size the excess is
                // dropped *and counted*, because a ladder whose last rung is twice the cap is the fixed size again.
                const uint64_t quantity =
                    std::min(buy_rung_quantity, buy_quantity - buy_rung_quantity * rung);
                if (quantity == 0) break;
                const double price = bid_price - static_cast<double>(rung) * depth_step;
                if (price <= 0.0) break;
                orders.emplace_back(Side::Buy, symbol, price, quantity, 0);
            }
            // The primary is what the next requote is compared against, whatever the layers behind it did.
            last_placed_bid_ = bid_price;
        }
        if (sell_quantity > 0)
        {
            for (uint64_t rung = 0; rung < sell_rungs; ++rung)
            {
                const uint64_t quantity =
                    std::min(sell_rung_quantity, sell_quantity - sell_rung_quantity * rung);
                if (quantity == 0) break;
                orders.emplace_back(Side::Sell, symbol, ask_price + static_cast<double>(rung) * depth_step,
                                    quantity, 0);
            }
            last_placed_ask_ = ask_price;
        }
        for (uint64_t layer = 1; layer <= config_.extra_layers; ++layer)
        {
            // The layer's own step: one tick by default, or a basis-point distance when the run asks for one. The tick
            // form is the collapse the fine-tick instruments show -- a 0.76 bps tick puts "one more tick" almost on the
            // first layer's price -- so the basis-point form is rounded outward to the grid and floored at one tick.
            double step = static_cast<double>(layer) * config_.tick_size;
            if (config_.layer_step_bps > 0.0 && config_.tick_size > 0.0 && fair_price > 0.0)
            {
                const double asked = static_cast<double>(layer) * config_.layer_step_bps / 1e4 * fair_price;
                step = std::max(1.0, std::ceil(asked / config_.tick_size)) * config_.tick_size;
            }
            if (buy_quantity > 0 && bid_price - step > 0.0)
                orders.emplace_back(Side::Buy, symbol, bid_price - step, buy_quantity, 0);
            if (sell_quantity > 0)
                orders.emplace_back(Side::Sell, symbol, ask_price + step, sell_quantity, 0);
        }
        if (!orders.empty())
        {
            last_fair_price_ = fair_price;
            last_market_mid_ = (top.bid_price + top.ask_price) / 2.0;
            quote_age_ = 0;
            queue_activity_ = 0;
            queue_hold_count_ = 0;
        }
        return orders;
    }

    struct TradeVolume
    {
        Side side;
        uint64_t quantity;
    };

    L2MarketMakerBaseConfig config_;
    std::unordered_map<uint64_t, StrategyOrder> open_orders_;
    // What each working order was sent for, because `open_orders_` holds what is *left* and a venue recomputes the
    // remainder from the total: an amend built from the remaining quantity is declined once anything has filled.
    std::unordered_map<uint64_t, uint64_t> order_totals_;
    // Amends this view asked for, consumed by the pipeline on the same cycle as the cancels.
    std::vector<AmendRequest> amend_requests_;
    SubmittedOrders submitted_orders_;
    std::deque<TradeVolume> recent_trades_;
    uint64_t buy_volume_{0};
    uint64_t sell_volume_{0};
    int64_t position_{0};
    // The carried position's volume-weighted entry and the mid of the last view: together they answer "is this tail marked
    // against us", which is the condition the conditional wind-down reads (`open_pnl_bps`).
    double average_entry_{0.0};
    double last_view_mid_{0.0};
    bool cancel_pending_{false};
    uint64_t quote_age_{0};
    double last_fair_price_{0.0};
    double last_market_mid_{0.0};
    uint64_t queue_activity_{0};
    uint64_t queue_hold_count_{0};
    // The view clock and the last time the fair price moved, which is what the asymmetric refresh measures.
    uint64_t last_view_ts_{0};
    uint64_t fair_change_ts_{0};
    // What the last requote actually sent per side, so the next one can tell chasing from retreating.
    double last_placed_bid_{0.0};
    double last_placed_ask_{0.0};
    StrategyMetrics metrics_;
    MarkoutEstimate markout_estimate_;

   public:
    // How often the decision layer refused to quote, so a run can report what it declined to do rather than only
    // what it did. Exposed through accessors because the counters themselves live in the configuration.
    [[nodiscard]] uint64_t edge_gate_skips() const { return config_.edge_gate_skips; }
    [[nodiscard]] uint64_t min_spread_skips() const { return config_.min_spread_skips; }
    [[nodiscard]] uint64_t advance_skips() const { return config_.advance_skips; }
    [[nodiscard]] uint64_t markout_gate_skips() const override { return config_.markout_gate_skips; }
    [[nodiscard]] uint64_t depth_capped_rungs() const { return config_.depth_capped_rungs; }
    [[nodiscard]] uint64_t depth_unquoted_quantity() const { return config_.depth_unquoted_quantity; }
    [[nodiscard]] uint64_t flatten_orders() const override { return config_.flatten_orders; }
    [[nodiscard]] uint64_t flatten_quantity() const override { return config_.flatten_quantity; }
};
