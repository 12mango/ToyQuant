# Troubleshooting

*Part of the [User Guide](../USER_GUIDE.md).*

One table, ordered by how often it is the answer. Each row names the fix rather than the theory.

| symptom | likely cause | fix |
|---|---|---|
| `fills=0` behind a live book | sizing defaults to one contract, or the queue model is the bound | `--order-size` and `--inventory-limit`, then rerun with `conservative` as the seventh positional |
| `[VENUE] dropped>0`, or `orders>0` with no reports | the relay misread a frame, or the venue refused it | rerun with `--order-dump=FILE` and read the venue's own words ([live page](04-live-and-carry.md)) |
| `[RISK]` rejections climb | the fat-finger band or the rate bucket is tighter than the quote | widen `--fat-finger-bps` or `--max-orders-per-second`, and check the quote is inside the band |
| OKX `50026` on an order | the demo account is not in margin mode | switch it to single-currency margin (`acctLv=2`) |
| OKX `60012` | an `op: order` sent to the business route | place over REST (`--order-rest`); the private route is for the `orders` channel |
| a sandbox fill is 100x the size meant | the relay's contract value and the engine's scale do not describe the same instrument | `ctVal` is 0.01 BTC for `BTC-USDT-SWAP`: the relay converts engine units to contracts from `--contract-size` and `--quantity-scale`, and refuses a size below one contract |
| `check_docs` red | a recorded number moved | reproduce the command, paste the new output, prove the diff -- never edit the number |
| a run refuses a file | a bad row or a missing column | the reader names the row: `sed -n '<row>p'` on the input |

Runtime hints that are not failures: run from the repository root so relative paths resolve; the delay is a
non-negative integer and a UDP port is between 1 and 65535; a quiet scenario may simply never cross the strategy's orders.

## When a file is refused

Every reader names the row it refused and what is wrong with it, so a bad row in a large file is one `sed -n '<row>p'`
away rather than a search: `Error: row 4000123: price must be finite and positive: nan`. The rules are the same in every
family: a price is finite and positive (so `nan`, `inf`, `0` and `-1` are refused where they are read), a quantity is
finite and non-negative, a timestamp and an id are unsigned integers, and a side is one of the two spellings the venue
publishes. A missing header is refused at open time naming the absent column; a header-only file is empty rather than
broken.

/// details | What a well-formed file can still do, and the venue's error codes

- **A depth input that delivers no depth events is an error.** An empty or header-only depth file used to produce a
  formatted run of zeros and an exit code of zero. A two-book run refuses a book that was given no market events the same
  way, because that means its file named the wrong symbol.
- **A crossed book is counted.** A snapshot whose best bid is at or above its best ask is refused; an incremental batch
  that leaves the book crossed is applied and skipped at the top, counted as `crossed_book_batches=` and printed only when
  non-zero.
- **The prints are scored against the book.** `stale_trades=` counts prints with no midpoint to compare them to and
  `dislocated_trades=` counts prints more than five basis points from a current midpoint; both print only when non-zero,
  and neither drops the print -- on a depth book a print far from the mid is a sweep.
- **The venue's codes are in one place.**
  [The live page](04-live-and-carry.md#the-error-codes-this-line-has-actually-produced) carries all eight this line has
  produced, with what each meant here; the two timestamp formats (`60004` epoch seconds, `50112` ISO milliseconds) are
  the one worth memorising, because a client sharing one timestamp function across both surfaces is one code away from
  looking like it has the wrong key.

///

For implementation details, see [Architecture](../ARCHITECTURE.md). Data file details are documented
