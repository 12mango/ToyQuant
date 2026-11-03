# Troubleshooting

*Part of the [User Guide](../USER_GUIDE.md).*

The failures that waste an afternoon, and what each one means.

## Troubleshooting

- Run commands from the repository root so relative paths resolve correctly.
- Check that the input CSV exists and uses the expected tick format.
- Use a non-negative integer for the delay and a UDP port between `1` and `65535`.
- If there are no trades, try `shock_ticks.csv` or `random_ticks.csv`; a quiet scenario may not cross the strategy's orders.

### When a file is refused

Every reader names the row it refused and what is wrong with it, so a bad row in a large file is one `sed -n
'<row>p'` away rather than a search: `Error: row 4000123: price must be finite and positive: nan`. The rules are the
same in every family -- a price has to be finite and positive (so `nan`, `inf`, `0` and `-1` are refused where they
are read, not where they are used), a quantity has to be finite and non-negative, a timestamp and an id have to be
unsigned integers, and a side has to be one of the two spellings the venue publishes. A file whose header is missing
is refused at open time, naming the column that is absent; a header-only file is empty rather than broken.

Two things a *well formed* file can still do are worth knowing about, because both used to be silent:

- **A depth input that delivers no depth events is an error.** An empty or header-only depth file used to produce a
  formatted run of zeros and an exit code of zero -- a measurement of nothing that read like a result. In a run that
  trades several instruments, a book that was given no market events is refused the same way, because that means its
  file named the wrong symbol.
- **A crossed book is counted.** A snapshot whose best bid is at or above its best ask is refused (`L2 snapshot is
  crossed`), because it is not a market anyone can trade. An incremental batch that leaves the book crossed is
  applied and skipped at the top instead, and counted on the `[L2 DATA]` line as `crossed_book_batches=`. The field is
  printed only when the count is non-zero, so a run whose depth had none keeps the line it always had -- the 200k
  window this repository quotes every L2 number from has none.
- **The prints are scored against the book, and the line says how it went.** `stale_trades=` counts prints that
  arrived with no midpoint to compare them to -- before the book was two-sided, or more than a second after its last
  update -- and `dislocated_trades=` counts the prints more than five basis points from a *current* midpoint. Both are
  printed only when non-zero, and neither drops the print: on a depth book a print far from the midpoint is a sweep,
  not a bad tick, and filtering it would move the fills instead of describing them. On the 200k window the two
  numbers read `stale_trades=117889 dislocated_trades=9`: the depth slice covers quarter of an hour while the trade
  file covers the day, so almost every print is scored as stale, and the nine that were scored were all inside six
  basis points of the midpoint. That is what the fields are for -- telling a window whose inputs cover the same span
  apart from one whose depth stopped early.

For implementation details, see [Architecture](../ARCHITECTURE.md). Data file details are documented
