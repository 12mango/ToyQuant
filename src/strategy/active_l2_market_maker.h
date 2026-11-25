#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "strategy/l2_market_maker_base.h"

struct ActiveL2MarketMakerConfig
{
    L2MarketMakerBaseConfig base;
    double volatility_alpha{0.25};
    // The halt compares this against a mid-price speed in ticks per second, but that speed is not
    // usable at this feed rate: view gaps here range from microseconds to hundreds of milliseconds, so
    // a speed built from a discrete tick move measures the gap distribution rather than the market, and
    // `min_elapsed_seconds` below dominates the result. Measured: with the effective threshold at 9.0
    // the halt fires 46 times in the 200,000-row window, at 50.0 it fires 246 times, and every value
    // from 1.0 to 50.0 lands in the same place. The threshold that would mean something is a tick range
    // over a fixed time window, which is a behaviour change to a risk control -- and it was measured before
    // being built: over five days of half-hour slices both halves of the median lose money a fill and the
    // quiet half loses less, so a floor would remove the cheaper minutes and none is built
    // (`tools/study/slice_pnl.py`; the studies page has the run).
    double pause_after_ticks_per_second{9.0};
    // The floor that makes the speed above rate-dependent. See the note on the threshold.
    double min_elapsed_seconds{0.05};
    // The forced-flow cooldown, which is a different measurement from the speed above: a mid-price move of at least
    // `jump_ticks` inside `jump_window_ms` is a jump rather than a drift, and the quotes are pulled and held off for
    // `jump_cooldown_ms` so the sweep's momentum passes before a quote goes out again. The speed gate reads a rate over
    // seconds and this one reads a range over tens of milliseconds, which is why both exist; any of the three at zero
    // leaves the cooldown off, and that is what every recorded run asks for.
    uint64_t jump_window_ms{0};
    double jump_ticks{0.0};
    uint64_t jump_cooldown_ms{0};
    uint64_t warmup_trades{0};
    uint64_t warmup_views{0};
};

// Adds a volatility halt to the L2 maker.
//
// It derives from L2MarketMakerBase instead of holding one and forwarding each callback. A wrapper has to
// hand-forward every virtual of the base, and a forgotten forward changes behaviour silently rather
// than failing to compile: InventoryAwareL2MarketMaker, which was written the other way, never
// forwarded on_queue_activity, which disabled the base's queue-hold refresh policy and its
// queue-consumption counters at the same time.
class ActiveL2MarketMaker final : public L2MarketMakerBase
{
   public:
    explicit ActiveL2MarketMaker(ActiveL2MarketMakerConfig config)
        : L2MarketMakerBase(config.base), config_(config)
    {
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        update_volatility(top);
        if (should_pause(0)) return pause();
        return L2MarketMakerBase::on_top_of_book(symbol, top);
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        update_volatility(view.top, view.ts);
        update_jump(view.top, view.ts);
        if (view.top.bid_price > 0.0 && view.top.ask_price > 0.0) ++valid_views_;
        if (!warmup_complete()) return {};
        if (should_pause(view.ts)) return pause();
        return L2MarketMakerBase::on_l2_market_view(view);
    }

    void on_market_trade(const MarketTrade& trade) override
    {
        L2MarketMakerBase::on_market_trade(trade);
        ++market_trades_;
    }

    StrategyMetrics metrics() const override
    {
        auto result = L2MarketMakerBase::metrics();
        result.risk_pause_count = risk_pause_count_;
        result.edge_gate_skips = edge_gate_skips();
        result.min_spread_skips = min_spread_skips();
        return result;
    }

    double volatility_ticks() const
    {
        return volatility_ticks_;
    }

   private:
    bool should_pause(uint64_t ts) const
    {
        if (volatility_ticks_ >= config_.pause_after_ticks_per_second) return true;
        // The cooldown is a property of the clock: a quote that goes out during the sweep is the fill this rule
        // exists to refuse, so the pause holds until the window it was armed for has passed.
        return ts > 0 && ts < jump_cooldown_until_;
    }

    // The jump detector: how far the mid has travelled inside the last `jump_window_ms`, compared against
    // `jump_ticks`. A range rather than a rate, because a speed over this feed's gaps measures the gaps (see the note
    // on `pause_after_ticks_per_second`), and a range over a fixed window is the measurement the rule asks for.
    void update_jump(const TopOfBook& top, uint64_t ts)
    {
        if (config_.jump_window_ms == 0 || config_.jump_ticks <= 0.0 || ts == 0 ||
            config_.jump_cooldown_ms == 0)
            return;
        if (top.bid_price <= 0.0 || top.ask_price <= 0.0 || config_.base.tick_size <= 0.0) return;
        jump_samples_.emplace_back(ts, (top.bid_price + top.ask_price) / 2.0);
        const uint64_t window_us = config_.jump_window_ms * 1000ULL;
        while (!jump_samples_.empty() && ts - jump_samples_.front().first > window_us)
            jump_samples_.pop_front();
        // A bound on memory rather than on the rule: a window of tens of milliseconds holds a few samples, and this
        // only matters for a feed whose timestamps collapse.
        while (jump_samples_.size() > 256) jump_samples_.pop_front();
        double low = jump_samples_.front().second;
        double high = low;
        for (const auto& sample : jump_samples_)
        {
            low = std::min(low, sample.second);
            high = std::max(high, sample.second);
        }
        if ((high - low) / config_.base.tick_size < config_.jump_ticks) return;
        const uint64_t until = ts + config_.jump_cooldown_ms * 1000ULL;
        if (until > jump_cooldown_until_)
        {
            jump_cooldown_until_ = until;
            ++jump_pauses_;
        }
    }

    bool warmup_complete() const
    {
        return market_trades_ >= config_.warmup_trades && valid_views_ >= config_.warmup_views;
    }

    std::vector<StrategyOrder> pause()
    {
        if (working_order_count() > 0) ++risk_pause_count_;
        request_cancel_all();
        return {};
    }

    void update_volatility(const TopOfBook& top, uint64_t ts = 0)
    {
        if (top.bid_price <= 0.0 || top.ask_price <= 0.0 || config_.base.tick_size <= 0.0)
            return;

        const double mid = (top.bid_price + top.ask_price) / 2.0;
        if (last_mid_ > 0.0)
        {
            const double move_ticks = std::abs(mid - last_mid_) / config_.base.tick_size;
            double normalized_move_ticks = move_ticks;
            if (ts > 0 && last_ts_ > 0 && ts > last_ts_)
            {
                const double elapsed_seconds =
                    static_cast<double>(ts - last_ts_) / 1000000.0;
                normalized_move_ticks =
                    move_ticks /
                    std::max(elapsed_seconds, std::max(config_.min_elapsed_seconds, 0.0));
            }
            volatility_ticks_ = config_.volatility_alpha * normalized_move_ticks +
                                (1.0 - config_.volatility_alpha) * volatility_ticks_;
        }
        last_mid_ = mid;
        last_ts_ = ts;
    }

    ActiveL2MarketMakerConfig config_;
    double last_mid_{0.0};
    uint64_t last_ts_{0};
    double volatility_ticks_{0.0};
    uint64_t risk_pause_count_{0};
    // The cooldown's own state: the samples inside the window, when it ends, and how often it was armed.
    std::deque<std::pair<uint64_t, double>> jump_samples_;
    uint64_t jump_cooldown_until_{0};
    uint64_t jump_pauses_{0};
    uint64_t market_trades_{0};
    uint64_t valid_views_{0};
};
