# The L2 replay

*Part of the [User Guide](../USER_GUIDE.md).*

The mainline: what goes into a replay, what the venue did with the orders that came out, and the inputs and names that are
still standing. **Every flag is in [the flag reference](../L2_FLAGS.md)** -- these three pages are the how-to.

## How to read this

| You want to... | Start here |
|---|---|
| run a replay with the recorded inputs | [The run and its inputs](03-l2-replay/01-the-run-and-its-inputs.md#one-run) |
| look up a flag, or what it changes | [The flag reference](../L2_FLAGS.md) |
| know what a counter on `[L2 DATA]` means | [The glossary](../GLOSSARY.md), then [reading the output](06-reading-the-output.md) |
| make the venue misbehave on purpose | [The venue side](03-l2-replay/02-the-venue-side.md#the-broker-and-the-reconciliation) |
| explain a timeout, or a re-send that was refused | [The order gateway](03-l2-replay/02-the-venue-side.md#the-order-gateway) |
| apply funding to a position a run is carrying | [Funding](03-l2-replay/03-funding-and-what-is-frozen.md#funding-as-an-input) |
| compare strategies side by side | [The benchmark](03-l2-replay/03-funding-and-what-is-frozen.md#comparing-strategies-the-benchmark) |

## The three parts

| part | what it covers |
|---|---|
| [1. The run and its inputs](03-l2-replay/01-the-run-and-its-inputs.md) | the command, the two input streams, what the reader does with the rows, and the two defaults that surprise a first run |
| [2. The venue side](03-l2-replay/02-the-venue-side.md) | the paper broker and its faults, the reconciliation, and the gateway's timeouts, idempotency and time in force |
| [3. Funding and what is frozen](03-l2-replay/03-funding-and-what-is-frozen.md) | funding as an input rather than a price, one order's life traced, the benchmark, and what the demo does not claim |

Where a number in these pages comes from is [Performance](../PERFORMANCE.md); what a *field* means is the
[field reference](../LEARNING.md#field-reference) in the Learning Path.
