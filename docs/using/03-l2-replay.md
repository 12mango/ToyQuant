# The L2 replay

*Part of the [User Guide](../USER_GUIDE.md).*

The mainline: trades plus incremental depth, every flag that changes it, and what each one does to the run.

Three parts, along the lines a run is actually debugged: what went in, what the venue did with the orders that came
out, and the inputs and names that are still standing.

## How to read this

| You want to... | Start here |
|---|---|
| run a replay with the recorded inputs | [The run and its inputs](03-l2-replay/01-the-run-and-its-inputs.md#replay-trades-and-bbo) |
| read the counters on `[L2 DATA]` | [Validation](03-l2-replay/01-the-run-and-its-inputs.md#l2-strategies-and-validation) |
| make the venue misbehave on purpose | [The venue side](03-l2-replay/02-the-venue-side.md#the-broker-and-the-reconciliation) |
| explain a timeout, or a re-send that was refused | [The order gateway](03-l2-replay/02-the-venue-side.md#the-order-gateway) |
| apply funding to a position a run is carrying | [Funding](03-l2-replay/03-funding-and-what-is-frozen.md#funding-as-an-input) |
| know which strategy names still work | [The frozen names](03-l2-replay/03-funding-and-what-is-frozen.md#replay-strategy-choices) |

## The three parts

| part | what it covers |
|---|---|
| [1. The run and its inputs](03-l2-replay/01-the-run-and-its-inputs.md) | the command, the two input streams, the strategy choices, and the validation counters that say what the feed delivered |
| [2. The venue side](03-l2-replay/02-the-venue-side.md) | the paper broker and its faults, the reconciliation, and the gateway's timeouts, idempotency and time in force |
| [3. Funding and what is frozen](03-l2-replay/03-funding-and-what-is-frozen.md) | funding as an input rather than a price, the L1 names kept for reproduction, and where the mode stands |

Where a number in these pages comes from is [Performance](../PERFORMANCE.md); what a *field* means is the
[field reference](../LEARNING.md#field-reference) in the Learning Path.
