#pragma once

#include <cstdint>

// The view clock's unit, in ticks per second, derived from the timestamp's magnitude.
//
// The two data sources in this repository disagree: an OKX book window prints milliseconds (`1794834900007`, 1.7e12
// today) while the Deribit fixtures print microseconds (1.7e15), and the unit tests use small microsecond clocks. A
// time-based setting -- `--advance-debounce-ms` on the maker, a parent's horizon on the execution line -- is therefore
// only meaningful once the clock it is compared against is known. Measured 2026-11-26, both ways it goes wrong:
// `--advance-debounce-ms=40` against a millisecond clock held every advance for forty *seconds* of book time, and a
// 7,200-second parent on a millisecond clock became an 8.3-day slice interval that sent one child in two hours.
//
// The thresholds are the units and not a guess: anything at or above 1e11 is milliseconds, because 1e11 milliseconds
// is 1973 and every millisecond clock in use is far above it, while a microsecond clock for the same era is 1e14 and
// up. A seconds clock is not represented here because no source in this repository emits one.
inline uint64_t clock_units_per_second(uint64_t ts)
{
    return ts >= 100'000'000'000ULL ? 1'000ULL : 1'000'000ULL;
}
