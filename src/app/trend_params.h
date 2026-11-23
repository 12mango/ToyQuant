#pragma once

#include <cstdint>

// The non-market-making signal's knobs, in one place because both the command line and the factory need the
// type. `set` is what keeps the strategy's own defaults in `TrendL2Config`: a run that does not name
// --trend-params leaves every number here unread rather than copying today's defaults into a second place
// that would drift from them.
struct TrendParams
{
    uint64_t fast_views{10};
    uint64_t slow_views{60};
    double entry_ticks{2.0};
    double exit_ticks{0.5};
    double stop_ticks{6.0};
    uint64_t exit_cooldown_views{60};
    uint64_t max_holding_views{2000};
    bool set{false};
};

// Where the non-market-making line's entry is priced, from `--trend-entry`. The defaults are the behaviour every
// recorded trend run had: the opposite touch, a taker fill, and an order that lives until it fills or the market moves
// away from it. `own_side` puts the clip on our own side of the book, `improve_ticks` inside the touch and never
// through it; `max_age_views` cancels an entry nobody hit and `cooldown_views` is the wait afterwards.
struct TrendEntryParams
{
    bool own_side{false};
    double improve_ticks{1.0};
    uint64_t max_age_views{0};
    uint64_t cooldown_views{0};
};
