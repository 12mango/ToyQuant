# Quickstart: the system on one page

*Part of the [User Guide](../USER_GUIDE.md). One diagram, one scoreboard, three commands; everything else on this page
is folded away.*

## The system, end to end

--8<-- "docs/_figures/path.mmd"

Two rows, and each is a claim. The top row is the path a market event takes -- a feed, a book with an **explicit** queue,
a strategy, a pre-trade gate, one venue interface with three engines behind it (the matcher, the paper broker, the live
venue), and **one** ledger. The bottom row is what makes the numbers re-derivable: a run records both legs, replays
offline, and has to reconcile -- so a live venue and a replay are the same program with a different plug.

## What eight markets said

| line | what a fill captured | what it cost | why it ended |
|---|---|---|---|
| crypto perpetuals (Deribit, Binance, OKX) | 0.088 ticks | a 2.57-tick fee | the fee is 29x the edge, and no lever moves a ratio like that |
| micro E-mini futures | 0.0131 ticks | a 0.5-tick commission | the same wall one market over: a fee a tick can cover is not a capture a fee can pay for |
| SPY, the window that started the fee question | 0.268 ticks | a 0.35-tick fee | the fee first, then 1 ms of order latency took +262 USD to +1.89 |
| US large caps (MU, MSTR, AMD) | 941 USD a window | a 10,588 USD position sigma | 506 windows to separate them, and the fee floor binds on 79% of order instances |
| options, 1 day to expiry | the spread is payment for tail risk | a 1.00 USD floor a contract | the premium can be smaller than the floor: insurance, not a quoting edge |
| options, 15 days and out | the fee axis passed for the first time | 56 prints a fill against 71 needed | the print count, not the fee, is what binds |
| funding carry, BTC, two venues, twelve months | 0 of 12 months clear | a 36 USD round trip | no month's differential earns the round trip |
| a low-turnover reversion rule | +149.76 cents a share, best window | -980.38 cents, worst window | it wins small and loses big: the sign does not replicate |

Every figure in this table is the [Retrospective](../RETROSPECTIVE.md)'s, and the quotients behind it are the fixture
`tests/fixtures/verdicts.json`, which `tools/study/viability.py` re-derives in tier 1 of the gate.

/// details | The same numbers as a picture, and the two lines measured after the eight

--8<-- "docs/_figures/charge.svg"

Two bars a market: what a fill captured, and what it cost. Where the fee bar is taller, no quote width pays. The labels
round to fit (0.088, 0.0131 and 0.268 ticks captured; 2.57, 0.5 and 0.35 ticks of fee); the table above carries the
exact figures.

| line | what the measurement said | why it stopped there |
|---|---|---|
| an EWMA trend rule (`trend_l2`) | +1.54 USD on 1 April over 90 fills, and negative on 1 June | closed by non-replication: the edge existed on one day and not the other |
| a volatility floor (standby when the market is quiet) | the quiet half loses 0.00410 USD a fill, the busy half 0.00515 | both halves lose, the quiet half loses less: a floor would keep the expensive minutes |

///

## Three commands

Build once, then the three paths this repository has: the replay, the same session over a socket, and the carry.

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug

# 1 one replay, and the market leg recorded for command 2
bash tools/gate/verify_l2.sh smoke
./out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
    /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --record /tmp/one

# 2 the same engine over a socket: a local sender streams what 1 wrote
python3 tools/live/websocket_sender.py --port 8765 --trades /tmp/one_trades.csv --depth /tmp/one_depth.csv &
./out/build/linux-debug/toy_quant ws 127.0.0.1:8765 BTC-PERPETUAL active_l2 1 prorata --fast-validation

# 3 the carry branch, closed by its own number
./out/build/linux-debug/toy_quant carry \
    data/v2/funding/BTCUSDT-fundingRate-2024-03.csv \
    data/v2/funding/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv \
    --symbol-a=BTCUSDT --symbol-b=BTC-USDT-SWAP --notional-usd=10000
```

1 prints `[PNL]` and writes the market leg; 2 feeds the same engine from a socket, so a replay and a live session are the
same program with a different reader; 3 prints `[CARRY]`, and the carry did not pay. The order leg to a venue, the
relay's flags and the recorded order leg are [the live page](04-live-and-carry.md)'s.

/// details | The recording loop, and the equality the gate checks

Command 1's recording replays as a file, and the `[PNL]` line has to be the one the recording run printed: that equality
is the loop in the diagram, and `fast` asserts it on this slice every run.

```bash
./out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
    /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
    --record /tmp/session --record-orders /tmp/session
./out/build/linux-debug/toy_quant l2_replay \
    /tmp/session_trades.csv /tmp/session_depth.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation
```

The order leg is `<prefix>_orders.csv`: the recording `RecordedVenue` replays as an engine, and what `[ORDER-LOG]`
reports. No app mode replays one yet -- that needs the first recorded session from a venue sandbox
([the live page](04-live-and-carry.md)).

///

## Every flag, folded

/// details | Every `l2_replay` flag, grouped by what it changes

--8<-- "docs/L2_FLAGS.md:flags"

///

Seven flags need a paragraph each rather than a line -- the recorder, the session snapshot, the order leg, the feed
thread, the paper broker, the order gateway and the live order leg. Those are on
[the flag reference](../L2_FLAGS.md).

/// details | Build, tests and the sanitizer tier

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug
ctest --preset linux-debug
```

The same code builds and tests under AddressSanitizer and UndefinedBehaviorSanitizer, which is the gate's tier 3
(`bash tools/gate/verify_l2.sh asan`):

```bash
cmake --preset linux-asan && cmake --build out/build/linux-asan && ctest --preset linux-asan
```

`out/build/linux-debug/toy_quant` is the simulator and replay binary; `out/build/linux-debug/backtest_main` computes
legacy metrics from recorded tick data.

///

/// details | The tour's last two commands, with the output to expect

`bash tools/gate/verify_l2.sh fast` prints one line a tier:

```text
tier0 OK
tier1 OK
```

`bash tools/sweep/risk_check.sh` isolates the pre-trade controls and must end with `risk OK`:

```text
risk baseline: submitted=55 risk_line=0 recon=ok
risk permissive: submitted=55 execution_same=yes recon=ok
risk fat: submitted=38 fat_finger_rejections=13 recon=ok
risk rate: submitted=6 rate_limited_orders=15 recon=ok
risk OK
```

The baseline and permissive runs submit the same 55 orders and print the same `[EXECUTION]` line, which is the point:
enabling a permissive control must not change the run. The 1 bp band refuses 13 orders and the 0.01-per-second bucket
refuses 15, and every run still reconciles. A single cell of the signal grid, which is negative on purpose:

```bash
FAST_SLOW="5:30" ENTRIES="3" STOPS="4" bash tools/sweep/trend_sweep.sh
```

```text
fast  slow  entry_ticks  exit_ticks  stop_ticks  fills  net_pnl_usd  fees_paid  risk_pauses  max_abs_inventory  recon
5     30    3            0.5         4           106    -6.04458     5.3        3            100                ok
```

106 taker clips pay 5.3 USD of fees against a gross loss of 0.74, so the signal has no forecast that covers the cost of
taking liquidity. The full 27-cell grid and the rest of that table are in
[Performance History](../performance/history-03-studies.md#the-non-market-making-signal-what-27-parameter-sets-say).

///

