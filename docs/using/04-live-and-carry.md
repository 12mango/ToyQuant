# A live session and the carry line

*Part of the [User Guide](../USER_GUIDE.md).*

The WebSocket session over a local sender, and the two-legged
funding carry.

## Run a Live Session Over a WebSocket

`ws` is `l2_replay` with its two readers moved from files onto a socket, and nothing else: the same book, the
same matching engine, the same recorder, the same paper broker and the same strategy, because none of them can
tell a live feed from a replayed one. That is the point of the seam the reader is written against, and it is what
lets one strategy drive a session and a session become replayable.

```text
ws <host:port> <symbol> [strategy] [quantity_scale] [queue_model] [flags]
```

```bash
./out/build/linux-debug/toy_quant ws 127.0.0.1:8765 BTC-PERPETUAL active_l2 1 prorata \
  --fast-validation --record=out/lat/live
```

The client speaks the WebSocket protocol directly -- the RFC 6455 handshake and text frames -- with no external
dependency, and reads the two replay CSV shapes as its wire format, one row per text message:

```text
exchange,symbol,timestamp,id,side,price,amount                                  a trade, 7 columns
exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount         a depth change, 8 columns
```

A row's kind is its column count and a header row is skipped, so the two files `--record` writes can be streamed
back with no transformation. One reader consumes one ordered ring: the receive thread assembles a run of depth
rows that share a local timestamp into the batch the file reader produces, holds a trade that arrives during
that run until the batch ends, and pushes the batch and then the trade. One stream is what makes a trade gap
unable to stall depth delivery, which the earlier two-ring version could reach when the merge waited for a
trade while the depth ring filled. `--record=PREFIX` then writes the live session back into
`PREFIX_trades.csv` and `PREFIX_depth.csv`, and streaming those two files over the feed again reproduces them
byte for byte.

`tools/live/websocket_sender.py` is a local stand-in for a venue: it is a WebSocket server that streams a trade file
and a depth file, interleaved by their own timestamps, so the whole path can be exercised on a desk:

```bash
python3 tools/live/websocket_sender.py --port 8765 \
  --trades out/lat/live_trades.csv --depth out/lat/live_depth.csv &
./out/build/linux-debug/toy_quant ws 127.0.0.1:8765 BTC-PERPETUAL active_l2 1 prorata --fast-validation
```

The session ends when the peer closes the connection, and the run reports it as a `[WS]` line: the frames and
messages it read, the trades and depth updates it decoded, whether the close was clean, and whether the run
stopped on its own `--ws-seconds=N` instead. `ring_waits` is the number of times the input ring was full:
a full ring waits rather than dropping the row, so the socket is what slows down, and this is the one
counter that says the engine rather than the venue was the limit. `ring_high_water` is the deepest it got;
the live ring holds 131,072 rows against the 4,096 a replay uses, and the measured high-water over two hours
was 3,917. A live capture usually needs a bound, because a venue's socket does not close itself, and the
rows that arrived before the deadline are still drained rather than dropped. The
connect and the opening handshake share a five-second budget, so a host that accepts the connection and then
says nothing fails the run instead of hanging it. `--ws-target=/path` sets the HTTP path the handshake asks
for; it defaults to `/`. Pacing has no meaning on this mode -- the venue sets the speed, not a file's clock --
so `--live-pacing` is refused there, and every other `l2_replay` flag (`--record`, `--feed-thread`,
`--ring-capacity`, `--ws-ring-capacity`, `--idle-wait-us`, `--fast-validation`, `--profile-stages`,
`--paper-broker` and its faults, `--record-orders`, and the strategy and risk flags) is accepted unchanged.

`bash tools/live/soak_push.sh` is the acceptance run for this mode: it streams for `DURATION` seconds with a ping
every `PING` seconds and a forced reconnect every `DROP` seconds, then replays the engine's own recording and
fails if a sequence gap, a parse error, a ring wait, a reconciliation break or a moved field appears.

`--record-orders=PREFIX` writes the order leg of a session to `<prefix>_orders.csv` -- every submission, cancel and
execution report, in order -- and prints `[ORDER-LOG]` with its path and its three counts. The file is what
`RecordedVenue` replays as an engine; no app mode does that yet, because a replayed order leg needs a recorded session
to compare against and the first one comes from a venue sandbox. The order channel that would produce one is in the
relay, answering as a simulated venue: `python3 tools/live/venue_relay.py --simulate-orders --order-listen
127.0.0.1:8766`, which `tests/relay_order_channel_test` drives with the real adapter and the real transport.


## Carry: Two Legs Over Two Funding Histories

A carry is a position and its hedge, so it needs two instruments and it does not need a book: what it earns is
funding and what it pays is a round trip of fees. The mode states the position and measures both:

```bash
./out/build/linux-debug/toy_quant carry \
  data/v2/funding/BTCUSDT-fundingRate-2024-03.csv \
  data/v2/funding/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv \
  --symbol-a=BTCUSDT --symbol-b=BTC-USDT-SWAP --notional-usd=10000
```

| Flag | Effect | Default |
|---|---|---|
| `--symbol-a=SYM`, `--symbol-b=SYM` | Which instrument each leg holds; the venue's fee schedule is looked up by symbol | Required |
| `--notional-usd=N` | USD held on each leg, so the position is a hedge of 2N gross and zero net | 10000 |
| `--long=a`/`--long=b` | Which leg is bought; the other is sold | `a` |

The window is whatever the two histories cover together, the position is opened at its first settlement and
closed at its last, and each settlement inside the window is charged to its own leg at its own mark. The
position is stated rather than chosen: `--long` fixes the side before the run looks at the data, so the number
is a measurement and not a fitted result. Entry and exit are taker fills at the venues' published rates, which
is the expensive assumption; a maker entry would halve the fee and is not assumed.

The report is one line:

```text
[CARRY] carry_leg_a=BTCUSDT carry_leg_b=BTC-USDT-SWAP carry_notional_usd=10000
        carry_gross_notional_usd=20000 carry_net_notional_usd=0 carry_window_hours=728
        carry_settlements=182 carry_leg_a_funding_usd=-304.656 carry_leg_b_funding_usd=322.241
        carry_funding_usd=17.5849 carry_fees_usd=18 carry_net_usd=-0.415093
        carry_annualized_carry_usd=211.599 carry_annualized_return=0.0211599
        carry_to_fees=11.7555 carry_breakeven_hours=745.184
```

`carry_gross_notional_usd` and `carry_net_notional_usd` are the two ways to read the same book and only the
second is the account's exposure: both legs carry a notional, and their sum is what the position is worth to
someone who reads one leg as the position. The per-leg funding shows what flows and what is left after the
hedge takes its side back: 304.66 USD paid on one venue and 322.24 received on the other leave 17.58 USD, which
is the carry. `carry_net_usd` is that minus the fees, `carry_annualized_carry_usd` is the carry as a yearly
rate, and `carry_to_fees` divides it by the fees: the acceptance number. `carry_breakeven_hours` is the same
comparison in the form a decision needs, because the fees are paid once and the carry accrues every interval —
here the position must be held 745 hours to pay for its fees and it was held for 728, which is why the net is
just below zero. The run is recorded with its numbers in
[Performance History](../performance/history-03-studies.md#the-acceptance-number)

The replay then reports a `[PNL]` line for every run, which splits what completed from what is still open and
adds the fees back to each:

```text
[PNL] gross_pnl_usd=-0.0818127 net_pnl_usd=-0.123813 gross_closed_pnl_usd=-0.0672835
      closed_pnl_usd=-0.109283 maker_notional_usd=210 net_per_fill_usd=-0.00589584
      gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
```

`closed_pnl_usd` is the portfolio's realized PnL, so a position the run was still holding at the end is not in
it; `net_pnl_usd` adds that position marked at the last price; the `gross_` pair adds the fees back to each, and
a negative gross PnL says the run lost money before the venue charged anything. `maker_notional_usd` is what the
maker fees were charged on, and `breakeven_maker_rate` is the maker rate at which this window's total PnL would
have been zero: **a negative answer means no fee at all would still have lost money**, and that size of rebate
would have been needed instead. `tools/sweep/fee_sweep.sh` runs the sweep over the windows the documents quote and
prints the table with the block that re-derives it.

