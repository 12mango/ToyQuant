# Getting started

*Part of the [User Guide](../USER_GUIDE.md).*

Requirements, the build, the tests, and the three-command demo path.

## Requirements

- CMake 3.16 or newer
- A C++20-capable compiler
- Python 3 for the optional scenario and UDP tools

## Build and Test

Run these commands from the repository root:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

The same code also builds and tests under AddressSanitizer and UndefinedBehaviorSanitizer, which is the
gate's tier 3 (`bash tools/gate/verify_l2.sh asan`):

```bash
cmake --preset linux-asan
cmake --build out/build/linux-asan
ctest --preset linux-asan
```

The main programs are `out/build/linux-debug/toy_quant` for simulation/replay and
`out/build/linux-debug/backtest_main` for legacy metrics from recorded Tick data.

## A short demo path

After the build above, these three commands are the shortest ordered tour of the project. The first is the
per-edit gate, the second isolates the pre-trade controls, and the third shows one non-market-making cell
rather than the full grid. Run them in this order because the first command also cuts the 200k-row depth
slice that the other two read.

```bash
bash tools/gate/verify_l2.sh fast
bash tools/sweep/risk_check.sh
FAST_SLOW="5:30" ENTRIES="3" STOPS="4" bash tools/sweep/trend_sweep.sh
```

The gate prints one line per tier; the expected result is:

```text
tier0 OK
tier1 OK
```

The risk check prints one line per run and must end with `risk OK`:

```text
risk baseline: submitted=55 risk_line=0 recon=ok
risk permissive: submitted=55 execution_same=yes recon=ok
risk fat: submitted=38 fat_finger_rejections=13 recon=ok
risk rate: submitted=6 rate_limited_orders=15 recon=ok
risk OK
```

The baseline and permissive runs submit the same 55 orders and print the same `[EXECUTION]` line, which is
the point: enabling a permissive control must not change the run. The 1 bp band refuses 13 orders and the
0.01-per-second bucket refuses 15, and every run still reconciles. These are the same values recorded in
[Performance History](../performance/history-03-studies.md#the-pre-trade-controls-what-the-backoff-changed)

The one-cell trend sweep runs with `--order-size=10 --inventory-limit=100` and the best cell of the recorded
grid; it prints the CSV path and the row it wrote:

```text
trend sweep: /tmp/trend_sweep.csv (1 rows, order_size=10 inventory=100)
best by net_pnl_usd:
fast  slow  entry_ticks  exit_ticks  stop_ticks  fills  net_pnl_usd  fees_paid  risk_pauses  max_abs_inventory  recon
5     30    3            0.5         4           106    -6.04458     5.3        3            100                ok
```

The negative PnL is the expected result, not a failure: 106 taker clips pay 5.3 USD of fees against a
gross loss of 0.74 USD, so the signal has no forecast that covers the cost of taking liquidity. The full
27-cell command and the rest of that table are in
[Performance History](../performance/history-03-studies.md#the-non-market-making-signal-what-27-parameter-sets-say) The
demo makes no profitability claim; it shows a gate, a control and a measured negative result.

