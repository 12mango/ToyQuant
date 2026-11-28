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
// **The boundary is one decade above any plausible millisecond clock, and it was wrong until 2026-11-29.** The first
// version asked `ts >= 1e11` and returned milliseconds, which is true of a millisecond clock (1.7e12) *and* of a
// microsecond one (1.7e15) -- every microsecond epoch is above 1e11 -- so the Deribit sources were read as milliseconds
// and every duration on them came out a thousand times short. A clock below 1e11 is a fixture counting from zero and is
// read as microseconds, which is what the unit tests assert.
inline uint64_t clock_units_per_second(uint64_t ts)
{
    if (ts >= 100'000'000'000'000ULL) return 1'000'000ULL;  // 1e14 microseconds is 1973: a microsecond epoch
    if (ts >= 100'000'000'000ULL) return 1'000ULL;          // 1e11 milliseconds is 1973, 1e14 is the year 5138
    return 1'000'000ULL;                                    // a fixture counting from zero
}

// The same instant in seconds. This exists because a duration divided out of a raw event clock is where the tree keeps
// getting the unit wrong, and a constant divisor is right for exactly one of the two sources. Measured 2026-11-29: the
// risk pipeline's token bucket refilled with `ts / 1'000'000`, which on an OKX cut (milliseconds) paid **144 tokens in a
// two-hour window instead of 144,000** -- so a session that asked 11,429 times was refused 11,086 of them, and the
// journal read as a strategy asking for 23 orders a second when it was asking for 1.59.
inline double seconds_of(uint64_t ts)
{
    return static_cast<double>(ts) / static_cast<double>(clock_units_per_second(ts));
}

// A duration a flag names in microseconds (a latency), in the units of the clock it is added to.
//
// Everywhere else the tree converts: the maker's debounce, the wind-down's window, `trend_l2`'s horizon and the execution
// line's all go through `clock_units_per_second`. The matching engine did not, so `--order-latency-us=100000` added
// 100,000 *milliseconds* to an OKX order -- a hundred seconds -- and the cell's own "10 ms" arm was ten seconds. The
// flag's unit is the flag's; the clock's is the clock's, and this is the line between them.
inline uint64_t clock_units_of_us(uint64_t ts, uint64_t us)
{
    if (us == 0) return 0;
    return us * clock_units_per_second(ts) / 1'000'000ULL;
}
