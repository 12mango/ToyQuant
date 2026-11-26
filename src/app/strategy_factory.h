#pragma once

#include <memory>
#include <string>

#include "app/trend_params.h"
#include "common/instrument_spec.h"
#include "lines/legacy/l1_market_maker_config.h"
#include "strategy/execution/execution_l2.h"
#include "strategy/strategy.h"

// Everything only the frozen L1 line reads. Five flat fields became one member so that the live
// configuration does not sit beside knobs it never touches, and so that the one place an L1 type is
// visible in this header is a single name a reader can skip. `FlowAwareMarketMakerConfig` still comes
// from `lines/legacy/`, because the type belongs to that line: moving it here would be a change to the
// frozen code rather than to this seam.
struct L1StrategyConfig
{
    FlowAwareMarketMakerConfig flow_config{};
    double risk_threshold = 0.60;
    double stress_spread_multiplier = 2.5;
    double minimum_stress_quantity_ratio = 0.10;
    double fee_spread_multiplier = 0.40;
};

// Everything make_strategy can be told. These used to be eleven positional arguments, seven of them
// doubles, so a call site could transpose two of them and still compile: the four L1 knobs are all
// fractions and the two overrides are both counts. Named members make that mistake impossible to write.
struct StrategyFactoryConfig
{
    const InstrumentSpec* instrument = nullptr;

    // The frozen L1 line's own settings. A run that selects an L1 name reads them; nothing else does.
    L1StrategyConfig l1{};

    // Overrides handed to the L2 configs, where 0 means "keep the strategy's own default".
    uint64_t order_size_override = 0;
    int64_t inventory_limit_override = 0;
    double base_spread_ticks_override = 0.0;
    // The same distance in basis points of the reference price, from --quote-distance-bps; the strategy turns it into
    // `max(2, round(bps / tick_bps))` ticks. 0 leaves the tick-count form.
    double base_spread_bps = 0.0;
    // The adding side stops at this fraction of the inventory cap, from --onesided-inventory-frac. 0 quotes both sides.
    double onesided_inventory_frac = 0.0;
    // The step between extra layers in basis points, from --layer-step-bps; 0 leaves one tick a layer.
    double layer_step_bps = 0.0;
    // From --amend-instead-of-cancel: the strategy moves a primary quote in place rather than pulling and placing it.
    bool amend_instead_of_cancel = false;
    // From --amend-deadband-ticks: how far the target must retreat before that amend fires; 0 leaves the strategy's 2.0.
    double amend_deadband_ticks = 0.0;
    // From --amend-on-toxicity: amend only while the book is toxic (a short-window range or one-sided flow).
    bool amend_on_toxicity = false;
    // From --toxicity-imbalance: how one-sided the recent flow must be for that state; 0 leaves the strategy's 0.9.
    double toxicity_imbalance = 0.0;
    // From --wind-down-skew-minutes / --wind-down-end / --wind-down-skew-ticks: the maker's own inventory skew over the
    // last minutes before a named session end. Zero minutes leaves both sides quoting.
    uint64_t wind_down_skew_minutes = 0;
    double wind_down_end_minute = 0.0;
    double wind_down_skew_ticks = 1.0;
    // From --pause-ticks-per-second: the active halt's threshold; 0 leaves the strategy's own 9.0.
    double pause_ticks_per_second = 0.0;
    // The inventory-aware strategy's quote shift at a full inventory, in ticks. 0 leaves its own default.
    double inventory_shift_ticks_override = 0.0;
    uint64_t refresh_price_ticks_override = 0;
    // The decision layer's expected-value gate: extra ticks a quote has to cover on top of the maker fee.
    // 0 leaves the gate off, which is what every run recorded in the documents asks for.
    double edge_cover_ticks_override = 0.0;
    // The adverse-selection gate: the cover a quote needs includes what passive fills at its distance have
    // actually cost in this window, as measured from the venue's own prints. Off by default.
    bool markout_cover = false;
    // Inventory discipline: above this many quantity units the L2 decision layer reduces the position with a
    // taker order at the touch instead of carrying it. 0 leaves every position held to the end.
    int64_t flatten_band_units = 0;
    // The conditional rule: quote only when the top of book is at least this many ticks wide. 0 leaves it off.
    double min_spread_ticks_override = 0.0;
    // The forced-flow cooldown: a mid-price range of `jump_ticks` inside `jump_window_ms` pulls the quotes and holds
    // them off for `jump_cooldown_ms`. Any of the three at zero leaves it off.
    uint64_t jump_window_ms = 0;
    double jump_ticks = 0.0;
    uint64_t jump_cooldown_ms = 0;
    // The asymmetric refresh: only the side that would move towards the market waits for the fair to settle. 0 leaves
    // the symmetric behaviour.
    uint64_t advance_debounce_ms = 0;
    // The closing side's distance while the position is not flat, in ticks; 0 leaves the symmetric distance.
    double close_side_ticks_override = 0.0;
    // Extra passive layers behind the primary quote; 0 leaves one layer a side.
    uint64_t extra_layers = 0;
    // The non-market-making signal, from --trend-params. `set` false leaves the strategy's own defaults.
    TrendParams trend{};
    // Where that line's entry is priced, from --trend-entry. The defaults are the touch entry every recorded run had.
    TrendEntryParams trend_entry{};
    // The execution line's parent order, from --parent and --algo. A side of Unknown means no parent, which is
    // what every strategy name except execution_l2 expects.
    struct ExecutionParams
    {
        Side side{Side::Unknown};
        uint64_t quantity{0};
        uint64_t horizon_us{0};
        ExecutionAlgo algo{ExecutionAlgo::Market};
        uint64_t slices{10};
        double participation{0.10};
        // Post-first entry: rest at the near touch and cross what the slice has not filled after the timeout.
        bool post_first{false};
        uint64_t post_timeout_us{0};
    } execution{};
};

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const StrategyFactoryConfig& config = {});
