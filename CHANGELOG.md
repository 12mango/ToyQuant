# Changelog

**What changed in the code, release by release.** The plan's own log — what the project decided to build or not build,
and when — is in [Roadmap](docs/ROADMAP.md#change-log), and the two are deliberately different records of the same work.

The repository is named `ToyQuant`. The `-v1` suffix that older links and a local clone may still carry is a leftover
from the first frozen tag, `v1.0-demo`, which is a *version* and stays: every reference in `docs/` and on the published
site now points at `ToyQuant`.

- **The recorded `+26.18` of the frozen cell is archived as not re-derivable, and `+9.83` is the number that stands.** The
  cell's own flags with `--advance-debounce-ms=40000` -- forty **seconds**, the arithmetic the cooldown defect was doing
  when the cell was measured -- read the same seven days identical to the last digit to the 40 ms arm, so the debounce is
  **inert in this cell** and the unit is excluded as the explanation; the edge cover (+5.37), the layer count (+17.93), the
  refresh column (+27.14, +9.59) and the day set are excluded as well, which leaves a code revision no run in the record
  carries. The archival changes the **verdict** too: at +9.83 the two-layer cell is worse on capture than the one-layer
  cell's re-derived +17.93 (26 fills against 19), so the layer buys **fills** and gives up **capture** -- the opposite of
  what the recorded table said. The tables keep the old value with the word *recorded* beside it.

- **The toxicity switch at ten contracts buys the tail and pays the median.** No code change -- the state was already
  wired -- and the arm was registered before it ran: the worst net day goes from **-202.88 to -76.38 bps**, because the
  carried inventory's mark on that day falls from **-168.76** to **-6.38** and the loss becomes a quote loss rather than a
  carry loss, while the closed median falls to +2.05 on 4 of 7 days, the settled median turns **-9.42** and the dispersion
  gate fails both of its conditions. Refused on its registered bar; the next registration is a **stricter** toxic
  definition (`--jump-ticks=3`) whose bar needs the tail **and** the median at once.

## The live file could not name `--live-orders`, and the engine said so into an unread journal (2026-11-27)

- **The engine makes the four `venue_*` flags *part of* `--live-orders` and refuses them without it**, and neither the
  template nor the config reader had the switch: the generated command line carried `--venue-endpoint`,
  `--venue-max-orders` and `--venue-orders-per-second`, no opt-in, and the unit started, exited 1 on
  `--venue-endpoint ... are part of --live-orders` and was restarted -- `ExecMainStatus=1`, `MainPID=0` between two
  attempts, and both relay sockets listening, which is the third deploy's report.
- **`engine.live_orders` and `link.venue_max_notional` are mapped**, and `live_config.py` now **validates the coupling
  itself**, in `--flags` and in `--check`: a venue key without the switch is refused by name, and the switch without
  `link.venue_endpoint`/`venue_max_orders` the same way. The self-test drives all three cases, so a file that would be
  refused on a host is refused where it is written. The template sets `live_orders: true`, because that is the shape a host
  runs.
- **The health report attaches the program's own words.** Three deploys in a row cost a round trip because the report quoted
  systemd's state and not the unit's stdout, where each explanation already was: on failure it now prints the last
  `HEALTH_JOURNAL_LINES` (25) lines of both units, and the error-severity count is windowed by `HEALTH_WINDOW_S` like the
  other two checks, so a repaired unit can go green without a reboot.

## The relay served one socket, not two (2026-11-27)

- **A relay given both `--listen` and `--order-listen` served the order channel and returned.** The order channel was a
  `return` in `main()`, so the market leg was unreachable in the only configuration a host uses: the engine connected to
  `8766`, found nothing on `8765`, exited on a refused connection and was restarted -- the second deploy measured **59
  restarts and 259 "Failed with result" lines**, with a health report that read the unit `active` between two of them and
  `0` session processes because `pgrep` had sampled the gap. Both halves now run in one process: the order channel on its
  own thread (it is a blocking accept loop) and the market leg on the main one; naming a single socket keeps the old
  behaviour, so every recorded run stands. Verified locally: with both flags, `18765` and `18766` are both in `LISTEN`.
- **The engine waits for the socket instead of racing it.** `ExecStartPre` polls the listening table for
  `127.0.0.1:8765` for up to 30 s, because `Type=simple` counts the relay started before it has bound anything. It reads
  `ss -ltn` and never connects: the relay's accept loop takes exactly one client, so a probe connection would be
  *accepted* and its failed handshake would kill the relay.
- **The health check asks systemd instead of sampling `/proc`.** The session's process is the unit's `MainPID` (a
  `pgrep` scan reads zero between two restarts, which is what the report said while the unit was cycling), duplicates stay
  a `pgrep` question because that is what it is good at, and the five systemd properties print one per line because
  `systemctl show` prints several `--value`s in its own order and a single line of them cannot be labelled honestly.
- **A counter that only a reboot clears is not a health check.** `Failed with result` is counted in the last
  `HEALTH_WINDOW_S` (600) rather than since boot, and `NRestarts` is read as "since the unit was last started by hand" --
  which the deploy now makes true by running `systemctl reset-failed` before it restarts, so a repaired unit can go green
  without a reboot and a fresh crash is still red.

## The shipped binary names its instruction set, and the first deploy crash-looped (2026-11-26)

- **`-march=native` shipped the builder's CPU to the host.** The artifact is built on `ubuntu-latest` and run on a Tokyo GCP
  vCPU; the compiler emitted whatever the *runner's* CPU had, the host's did not have it, and `toyquant-live.service` died on
  an illegal instruction -- `code=dumped, status=4/ILL` -- **35 restarts** before anyone read the journal. The health report
  meanwhile said "no error-severity journal line since boot", because systemd writes `Failed with result 'core-dump'` at a
  severity `journalctl -p err` does not select.
- **`TOYQUANT_PORTABLE_BINARY` names the instruction set instead of inheriting one** (`-march=x86-64-v2 -mtune=generic`), and
  the `linux-relwithdebinfo` preset -- the one the deploy ships -- sets it. The debug preset keeps `-march=native`, so every
  recorded number in this repository is unchanged; the deploy's `verify` job now starts the artifact (`--help`) and refuses
  a bundle whose program does not start.
- **A crash loop is a red unit, not a climbing counter.** Both units carry `StartLimitIntervalSec=300` and
  `StartLimitBurst=5`: five failures in five minutes stops the unit and leaves it failed. After the cause is fixed,
  `systemctl reset-failed <unit>` is what clears the limit, and `deploy/check_health.sh` says so in its failure line.
- **`check_health.sh` waits for the state instead of sleeping at it, and reads the reason.** It polls `is-active` for up to
  `WAIT_ACTIVE_S` (15 s) rather than being preceded by a `sleep 3`, prints
  `ActiveState/SubState/NRestarts/ExecMainStatus/Result`, fails on any restart, and counts `Failed with result` lines. A
  fixed sleep was the first version and the wrong shape: three seconds is either too short (the unit is still `activating`)
  or wasted, and it says nothing about *why* a unit is not up.

## A live host can be brought up, and the two files say what it is (2026-11-26)

- **The relay had no unit, and the engine's put it out of reach.** `toyquant-live.service` described "the relay and the
  engine" and started only the engine, so a host that followed the record ran an engine reading a closed port. There are now
  **two** units, `deploy/toyquant-relay.service` and `deploy/toyquant-live.service`, ordered so the relay starts first and
  stops last, and `deploy/check_health.sh` asks both halves' questions: each unit active, one process of each kind, the
  market socket and the order socket listening, the binary present, the snapshot parseable and fresh, and neither operator
  file readable by others.
- **`.env.example` named five variables nothing reads.** `VenueCredentials.from_environment` reads
  `TOYQUANT_API_KEY`/`TOYQUANT_API_SECRET`/`TOYQUANT_PASSPHRASE` (ADR 0005 decision 3), while the template said
  `OKX_API_*` and `OKX_ORDER_URL`; a host that filled it in would have been refused with "the private channel needs
  TOYQUANT_API_KEY". The template now carries the real names plus the seat's five venue facts and the route's six keys, and
  `docs/using/04-live-and-carry.md` documents what each group does when it is wrong.
- **`RELAY_*` are flag defaults, so a unit carries no seat.** `venue_relay.py` reads eleven `RELAY_*` variables as the
  defaults of its own flags: the instrument, the contract's value, the tick, the lot, the engine's scale, the venue's
  `instIdCode`, both endpoints, both sockets and the route. A host that names none behaves exactly as before, and the
  instrument a session trades stops being a fact about a tracked file.
- **`--order-live` exists, because the demo and live hosts disagree about one header.** The live path always sent
  `x-simulated-trading: 1`, which the demo host requires and the live host refuses; the flag drops it
  (`RELAY_ORDER_LIVE=1` does the same), and the relay's self-test pins the header map both ways -- the demo arm has the
  header and the live arm does not -- without a network call.
- **`okx_probe.py spec <inst>` prints the venue's own numbers**: `tickSz`, `lotSz`, `minSz`, `ctVal`, `ctValCcy` and
  `instIdCode`, and what one contract is in engine units. Measured against the live instrument table:
  `BTC-USDT-SWAP: tick 0.1 lot 0.01 min 0.01 ctVal 0.01 BTC instIdCode 10459` and **one contract = 10,000 engine units**,
  which is the arithmetic `order_size` in `/etc/toyquant/live.yaml` is counted in.

## The publication boundary is a check, and the deploy is a closed loop (2026-11-26)

- **`tools/gate/check_leaks.py` is the boundary, and its first run found the tree on the wrong side of it.** Structural
  rules refuse a key shape, a drive letter followed by a backslash, a POSIX home path, the cloud-drive folder name, a
  filled credential, and a tracked `config/live*.yaml`, `.env`, `secrets/`, `private/` or `local_reports/`; they scan
  `git ls-files` because that is the list a reviewer's bundle carries. A deny-list at `private/leak_denylist.txt` (ignored)
  carries the policy half with a `[block]` section that fails and an `[audit]` section that is counted and printed, so the
  same tool measures the migration and then enforces it. Wired into `verify_l2.sh` (every tier), CI and the deploy's
  `verify` job, with `--self-test` covering both directions of every rule.
- **Thirteen machine paths left the tracked tree.** One drive letter with a backslash, three `/home/<operator>/` paths and
  the cloud-drive folder name were in `.clinerules`, `CHANGELOG.md`, `docs/ROADMAP.md`, `.gitignore` and
  `tools/export/push_windows.sh`; each is now a spelling a document may use (`X:/sync`, `<the folder .drive-target names>`,
  "a path in the operator's home directory") so the lesson travels and the machine does not.
- **`private/` exists, and stage 1 of the migration is in it.** The research ledger (`docs/archive/studies.md`) and the
  four parts of the performance history moved with `git rm --cached` + `mv` into `private/docs/`, and each path now carries
  a stub that says what the page recorded, which public page documents the same method, and who may ask for the numbers.
  Nine inbound `page.md#anchor` links were dropped to the page in the same change, because a stub has no sections. The
  deny-list's audit count -- 22 files naming the first seat, 4 the second -- is what stage 2 has left.
- **`.gitignore` covers the private side**: `private/`, `local_reports/`, `deploy/secrets/` and key material by extension.
- **The deploy is a closed loop, and the unit had a defect.** `ExecStart` pointed at `/opt/toyquant/out/build/linux-debug/toy_quant`,
  a path no bundle ever ships; it now runs `/opt/toyquant/toy_quant` under an unprivileged `toyquant` account, reads
  `/etc/toyquant/live.yaml`, and `TimeoutStopSec=12` is documented as the engine's own 10 s shutdown deadline plus a margin.
  `deploy/bootstrap_server.sh` (idempotent: packages, account, directories, logrotate, unit, and the two operator files
  seeded at `0640` and never rewritten), `deploy/check_health.sh` (five questions: active, exactly one process, no
  error-severity journal line, binary present, snapshot parses and is fresh, credentials not world-readable) and
  `deploy/rollback.sh` (one command, from the `toy_quant.prev` the deploy backs up). The workflow now backs the binary up,
  runs the bootstrap, restarts, and **fails the job on a red health check**.

## The tail's exit price is not a lever, and the two seats get their sizes (2026-11-26)

- **`--wind-down-adverse-bps=X` makes the wind-down conditional**, and the arm it was built for refuses the mechanism. The
  skew now starts only when the carried position is marked against us by X bps -- `open_pnl_bps`, read from the
  volume-weighted entry the maker keeps beside its position -- so a favourably marked tail settles untouched. On the
  ten-contract UP cell the condition selects exactly as specified (the three losing days reproduce the unconditional arm to
  the digit, the winning days come back to the control) and the arm is still worse: closed median **-2.13 on 3 of 7**
  against the control's +10.32 on 5, worst day **-123.02 against -34.12**, settled median -5.43 against +11.33. The skew
  sells into the run instead of avoiding it, and the carried mark does not predict the tail's direction. `l2_strategy_test`
  pins the condition at three marks -- favourable, -10 bps and -20 bps against a 15 bps threshold -- and the flag is
  refused without `--wind-down-skew-minutes` by name.
- **NES's seat is sized at five contracts, not ten.** The same frozen shape at both sizes over the same seven days: five
  reads a closed median of **+16.60 on 5 of 7**, `settled_bps` **+6.94**, `fees_bps` exactly 2.00 and **+0.50 USD a day**;
  ten reads +7.08 on 4 of 7, a settled median of **-18.17** and a worst day **8.76 times its own median**. So the two-seat
  quota is UP at ten contracts (+0.73 USD a day) and NES at five.
- **`tools/study/sweep_summary.py --gate`** applies the two dispersion conditions the BLUR ladder needed -- the closed
  median with its best day removed stays positive, and the worst day stays within `--worst-multiple` (3 by default) of the
  median -- and prints the best day's share of the cell and its multiple of the second best beside the median. Under it
  BLUR archives: the **hundred-contract capped cell is the only arm that passes both conditions with moderate dominance**
  (+43.76, leave-one-out +27.47, best day 72 per cent and 3.3x the next, worst 2.96x the median), the three-hundred point
  passes while 88 per cent of it is one day (9.2x the next), and the thousand-contract and uncapped arms **fail the tail
  clause** (6.82x and 13.91x their medians). The self-test covers both failure modes and runs in the `code` tier.
- **`tools/live/live_config.py` maps two more engine keys** -- `wind_down_skew_ticks` and `wind_down_adverse_bps`, **42 flag
  keys and five switches** in the table now -- and `config/live.example.yaml` carries both as comments, because a live
  session that names a wind-down needs to be able to name its condition.

## The parent's deadline is in the clock's unit (2026-11-26)

- **`span_seconds` and `complete` read 0 on every OKX window, and the cause was the deadline rather than the summary.**
  `RunAccounting` added the parent's horizon *in microseconds* to the path's timestamp, and an OKX cut counts
  **milliseconds**: a two-hour parent's deadline sat eighty-three days out, `finish_parent` never ran, and the
  `[EXECUTION_ALGO]` line reported a span of zero for a schedule that had finished. The unit now comes from
  `common/clock_units.h` at the first event, the L1 path keeps its explicit override, and the two numbers read
  **`span_seconds=7200 complete=1`** on UP 11-16 through the binary. `pipeline_accounting_test` pins a millisecond clock
  (sixty seconds is sixty thousand ticks) beside the microsecond fixtures' sixty million.
- **The live config's key list grew with the flags a live session names.** `tools/live/live_config.py` now maps **40 flag
  keys and five switches** --
  the risk limits (`starting_cash`, `max_exposure_usd`, `kill_switch_loss_usd`, `fat_finger_bps`,
  `max_orders_per_second`), the adapter's own bounds, the quote geometry, the two state switches and the sizing cap --
  plus the two optional positionals `quantity_scale` and `queue_model`, and `config/live.example.yaml` carries the risk
  block rather than only the demo shape. Every flag name in the map was checked against the parser's own prefixes; an
  unknown key is still refused by name.



## Capacity by sizing, and the trend line's signal replaced and sealed (2026-11-26)

- **`--max-depth-ratio=F --depth-ladder-max=N`**: no quote rung larger than F of the same side's displayed touch size, and
  a quote above the cap split into up to N rungs one tick apart, with the excess dropped **and counted**
  (`depth_capped_rungs`, `depth_unquoted_quantity`). Refused with `--extra-layers` (two ladders) and on a strategy that does
  not quote; off by default.
- **The cap recovers BLUR's hundred-contract cell and the ladder destroys it.** At thirteen ticks and a hundred contracts:
  uncapped **+9.32 closed on 5 of 7**, a 0.10 cap with **four rungs** 6 fills and **-2.15**, a 0.15 cap with **one rung**
  **+43.76 on 5 of 7** and a net of +48.16 -- identically under `conservative`, which is the half of the bar that matters for
  a rule that sizes to a level a spoof can move. NES at ten contracts is worse under either cap (+1.45 on four days
  uncapped, -5.87 on one capped), so the lever is a fine-tick one and the money question (0.67 to 10.63 USD a window) is
  untouched.
- **`--trend-signal=burst --trend-burst=ticks=T,window_ms=W,obi=F,hold=VIEWS`** replaces the trend line's EWMA cross with
  the maker's own event -- a mid range of T ticks inside W milliseconds of book time, taken only when the displayed-depth
  imbalance is F of one side on the same sign -- and keeps the framework's target, clips and three exits. The EWMA is the
  default, so every recorded trend number keeps its rule.
- **And the replacement is sealed rather than seated.** With the entry priced inside the touch (`--post-only`, fee exactly
  2.00 on every day) the burst reads **UP: 8 fills, 1 of 7 positive, -5.18 closed** and **NES: 6 fills, 5 of 7 positive,
  +2.12 closed / +0.12 net** -- the line's best day count ever measured, at about half the 4.00 the round trip costs. The
  bar was +4.00 on both seats, so `trend_l2` is sealed as a family on the owner's instruction, with the one number that
  reopens it written down (a seat whose burst fills average above +4.00 closed).
- `l2_strategy_test` pins both: the depth cap's rungs (four of a hundred, one tick apart, the excess counted, and the whole
  size as one order when the ratio is off) and the burst (two ticks inside the window with the depth agreeing sets the
  target, the depth against it does not, and the hold expires).



## Two state switches on the maker: a toxicity-selected amend and a wind-down (2026-11-26)

- **`--amend-on-toxicity`** chooses between the two separately measured repricing arms by state instead of by run: the
  cancel path while the book is calm, the two-tick retreat amend while it is toxic, where toxic is the mid's range over
  `--jump-window-ms` past `--jump-ticks` (`active_l2`'s detector, no cooldown needed) or the recent flow past
  `--toxicity-imbalance` (0.9 by default). Both branches are counted in `[STRATEGY_METRICS]` as `toxicity_amends` and
  `toxicity_refreshes`, because a state that never fires reads exactly like a switch that does nothing -- on UP 11-16 the
  arm amended **9,571** refreshes and kept the cancel path on **13,707**.
- **`--wind-down-skew-minutes=M --wind-down-end=HH:MM --wind-down-skew-ticks=T`** is the maker's own tail work: inside the
  last M minutes of a named UTC session end the adding side stops opening and the closing side walks in towards the touch
  in whole ticks as the clock advances. The end is named because a strategy inside a replay cannot see where the replay
  stops. The wind-down's closing side is **exempt from `--advance-debounce-ms`** -- a deadline outranks a debounce, and
  without the exemption the 40-second debounce of the frozen cell held the skew's own move back.
- **The jump detector's window was forty seconds, not forty milliseconds.** `ActiveL2MarketMaker` turned
  `jump_window_ms` into microseconds (`* 1000`) and compared it against a clock that is milliseconds on an OKX cut, so
  every "40 ms" jump window was forty seconds and the recorded cooldown row ("the cooldown kills the flow, 0-1 fills") was
  measured through that defect. `clock_units_per_second` derives the unit now, and `l2_strategy_test` pins a two-tick
  range inside forty milliseconds against the same range spread over a hundred.
- **An amend built from the working record's quantity is declined once the order has partly filled.** The record holds
  what is left and a venue recomputes the remainder from the *total*, so a half-filled order's amend asked the venue for
  `remaining - filled` and was refused -- and a refusal is a terminal report, so the strategy would forget an order that is
  still resting. Unreachable at one contract a layer (the working record empties and the order is dropped), reachable at
  any size; the amend now carries the total and the test pins it after a three-of-four fill.
- **Measured, and both bars refuse their arm.** The switch lifts the per-fill capture on all three seats (UP closed
  +5.37 -> +5.74, NES net +1.25 -> +6.41, PROS closed +5.82 -> **+7.21**) without losing a fifth of the fills and narrows
  UP's worst day (-66.80 -> -48.67), but it converts no day: UP's positive closed days fall 5 -> 4, NES's median and worst
  day get worse, and **PROS stays at 4 of 7** against the five a seat needs. The wind-down takes the residual to zero on
  both seats (median absolute position 50M and 20M units -> 0) at a settled median of +6.00 against +12.75 on UP and
  -10.24 against +0.51 on NES, so the tail can be worked but this week's mark did not repay it. Neither mechanism changes
  the frozen configuration, and the closed/tail behaviour of both is in `docs/archive/studies.md`; the RETROSPECTIVE's
  jump-cooldown row is **withdrawn and replaced** by the re-measurement through the fixed clock (30 fills against 30, a
  closed median of +6.02 against +5.37, a worst day of -53.37 against -66.80).



## The execution line's resting child follows the book (2026-11-26)

- **`--amend-instead-of-cancel` now reaches `execution_l2`**, as one flag and one rule: a posted child whose near touch
  has moved at least a tick -- either direction -- is amended to the new touch in place instead of waiting at the price it
  joined for its whole timeout. The request carries the child's *total* quantity, because the venue recomputes the
  remainder from what it has already filled; and an engine without an amend answers with the cancel it replaced, after
  which the slice is **posted again at the new price rather than crossed** -- an amend is not a deadline, so turning it
  into a taker fill would price the follow as the timeout it is not. `execution_l2_test` pins the deadband, the whole
  quantity, the once-per-move record and the repost.
- **The fourth close-out arm ran on the window the other three did** (15:15-15:30 UTC, seven days, UP and NES, a
  ten-contract sell, `--algo=twap:10 --algo-entry=post:60`): the follow clears **0.5-0.7 of the parent on both seats**
  against the 1.0 its registered bar asked, at `is_total_bps` **-17.68** (UP) and **+4.83** (NES) against the settle
  bench of 1.56 and 4.01. It does act, and in the direction it was built for -- NES's fee falls from 1.78-3.42 to
  1.00-2.62 bps and `maker_share` rises with it, the cost against the arrival mid improves by 3.4 and 4.3 bps, and the
  child count falls because a followed child keeps its id -- but every amendment re-queues the child at the back of the
  level it moves to, so the follow trades clearance for price. The wind-down stays `--settle-residual`.
- **Three sentences about the engine were stale and are corrected.** `--amend-instead-of-cancel`'s help, `docs/L2_FLAGS.md`
  and the retrospective said the replay's engine "takes the request as the cancel it replaced"; `MatchingEngine` has
  amended in place since `a99aed0`, so the amend arms priced the queue reset as well as the choice, and the fallback is
  now described as what an engine *without* an amend does.
- **And two rows of the close-out table were re-derived rather than trusted**: the passive arm's `filled_ratio` had been
  quoted from the run's `fill_rate` (fills per child: 0.857, 0.78-0.89) and UP's cost as -10.73, which is one day's value.
  The parent's clearance is 0.5-0.7 on UP and 0.6-0.8 on NES, UP's median is **-21.04**, and the fourth arm's bar is
  written on that column. `docs/archive/studies.md` carries both the correction and the arm.



- **Eight instruments had no specification**, so every replay of them exited 1 with `Error: no instrument specification
  for L2 replay symbol: OL-USDT-SWAP` -- and the sweep wrote a row of `?` and `0.0000` instead of saying so. OL, PROS,
  BLUR, DGAI, GRVT, ONE, BABY and ALLO are now registered from the venue's own instrument list: OL and ONE quote at 1e-6
  and 1e-7 on 100-token contracts, PROS at 1e-4 on a one-token contract, BLUR and BABY at 1e-6, GRVT and ALLO at 1e-5,
  DGAI at 1e-4 -- all on ten-token contracts except where stated.
- **`okx_maker_baseline.py` turned a failed replay into a table of zeros.** It now stops with the day, the model and the
  child's last stderr line, because a missing input is an error that names the symbol and never a row that reads
  "measured, nothing happened".
- **And the scripts passed `--tick 0.0001` for every instrument**, which cuts the book at a resolution the venue never
  used: on OL that is a hundred ticks too coarse and on ONE a thousand. The runs now carry the venue's own tick, which
  is also what makes `tick_bps` -- and every distance in basis points derived from it -- mean what it says.

## Amend, from the strategy layer down to the book (2026-11-25)

- **The amend is asymmetric now, and the retreat-only arm has the best capture of the three measured.** Amending every
  refresh lost (closed +9.83 -> -5.92, `zero_queue` 21-53 -> 239-296) because an amended quote sits at the fresh price,
  i.e. at the back of the level it just cleared. Amending only a retreat, with a two-tick deadband, reads closed median
  **+14.87** with `zero_queue` back at 35-64 -- and fills 19 against 26, outside the bar's 20 per cent, so the row stays
  open on the deadband rather than on a threshold.
- **`MatchingEngine::amend_order` keeps the queue position for a same-price size reduction**, the one amend a venue does
  not charge for; `matching_engine_test` pins it with two orders at one price and the first cut from ten to five.

- **`--pause-ticks-per-second=D`** exposes `ActiveL2MarketMaker`'s volatility halt, which had been on at 9.0 with no switch
  in every recorded run. `0` leaves the strategy's own value, and the default arm reproduces the frozen rows to the last
  digit; `1e9` turns it off and `risk_pause_count` reads 513 -> 0. The registered bar asked whether the halt was a tax:
  fills 26 (19-50) with it and 26 (12-44) without do not reach thirty, and the closed median falls from **+9.83** to
  **+6.38**, worse on six of seven days and worse on the worst day (-69.40 to -87.88). The halt stays on and the row closes.

- **`--amend-instead-of-cancel`** moves a primary quote in place instead of pulling it and placing it again. The strategy
  emits the request, the pipeline routes it, and an engine that cannot amend receives the cancel the request replaced --
  so the flag is inert on an engine without an amend rather than an error.
- **`MatchingEngine::amend_order`** implements the venue's semantics as far as a replay can need them: the id survives, the
  order is re-priced onto the tick grid, it lands at the **back** of the level it moves to, a crossing post-only amend is
  refused with the order left where it was, and **no execution report is emitted** -- a venue answers an amend with an
  acknowledgement. `matching_engine_test` pins each of those, including the remainder arithmetic after a partial fill.
- **The UP gap is attributed and it is not the rule**: `--quote-distance-bps=14` reproduces the frozen cell's fills and
  closed bps *identically on six of seven days*, and differs on the one day whose price halved (2 ticks instead of 3). The
  recorded +26.18 does not re-derive from the flags the studies page records for it -- four further variants read +5.37,
  +17.93, +27.14 and +9.59 -- so it is quotable as "recorded" only.
- **The amend was then measured in the replay and it loses on capture**: with the venue's semantics modelled (the order
  keeps its id and re-queues at the back of the level it moves to), UP's frozen cell reads fills **26 → 49** a window,
  closed median **+9.83 → -5.92**, positive days 5 → 3, net median **+6.78 → -1.82**, and quotes that were never behind
  anyone 21-53 → 239-296. The amend doubles the fills by never being absent from the fresh price, and every extra fill is
  a quote the market walked into. Closed at a 10 ms link: the 79.9 ms it saves is worth less than the queue it gives up.

## Two registered mechanisms, built, and both fail their bars (2026-11-25)

- **`--quote-distance-bps=D`** turns the quote's half width into `max(2, round(D / tick_bps))` ticks, one tick being worth
  `tick / price` of the price; it and `--base-spread-ticks` set one distance and are refused together. At `D = 14` it picks
  3 ticks on UP (5.2 bps a tick) and 2 on DNA (7.3) -- the distances the per-instrument sweeps found by hand -- and
  reproduces their fill counts (26 and 21 a window) over the same seven days at 13:30-15:30 UTC with the frozen cell.
  Neither reaches the 30-fill floor, which is a property of the books; on UP the closed median reads +9.83 against the
  recorded +26.18, and that gap is not attributed.
- **`--onesided-inventory-frac=F`** stops the adding side at F of the inventory cap. At twenty contracts and F = 0.5 it
  takes UP's worst day from -215.52 to -138.01 bps and three peak inventories down by 400 to 600 contracts, and the
  medians fall with them (closed +14.87 to +9.62, net +27.14 to +4.84): the tail is what it buys.
- **The wiring defect both runs exposed**: the two flags reached the strategy's config in the unit test but not through the
  application, so the first pass ran both distance arms at the default two ticks and the two one-sided arms came out
  identical row for row -- caught because a flag that changes nothing shows up as an equality, not as a small difference.
- **`l2_strategy_test` pins both**: the bps rule's two-tick floor and its 385-tick width on a 6421.25 book, and the
  one-sided retreat against the same position with the flag off.

## An amend is one round trip and cancel+place is two (2026-11-25)

- **`okx_probe.py` gained `amend`**, which runs both ways of moving a resting quote in one process on one kept-alive
  tunnel, alternating which leg goes first: a `post_only` buy two percent under the best bid, amended to the same price
  one tick nearer, against the same order cancelled and placed again.
- **Measured on `BTC-USDT-SWAP`: amend p50 83.2 ms (p95 100.0) against cancel+place 163.1 ms (p95 175.6)**, and the
  decomposition reconciles -- the same run's cancel reads 80.2 and its place 80.8, so the two-call leg should cost 161.0
  and reads 163.1. Every amend was applied (the price read back from `/trade/order` moved on 12 of 12) with no `sCode`
  and no decline, so a `post_only` buy can be repriced in place: 79.9 ms saved, half a repricing decision, and no moment
  with an empty level. The ruler was registered before the run -- inside 75 to 85 ms -- and it landed at 83.2.
- **A defect in the probe's first version**: the two prices were computed with `round` on the raw fraction, and a
  half-tick lands on a boundary where `round` answers to even, so the target came out equal to the start and the run
  timed an amend that asked for the price the order already had (82.6 ms, meaning nothing). The prices are built from an
  integer tick count now and the probe refuses a target equal to its start.

## The recording decorator handed every L2 update to the engine as a BBO one (2026-11-25)

- **`--record-orders` really did change the run, and the cause was a virtual that was not forwarded.**
  `IMatchingEngine::process_l2_top` defaults to `process_bbo`, and `RecordingEngine` overrode only `process_bbo`, so an L2
  feed reached the engine as a BBO feed and the queue accounting inside `MatchingEngine::process_l2_top` never ran.
  Measured on one day and one eight-hour window of `UP-USDT-SWAP`: the flagged run read **207 fills with
  `queue_ahead_consumed=0`** against **123 fills with 142,572,001,537** without it. `PaperBroker` and `OrderGateway` both
  forward `process_l2_top`; this decorator did not. It also answered all six queue queries -- `queue_ahead_at`,
  the two `queue_ahead_consumed`, `queue_ahead_levels_cleared`, `queue_ahead_from_quantity_changes` and
  `rejected_orders` -- from the interface's defaults, which return zero, so the ledger's queue audit and the pipeline's
  refusal detection were blind under the flag as well.
- **Both sets are forwarded now and the A/B closes.** The same day, window, cell and input read **123 fills on both
  sides**, net **-12.22 bps**, `queue_zero_orders=123` and the same `queue_ahead_consumed`: a recording is a recording
  again. The hypothesis the record carried -- that the recorder stamped untimed reports and the debounce read them -- was
  wrong, and the measurement replaces it.
- **`order_recorder_test` pins both halves**, with a scripted venue whose queue queries carry distinctive values and
  whose L2 and BBO entry points are counted apart, so a decorator that answers the interface's zero, or downgrades an L2
  update to a BBO one, fails a test instead of moving a session. `code` OK, `fast` green.

## The screen's top instrument fails the fill test, and the cut cache forgot which symbol it replayed (2026-11-25)

- **`DNA-USDT-SWAP` was measured on the owner's seven-day portal download** and the same frozen cell as UP: seven to twenty
  fills a window against the thirty-fill floor, a median net of **-11.84 bps on 2 of 7 days** (UP: 26 fills, **+15.13**,
  5 of 7), and **-14.34** after the handover is priced. The instrument the capture column ranked first of 555 -- 22.03 bps
  a fill -- fails both halves of the registered bar.
- **The reason is that same column**: DNA's tick is 7.3 bps and its book two ticks wide, so a quote three ticks behind the
  touch rests 22 bps from the mid and almost nothing sweeps that far (capture 0.92-3.14 ticks a fill, fill rate 1.6-5.4%).
  The ranking is of captures, not of opportunities; what it buys is the order in which to spend portal downloads.
- **DNA's best quote distance still misses the bar**: at half widths of 1, 2 and 3 ticks the same seven days read 28 fills
  and a median net of -2.36, 21 and **+10.37**, 18 and -11.84 -- all under the thirty-fill floor, and four of seven at the
  best of them. And **the touch is the worst place to quote** even though it fills most, which is the tape's finding
  (informed at the touch, reverting two ticks through) read from the strategy's side.
- **The quote is two orders of magnitude smaller than the book**, measured rather than estimated:
  `tools/study/okx_quote_share.py` reads the window's market turnover (median **1.02 M USD** over two hours) and the
  time-weighted USD resting three ticks behind the touch (**538 USD**) from the archive and the tape, so one contract --
  **2.09 USD** -- is **0.35 per cent of its level and 0.005 per cent of the flow**.
- **A size ladder says the ceiling is not the book**: at 1 / 5 / 10 / 20 contracts the median day pays **+0.13 / +0.40 /
  +1.11 / +2.44 USD** and the median rate does not fall (+15.13 / +10.67 / +24.11 / +28.93 bps), so twenty times the order
  pays 19.1 times the median day and no absorption break appears through 7.75 per cent of the level. **What bounds this
  instrument is its own turnover**, not its depth: 2.44 USD a day at 20 contracts on 825 USD of maker notional in two
  hours, and a volume tier is not reachable from it.
- **The live stage is therefore reserved, not built**: the 1x -> 3x -> 5x -> 8x escalation with three sensors (fill
  notional scaling, five-second markout, `post-only` refusal rate) needs a candidate with a hundred times this turnover,
  and the end form `size = min(cap, floor(alpha x level_usd / one_order_usd))` is anchored at 5 per cent of 538 USD, about
  13 contracts today. `docs/RETROSPECTIVE.md` carries the table beside the four reserved directions.
- `tools/sweep/okx_maker_baseline.py` gains `net_usd` and `notional_usd`, because a size ladder is a question about
  dollars and the bps columns divide them away.
- **`data/runtime/` stays where it is, and now says why.** It is the one folder inside `data/` that a run writes to, and
  moving it to `out/` looked like the obvious fix until the performance history turned out to pin the sha256 of both files
  in a checked block: the move would re-record an artifact that is evidence of an earlier behaviour. `docs/DATA.md` states
  the exception and the reason instead, and `okx_quote_share.py` picks the trade tape by the window's own start (a file
  named D holds [D-1 16:00, D 16:00), so an evening window lives in D+1 -- the same rule the sweep learned the hard way).
  DNA run replayed *UP's* cut (its `instId` read `UP-USDT-SWAP`) and every cell came back `?`. `okx_book_window.py` names
  its emit the same way, so the two sides agree; no recorded number came from the broken run.
- **`data/v2/crypto/` is one folder per instrument** (UP, DNA, BTC, ETH and the single-day symbols), the eighteen
  `*:Zone.Identifier` sidecars are gone, and `tools/lib/okx_data.py` is the one place that resolves both that layout and
  the flat one -- the flat one is what every recorded number was measured from.
- `docs/DATA.md` carries the layout and the counts, `docs/archive/studies.md` the seven days. `code`, `docs` and `fast`
  green.


- **`okx_spread_screen.py --quote-ticks`** adds the column the mechanism lives on: the basis points a quote that many ticks
  behind the touch captures a fill, computed as `quote_ticks x 2 x half-spread / spread-in-ticks`. On one archived sample
  of 555 instruments **71 clear 2 bps of capture**, and the fourteen names the width screen called passers are not among
  them: BE at 1.67 bps over five ticks is 1.00, COST at 1.37 over thirty is 0.14. `UP-USDT-SWAP` reads 16.21 and sits fifth
  of 555, which is the sanity check -- the instrument the line was measured on is near the top of the ranking that matches
  the mechanism. The sample was taken 06:58-07:08 UTC, so it is a reading and not the registered 13:30-15:30 screen, and
  capture is not expectation: a two-tick book pays a lot a fill and fills rarely.
- **The tick on `UP-USDT-SWAP` is 5.2 bps, not half of one.** `0.0001` on a `0.19` price is `5.2` bps, and the profile
  page, this changelog and two tool docstrings said "one tick is half a basis point" for a day -- 10x low, which also made
  "a capture of about a basis point a fill" out of a capture of 1.79-1.98 ticks (9 to 10 bps). Corrected in all four
  places, with the venue's own `tickSz` and the screen's spread-in-ticks agreeing on 5.2-5.4. `.clinerules` gains the rule:
  a tick is a price, not an amount, so a per-tick figure carries its conversion.
- `docs/archive/studies.md` carries the capture table and the corrections, `docs/TOOLS.md` the tool's new column,
  `docs/L2_FLAGS.md` the jump-cooldown paragraph without the wrong premise. `docs` and `fast` green.

## Our own fills are churned evenly at night, and the recording flag is not a silent observer (2026-11-25)

- **`tools/study/okx_fill_profile.py`** buckets our own fills by half hour from the trade stream every replay writes
  (`data/runtime/trades.csv`): fills, the net contracts a bucket moved, the share of its fills that opened a position, the
  capture against the midpoint and the markout at five and thirty seconds.
- **The night has no bucket where the inventory piles up.** Across its sixteen buckets the net a bucket moves is **-2.00 to
  +3.00 contracts** and the share of fills that open stays **37 to 62 per cent** -- half in every bucket. So the night's loss
  is the same churn spread evenly at about a basis point a fill against the 2.00 bps fee, not a few bad buckets a gate could
  learn to avoid; the markout columns (5 s -4.12 to +2.31, 30 s -5.64 to +9.64) carry no bucket-level signal either.
- **`--record-orders` changes the run, which its own documentation denied.** Measured on the same day and window: **117
  fills and net -12.67 bps without it, 207 fills and net +2.71 with it**, the zero-queue count going 116 to 0. The cause is
  not established -- the recorder stamps reports a venue gives no timestamp, and the debounce reads those -- so the flag is
  now documented as a measurement hazard rather than a passive record, and a fill measurement reads the trade stream
  instead: its seven days' line counts match the sweep's fill counts exactly.
- `docs/archive/studies.md` carries the table, `docs/TOOLS.md` the tool, `docs/L2_FLAGS.md` the corrected paragraph. `docs`
  and `fast` green.


- **`--settle-residual`** prices what a run's leftover position would cost to hand over, because `--close-out` cannot: once
  the replay's input ends the touch is a displayed quantity no print restores, so the closing IOC finds no counterparty --
  measured, the eight-hour run with `--close-out` returned numbers identical to the run without it, down to the last digit
  of every column. The flag crosses the residual at the far touch, `--settle-slippage-ticks` (1) further out, paying
  `--settle-taker-bps` (5) of taker fee, and prints `[SETTLE]` with `settle_cost_usd`, `settle_cost_bps` and the marked
  result less that cost. It books no fill, so `[PNL]` keeps its own definition; it refuses a two-book run, and off by
  default it changes no recorded line. `pipeline_accounting_test` pins the arithmetic by hand on a ten-dollar face.
- **The two hours survive the worst case.** Over the same seven days the settlement costs 0.73 to 4.88 bps a day there and
  the median of the settled result is **+11.22** on the same 5 of 7 -- 74 per cent of the net median -- so the window does
  not depend on walking away from its last contracts. At night it costs 0.16 to 1.41 bps and the block stays at -16.67 on
  2 of 7.
- **The time stop was refuted before it was built.** Completed round trips live **0.62 to 4.37 seconds** in the two hours
  and **1.53 to 10.75 at night**, from `--metrics`' own `average_holding_seconds`: seconds, not minutes, and no long-held
  population for a timer to reach. What the night carries is the residual at the end, which the cap ladder and the
  settlement already price. Nothing was added to the strategy.
- `tools/sweep/okx_maker_baseline.py` gains `--hold-time` (asks the run for `--metrics`), `--settle-residual` and
  `--settle-taker-bps`, and three columns: `hold_s`, `settle_bps`, `settled_bps`. `docs/L2_FLAGS.md` documents the flags,
  `docs/archive/studies.md` carries the table. `pipeline_accounting_test` green, `docs` and `fast` green.


- **`tools/study/okx_tape_toxicity.py`** measures the tape's own adverse selection with no strategy in it: for each print,
  the mid's move over a stated horizon with the aggressor's sign, negated -- what the passive side made of that print --
  as a mean, a median, the share of prints the mid moved on, and the count, per half hour. `--min-distance-ticks` keeps
  only the prints that reached a stated distance through the mid, which is the subset a deeper quote is filled by.
- **The frozen cell over four blocks of the same seven days**: 13:30-15:30 gives 26 fills a day, a closed median of
  **+26.18** and a net median of **+15.13** on 5 of 7 days; 08:00-13:30 gives 61-97 fills and a net median of **+4.59** on
  4 of 7; 00:00-08:00 gives 51-152 fills and **-16.51** on 2 of 7; 16:00-24:00 gives two fills in eight hours. So the two
  hours are not a window chosen for being positive: they are the only block that clears the registered bar of 30 fills
  with a positive median on 5 of 7, and the night -- which trades 14.6 times an hour against the window's 13 -- loses with
  four times the fills. The two medians are stated separately because they are different quotients, and quoting a closed
  median beside a net one is how a table ends up carrying two definitions.
- **A density gate cannot be the selector it was meant to be.** At 1.5 prints a second the gate opens fourteen of the
  sixteen night buckets as well as the whole afternoon, so what it separates is the late evening and nothing else.
- **Most of the night's loss is inventory the two hours never carry**: the same eight hours at a ten-contract cap read
  -16.51, at three contracts -6.80, at one -7.20. Loosening the cap accounts for ten of the seventeen bps, and the
  residual is the fills' own economics -- about zero a fill against the window's +0.71. `--close-out` at the end of the
  same run changes nothing, to the last digit: in the replay an IOC at the touch has no counterparty once the input ends,
  so a forced exit is the one desk control this harness cannot price.
- **The tape's adverse selection depends on how far a print reached, and that is the edge**: at a five-second horizon the
  mean maker markout over every print is -0.03 to -2.99 bps in forty-three of forty-eight buckets, while the prints that
  reached **two ticks or more through the mid** (thirteen to twenty per cent of them) read **+0.34 to +5.69 bps, positive in
  thirty-one of the forty buckets measured** from 00:00 to 19:30, the two hours among them (13:30 +1.67, 14:30 +1.37,
  15:00 +2.11) and the night mostly so too (00:00 +2.43, 02:00 +3.30). At the touch the flow is informed; deeper, it
  reverts. At one second the median is zero everywhere, because half a basis point is one tick.
- **So the night's missing thing is conversion rather than opportunity**: the deep prints are more frequent at night (719 a
  day at 00:00, 1,153 at 05:00) than in the two hours (410 at 13:30, 733 at 14:30), and the closed capture a fill is
  +0.71 bps inside the two hours against +0.07 in the morning and -0.09 at night. The measurement that can see it is our
  own fills bucketed by half hour -- one full-day run a day with the fills recorded -- not a tape gate and not a second
  strategy module. `docs/archive/studies.md` carries the tables; `docs/TOOLS.md` the tool. `docs` and `fast` green.


- **`tools/study/okx_halfhour_profile.py`** profiles a day in half-hour buckets with two physical quantities and no P&L:
  the duration-weighted half spread and the prints a second, reported as medians across days with the worst day beside them
  and a count of the days at or above a stated rate.
- **The "dead wide" explanation of the eight-hour loss does not survive it.** The half spread runs 4.78 to 7.10 bps across
  all forty-eight buckets -- 5.27 at 08:00, 6.06 at 13:30, 5.43 at 20:00 -- so on this instrument a quiet book is not a
  wider one. A 0.0001 tick on a 0.19 price is **5.2 bps**, so a two-to-three-tick book is 5 to 8 bps of half spread at any
  hour. (This paragraph first said a tick was half a basis point -- 10x low; the venue's own `tickSz` and the screen's
  spread-in-ticks agree on 5.2-5.4.)
- **The flow is what moves, and only after 16:00 UTC**: the median rate is 0.93-3.18 prints a second all day, but the
  worst day of the seven has **zero prints** in every bucket from 16:00 on and 0.38-1.66 before it. The morning is not
  quiet: 08:00-13:00 prints at 1.3-2.1 a second against the good window's 1.7-2.9.
- **The equity screen's 2.5-prints-a-second gate does not transfer**: only six of forty-eight buckets have a median at or
  above it, two of them at 02:00-02:30, and 13:30 clears it on two days of seven. A gate at that number would close the two
  hours the line earned in. A density gate read off this profile separates the late evening and nothing else -- 1.5 a
  second clears fourteen of the sixteen buckets from 00:00 to 08:00 too, so it is a market-open switch rather than a window
  selector -- and the night run below is the measurement that shows why the distinction matters.
- **So the eight-hour loss is toxicity rather than inactivity**: same half spread, comparable flow, and the closed capture
  per fill fell about ten-fold. The next measurement that could see it is a per-bucket markout on the strategy's own fills,
  which needs the run rather than the archive.
- `docs/archive/studies.md` carries the table; `docs/TOOLS.md` the tool. `docs` and `fast` green.

## The second layer is what buys fills, and the size is what sells them (2026-11-25)

- **`--extra-layers=L`** quotes L further passive layers one tick behind the first, at the same size, inside the same
  asymmetric refresh. Zero is the default, every recorded number is unchanged, and `l2_strategy_test` pins it: one extra
  layer is four orders, the two behind the first by a tick each and at its size.
- **The fill count is bounded by the number of orders, not by their size.** At 10 ms with `--post-only` and the 40 ms
  refresh: one layer and one contract reads 17 fills a window, a median closed of +12.40 bps and 6 of 7 days positive;
  two contracts reads 21 fills and **+20.46** with the same 6 of 7; four contracts reads 24 fills and **+11.76** with 4 of
  7, which is more size through the same thin book and the closing-side result again.
- **Two layers at one contract is the best cell**: **26 fills** a window (one day at 37) and a closed median of **+26.18**
  on 5 of 7 days, at the maker's 2.00 bps.
- **Two layers at two contracts lifts the count to 29 and drops the median to +4.53**, so the two levers do not add: the
  layers buy the count and the size sells the capture back.
- **The bar's third condition was written wrong and is corrected rather than bent.** It asked for `max_abs_inventory`
  unchanged; a second resting order a side can also fill, so it goes from 3-9 contracts to 4-10 and reaches the default cap
  of ten exactly. The condition worth registering is "inside the configured cap", and the layer is a sized decision because
  of it.
- `docs/archive/studies.md` carries the table; `docs/RETROSPECTIVE.md`'s multi-layer row moves from registered to built.
- `docs` and `fast` green.

## Three ways to reach thirty fills, and only one of them was left standing (2026-11-25)

- **`--close-side-ticks=N`** brings the side that reduces the position in to N ticks while the position is not flat, and
  leaves the adding side at its own distance. Zero is the default and every recorded number is unchanged; it is pinned by
  `l2_strategy_test` (a long position moves the ask and not the bid).
- **Widening the clock buys the sample and gives up the edge.** At 10 ms with `--post-only` and the 40 ms asymmetric
  refresh, the 13:30-15:30 window reads 13-27 fills at a +12.40 bps closed median with 6 of 7 days positive; 08:00-16:00
  reads **49-93 fills** at **-1.89** with 3 of 7. The two hours were never an arbitrary lock -- they are the best part of
  the session -- and 16:00-24:00 UTC produced **two fills in eight hours**, so the evening is not a window at all.
- **The closing-side skew does not pay at this size**: fills stay at 14-26 against 13-27 and the closed median falls from
  +12.40 to +11.41, with the positive days from six to four. It trades capture for turnover, and turnover is not the
  constraint.
- **Multi-layer quoting is registered and not built**, with its bar in `docs/archive/studies.md`: two passive layers inside
  the same caps and the same refresh, fills above the one-layer row and the closed median positive on 5 of 7, with
  `max_abs_inventory` unchanged.
- `docs` and `fast` green.

## Two execution mechanisms, registered before the run and measured (2026-11-25)

- **Four flags, all defaulting to zero** -- `--jump-window-ms`/`--jump-ticks`/`--jump-cooldown-ms` (pull the quotes and
  hold them off when the mid crosses N ticks inside M milliseconds) and `--advance-debounce-ms` (only the side that would
  move towards the market waits for the fair price to settle; the side that retreats goes out at once). Zero keeps every
  recorded number where it was, and both mechanisms are pinned by `l2_strategy_test`: a jump pauses and releases while a
  one-tick drift does not, and the chasing side is held while the retreating one goes out and follows once the fair is still.
- **The bar, written before the runs**: at 80 ms and `--post-only` the rescue configuration reads 1-10 fills a window at
  2.00 bps, so a mechanism that helped would raise the median above 20 fills with the fee at 2.00 and the capture at +1.0
  tick or better on five of seven days.
- **Both fail at 80 ms** (fills 0-4), for a reason that is not about gating: at that latency every quote is stale on
  arrival, so only the few the sweep had not reached yet can rest, and pausing removes them.
- **At 10 ms the cooldown kills the flow** (38-75 fills down to 0-1). On an instrument whose tick is 0.0001 on a 0.19 price
  -- about half a basis point -- a two-tick move inside 40 ms is the normal state rather than a jump, so the gate pauses
  almost always.
- **At 10 ms the asymmetric refresh halves the fills and flips the sign**: 38-75 down to 13-27 at the maker's 2.00 bps, and
  the closed median from -4.61 bps (1 of 7 days positive) to **+12.40 (6 of 7)**. That is below the thirty fills the project
  requires, so it is registered as the next sample: thirty fills or more a window and a positive closed median on five of
  seven days, which needs a larger clip or a busier instrument rather than a different threshold.
- `docs/archive/studies.md` carries the table; `docs/L2_FLAGS.md` carries the flags with these numbers beside them.
- `docs` and `fast` green.

## The link is measured, and it is the wall above the fee (2026-11-25)

- **`okx_probe.py` gained `rtt`, `warm` and `touch`.** `rtt` places a `post_only` buy two percent under the best bid, so
  the order must rest and the venue's reply is the placement's; it times that reply and the cancel after it, and prices
  the same order one tick through the ask to see what the venue does with a crossing quote. `warm` does the same on one
  kept-alive CONNECT tunnel, because `urllib` pays a connection and a TLS handshake per request and the two numbers
  answer different questions. `touch` prices the quote at the best bid, where only a book that moved inside the round
  trip can make it marketable, so its decline rate *is* the stale-on-arrival rate. The instrument list is read unsigned:
  the demo header returns only the demo's own subset, which is how `UP-USDT-SWAP` first answered "does not list".
- **Measured on 2026-11-25, two paths.** Direct, with no proxy: placement p50 **118 ms** (p95 123) on one kept-alive
  tunnel and 194 ms (p95 2817) on a fresh connection; cancel 76 and 161 ms. Through the operator's proxy the same two are
  **414** (p95 481) and **632** (p95 977); cancel 341 and 584. That is an order of magnitude above the 30-80 ms the latency
  work had assumed, and it is three times the direct path. A second run at 11:21 UTC read **77.5 ms** (p95 81.4) kept-alive
  and **127.5 ms** (p95 136.0) fresh, cancel 78.0 and 131.5, so the direct path is a range -- 77 to 118 ms -- rather than a
  constant, and its fast end is still seven times past the 10 ms where the capture halves.
- **Where the delay is, and it is not the venue.** `curl` splits a cold request: direct, 70 ms to connect (13 ms of TLS
  after it) and **98 ms** from request to first byte; through the proxy, 723 ms of CONNECT and TLS and **386 ms** of
  request. The venue is a Cloudflare CNAME (`www.okx.com.cdn.cloudflare.net`), so the direct 98 ms is the edge reaching
  OKX's origin, and the proxy is the larger half -- the same proxy answers `api.github.com` in 435 ms a request and takes
  5.6 s to establish a connection to `www.google.com`. An order costs no more than a public read on the same tunnel (336
  against 326 ms), so the matching engine is not in the number.
- **`post_only` at the touch was declined 14 times out of 14** on the proxy's 553 ms round trip, and **12 of 12 on the
  direct route** at a 113 ms placement p50 (none rested, none filled) -- so a quote at the touch is marketable on arrival
  every time at both measured routes, and the decline is not the proxy's. Its through-ask version rested six of six
  direct, which is the same staleness from the other side.
- **`UP-USDT-SWAP` is not tradable in the demo environment**: eight placements, resting and crossing, every one
  `code=1/sCode=51001` with no `ordId`. Its live record says `state: live`, `ctVal 10`, `tickSz 0.0001`, lot and minimum 1
  -- so it is a demo-environment gap, not a venue one, and this tree holds only a demo key.
- **The configuration is a maker only near zero latency, and the first version of this grid was a 1000x unit error.**
  `--latency-us 10/30/50/80` is **microseconds**, so a five-point table labelled "0 to 80 ms" was five zero-latency runs and
  every conclusion drawn from it was about no delay at all. Caught by the rule that a too-clean result has to reconcile: a
  single day at a true 80 ms gave 41 fills where the "80 ms" row claimed 100 to 388. Re-derived at the real values, the
  rescue configuration's fee runs 2.00 bps at 0, 4.46-10.28 at 10 ms, 7.22-16.12 at 30, 15.89-29.91 at 50 and
  12.74-129.47 at 80, with the capture falling from +1.4..+2.7 ticks to +0.05..+0.99 at 10 ms and negative from 30.
  `okx_maker_baseline.py` now warns when the value is under one millisecond, so the same 1000x shows up in the log.
- **With `--post-only` at 80 ms the fills fall to 1-10 a window** against 22-46 unflagged, because the quotes that would
  have crossed are declined: one day decomposes into `post_only_cancels=41`, 8 fills and zero taker fills, against 41 fills
  of which 35 were taker without the flag. Both branches end the line at this latency; which one a session gets is the flag.
- `--latency-us` for this host is 77,500 to 118,000 direct or 414,000 through the proxy, and both are seven to forty
  times past the point where the configuration is a maker.
- `docs/archive/studies.md` carries the tables; the frontier map's UP row now names the route as the first gate and the
  fee tier as the second.
- `docs` and `fast` green.

## The burst row was one day of five, and the tool now takes a day list (2026-11-25)

- **`cascade_probe.py` takes `--days`**, so the multi-day test of a row is one command rather than a shell loop around
  one: one `###` table and one `[CASCADE]` line per day, and a day whose file is missing stops the run naming the path.
  `--day` and its default are unchanged, so the two pinned tool blocks reproduce to the digit.
- **The 900 s taker-entry/passive-exit row was 2020-04-01 and nothing else.** The same run on the five BTC days on disk
  reads net +7.40, -17.16, -27.14, -7.83 and -16.01 bps, with a negative median on all five (-9.34 on the day that
  produced the +7.40): one positive net cell out of the fifteen the three-horizon run prints.
- **The 30 s continuation replicates and is still inside the toll.** Each day's best horizon pays +6.09, +1.33, +3.79,
  +2.92 and +1.98 bps of gross against a 10 bps taker round trip, so every day nets -3.91 to -8.67. The direction is
  real and its size is a fraction of the fee, which is the stronger closure and the one a single day could not make.
- The 900 s day and the worst of the four that disagree with it are pinned in `docs/archive/studies.md`, which carries
  the five-day table; the frontier map's burst row now records the test as done rather than as the next thing to run.
- `docs` and `fast` green; the two blocks that were already pinned reproduce to the digit.

## The engine stops dropping a cancel that races its placement (2026-11-25)

- **`MatchingEngine::cancel_order` now holds a cancel that names an order still in flight and applies it when the
  order lands**, instead of returning silently because `order_index_` did not contain the id yet. The cancel's own
  delay is still measured from the request (`event_counter_ + cancel_delay_events_`, `now_us_ + cancel_latency_us_`);
  what changed is that a venue which cannot cancel what it has not received waits for the order rather than
  discarding the request. `in_flight_ids_` is the table that answers "has the venue received this order", filled by
  `send_order` and emptied at the landing in `advance_to`, so from then on `order_index_` is the single answer for a
  cancel that arrives afterwards. A cancel naming an id the engine was never given is still dropped: parking it
  would fire the cancel against a later order that reused the id.
- **Pinned by three cases in `matching_engine_test`**: a cancel whose own latency expires before the order lands (the
  request is held, and applied in the event that delivers the order), the measured shape where both legs share the
  link and the cancel is decided while the order is in flight (the quote is a working order until the cancel's own
  due time), and a cancel that arrives after a fill (nothing is reported and nothing is carried over).
- **The recorded latency table moved, and the movement is the defect being removed.** `tools/sweep/latency_sweep.sh`
  re-derived: SPY at 1 ms each way reads 923 fills and +9.6455 USD net (was 921 and +1.8855), at 5 ms each way 901
  fills and -179.947 (was 9 fills and -1143.75), at 20 ms -171.173 (was -755.1) and at 50 ms +254.603 (was -1044.07).
  The synchronous rows, the cancel-only row and both MES rows are unchanged. Above one millisecond the window's own
  mark dominates the column now, so `docs/performance/history-04` rests its verdict on the one-millisecond row and
  says why the rows above it cannot price the delay.
- **The OKX maker's 30 ms collapse was this defect, not the link.** The repair is what made a 30 ms run place its quotes at
  all: the pre-fix run placed one to forty-four fills across seven days, and at a true 30 ms the same configuration now
  fills 89 to 138 a window. The five-point "0/10/30/50/80 ms" table this entry first carried was **microseconds** rather
  than milliseconds -- a unit error corrected in the entry above -- which is why the fee ladder (2.02 bps to 5-13 at 30 ms)
  is the number to read rather than the net.
- `docs/performance/history-04`'s marked block re-derives to the new `then:` values; `code`, `docs` and `fast` green.

## The screen gains the tick width, and its readings move out of /tmp (2026-11-25)

- **`okx_spread_screen.py` reports the median spread in ticks beside the half-spread**, joining the venue's instrument
  table for `tickSz`, and a candidate now has to pass three thresholds: half-spread >= 1.2 bps, **median spread >= 2
  ticks**, and 24-hour notional >= 500,000 USD. The middle one is what decides whether a symbol is the same kind of
  animal as `UP-USDT-SWAP`: a cheap coin can show 2.2 bps of half-spread with its bid and ask one tick apart, and the
  rule that earns on this instrument -- three ticks behind the touch -- cannot be placed in a one-tick book at all.
- **The raw reads are archived by default**, to `data/v2/crypto/okx_tickers_<utc>.json` rather than a `/tmp` path: a
  screen whose readings are gone cannot be re-derived, and this tree has lost `/tmp` scratch twice. The file is input and
  git-ignored, so archiving costs no repository bytes, and `--read` replays it to the same table.
- Validated live with two reads at 04:00 UTC: the tick column joins against the instrument table (HYPE-USD-SWAP 16 ticks,
  RAVE-USDT-SWAP and CT-USDT-SWAP 3) and the archived set re-derives to the same numbers.
- **The run prints three tier counts** -- 1.2 / 2.0 / 2.5 bps under the same tick and volume floors (99 / 76 / 66 at
  20:11 UTC on 2026-11-24) -- so one run answers "how many candidates at each width" rather than leaving a reader to count
  rows in a table sorted by width.

## The rescue configuration does not survive a 30 ms link (2026-11-25)

- **`okx_maker_baseline.py` grew `--latency-us`**, which sets `--order-latency-us` and `--cancel-latency-us` together, so a
  sweep can ask the same question the demo venue answers by existing: 30-80 ms of round trip.
- **At 30 ms the flow collapses, not merely the edge.** On one measured day the baseline configuration submits **31 quotes
  in a two-hour window** where zero latency gives **1,275**, and 29 of the 31 fill; the fee rises from 2.00 bps to **4.25**
  because a quote that lands stale is marketable on arrival, and `max_quote_lifetime` goes from 684 cycles to **26,570**.
  The deep-quote configuration behaves the same way (one to forty-four fills across seven days against 183), so what this
  exposes is the link and not the deep quote.
- **The collapse is a defect in the replay's cancel path, not a property of the strategy.**
  `MatchingEngine::cancel_order` parks a cancel only when `order_index_.contains(order_id)` and returns silently
  otherwise, so a cancel issued while its order is still waiting out the *placement* latency is dropped; with both legs
  delayed equally that is the common case, and the strategy then waits for an order it already tried to pull.
  `[TRACE_HINTS]` names that order -- `longest_wait_filled_cycles=26570`, a fill wait rather than an age refresh. The fix
  belongs in the engine (a cancel that names an unknown order waits for it to arrive), and until it lands the 30 ms
  number is a statement about the replay rather than about the configuration.
- No recorded number moved; `docs` and `fast` green.

## The burst family's passive legs are measured, and they make it worse (2026-11-25)

- **`cascade_probe.py` grew the two leg modes** (`--entry-mode`, `--exit-mode`, `--maker-bps`, `--tick`,
  `--patience-s`) with the conservative fill rule the question needs: a passive leg is not filled because the price
  touched it, only when the tape prints at least one tick *through* the limit, and the fill is at the limit. The defaults
  are unchanged and the default run reproduces the recorded marks to the digit (`best_net_bps=-3.9088
  gross_at_best_bps=6.0912`), so the block that was already there still pins the taker/taker shape.
- **Both legs passive costs six basis points of fee and eleven of gross**: on BTC 2020-04-01 the 30 s gross falls from
  +6.09 to **-5.23** (net -9.45), with 16 of 84 events never filling at all. A passive entry is filled only when the
  market comes back through the price the burst printed at, which is the moment the continuation has failed -- so P_fill
  is negatively correlated with the outcome, and pricing two passive legs as a fee saving overstates the result.
- **A taker entry with a passive exit reads +2.50 gross at 30 s and +14.44 at 900 s**, the latter on a median of -9.34
  and 76 fills: a mean carried by two or three events, recorded as the next row to test on more days rather than quoted.
- **The section's own earlier sentence is corrected in place**: the taker entry was written as "the worse of the two ways
  to join it", and it is the better one in outcome for the reason above.
- No recorded number moved; the new block pins the passive pair. `docs` and `fast` green.

## The quote geometry is swept, and one configuration reaches the fee (2026-11-25)

- **A 3x3 grid over `--base-spread-ticks` and `--refresh-price-ticks`** (seven windows, conservative fill) says the
  geometry alone cannot turn the sign: the best cell is -0.46 bps mean against the baseline's -3.43, and no cell has a
  positive median. A refresh on every tick beats 2 and 4 in every column, which is the opposite of the advice a deep
  book gives.
- **One configuration earns 1.5 to 3.1 bps of a 5 bps half-spread in all three windows measured**: a quote three ticks
  deep behind `--edge-cover-ticks=1`, with a refresh on every tick. `closed bps` -- round trips only -- reads +3.12 at
  13:30-15:30, +1.52 at 09:30-11:30 and +2.01 at 20:00-22:00, mean **+2.2** against a 2.00 bps fee, so the instrument is
  a break-even maker at the retail schedule rather than a losing one. `--min-spread-ticks=2` instead earns +3.01 in the
  US window and -1.69 in the morning one, which is why the two gates are reported separately.
- **The implied break-even fee is about 1.5 bps**: at 1.5 the three windows read +1.62, +0.02 and +0.51; at 1.0, +2.12,
  +0.52 and +1.01. That is an account-tier question, not a code one.
- **`okx_maker_baseline.py` grew `closed_bps` and the grid mode**, and two defects are fixed with it: a window cut left
  half-written by a concurrent run is now detected and re-cut, and the trade file is chosen by the window rather than by
  the book's day, because OKX cuts trades on midnight UTC+8 -- reading the book's day gave an evening window two hours of
  book against zero trades and seven days of zero quotes that looked like a gate blocking a quiet market.
- No recorded number moved; `docs` and `fast` green.

## The venue's own archive answers the fee question (2026-11-25)

- **Two tracked tools make an OKX day archive measurable.** `tools/study/okx_book_window.py` cuts a UTC window that
  opens on a snapshot -- the reader refuses a depth file whose first record is an update -- and reports the
  *time-weighted* half-spread, because a book that is one tick wide ninety percent of the time is not described by the
  median of its records. `tools/sweep/okx_maker_baseline.py` runs the maker over one window per day per queue model.
- **The seven-day answer, on `UP-USDT-SWAP` at 13:30-15:30 UTC**: the half-spread is 4.43 to 7.69 bps, two to three
  ticks, so the fee is 0.26 to 0.45 of it. The maker's median net is **-2.28 bps** under the conservative fill model
  (cancels never move a resting order forward) and -2.49 under prorata; one day of seven is positive either way; the fee
  is exactly 2.00 bps with no taker fill, so what the window loses is gross.
- **The capture is 24 percent of what the book offers**: `gross_closed_per_fill_usd` is +1.23 bps against a 5.20 bps
  half-spread, and `breakeven_maker_rate` is +0.87 bps -- the fee would have to be under 0.87 for that window to pay.
- **Two inventory experiments argue against the obvious fix**: tightening the cap from ten contracts to three made the
  median worse (-2.28 to -3.22) and a taker close-out at the window's end changed nothing (-3.27, ending flat), so the
  loss is realised inside the window rather than carried to the mark.
- No C++ changed and no recorded number moved. These numbers are prose rather than marked blocks: they need 140 MB of
  archive that git does not carry, and both headers state the commands that re-derive them.

## A snapshot at a new timestamp replaces the book (2026-11-25)

- **The book clears when a snapshot arrives at a new exchange timestamp** (`l2_orderbook.cpp`): clearing only on the first
  snapshot batch is right for a cumulative snapshot delivered in pieces and wrong for a top-of-book stream where every
  message is a snapshot. OKX's `bbo-tbt` therefore left the level each snapshot superseded standing, and the book read
  crossed in 84 of 94 batches of an `UP-USDT-SWAP` session; after the change the same session reads no crossed batch,
  `bbo_quote_observations` 2 of 14 quotes, `working_orders=2` and `total_order_lifetime_cycles=84` against 0, 0 and 14. No
  recorded number moved -- `code` re-derives them all, because every archived window opens with one snapshot per connect.
  Pinned by `l2_orderbook_test`, which also asserts that the second piece of one snapshot, sharing its timestamp, keeps
  the side that already arrived.
- **`soak.sh` sizes a non-BTC instrument by its own contract and refuses an instrument it does not know**: the relay's
  mapping defaults to BTC's, so six UP orders asked for 1000 contracts (~1,895 USD) where the strategy believed it sent
  1.90. The order leg carries only the two size flags; `--tick-size` and `--lot-size` stay with the leg that owns the price
  grid, which a relay without `--order-listen` refuses.
- **The demo account does not carry `UP-USDT-SWAP`**: every order is answered `sCode=51001` in all three sessions. A
  measurable market from the real stream plus an order leg the demo refuses is not a measurable instrument, so this
  instrument's queue-model question needs the venue's L2 archive or an account that carries it.
- **A correction to what this file said an hour earlier.** The six rejects were recorded as the venue's margin check
  standing between the harness and a thousand-fold position; the order log shows `51001`, so no sizing check was ever
  reached, and the record now says so in `docs/archive/studies.md` too.

## One invariant instead of three hand-copied minima (2026-11-23)

- The three OKX swap factories repeated the same seven fields, and `min_order_quantity` was a figure copied by hand --
  the copy that once put BTC's 10000 on ETH and was ten times too small. `okx_swap_spec` computes the minimum from the
  contract value and the scale, and each symbol is one line: `common` 989 -> 981 lines, while the three minima
  `l2_replay_test` pins are unchanged.
- No behaviour: `code` and `fast` green and no recorded number moved.

## The screen's passer, and the session that could not measure it (2026-11-23)

- **`UP-USDT-SWAP` is registered** from the venue's own metadata: a 0.0001 tick, one contract carrying ten UP, a minimum
  order of one contract, the venue's 2/5 bps schedule. `l2_replay_test` pins those five numbers, because registering a
  symbol is a claim that its numbers are re-derivable.
- **`tools/live/soak.sh` grew `INST`, `INST_CODE` and `MARKET_DEMO`**, so the two legs can point at different venues: a
  symbol the demo does not quote takes its book from the real public stream while its orders still go to the demo account.
- **What the sessions said.** The demo's public feed delivered no market events for the symbol at all; with the market leg
  on the real stream the engine quoted 62 times and got 62 reports back with no refusals -- the venue knows the
  instrument -- while `bbo_quote_observations=0` says the book never arrived and nothing filled. The queue-model question
  the instrument was registered for still needs a book: the venue's L2 archive (portal-only) or the relay's market leg for
  this symbol.

## The two screen cells that had never been run (2026-11-23)

- **Small caps: the spread half passes, the activity floor does not.** Seven names under twenty dollars for 0.053662 USD of
  credit: IONQ's spread at the trades is 3.494 times the round turn of the account's own schedule and FCEL's 3.874, and
  they trade 1.473 and 0.594 prints a second against the pre-registered 2.8; the other five are one tick wide and fail
  both halves. JNUG's shape, one price band down.
- **Perpetuals: one instrument passes both, and the bound was a property of BTC.** The venue's public tickers route
  returns 548 two-sided swaps, 107 of them with a half spread at or above the 2 bps it charges while trading at least a
  million dollars a day. Sampled a second apart, `UP-USDT-SWAP` quotes 8.51 bps (4.25 times the fee) at 3.21 prints a
  second, which passes both halves; DOS and SKY pass the spread and trade 0.38 and 0.52 prints a second.
- **The demo's book understates the real one by ten times.** `BTC-USDT-SWAP` at the venue quotes a 0.01 bps half spread
  against the demo window's `spread_bps=0.101`, so the demo's 19.8 half-spreads is a number about the demo.
- Recorded in `docs/archive/studies.md` with no marked block: a live snapshot and a downloaded archive are not numbers the
  gate can re-derive, and the section says so rather than implying a check it does not have.

## The fee tier, and the cascade window (2026-11-23)

- **A one-basis-point maker tier does not save the demo line.** The breakeven maker rate is the gross minus the exit's
  0.21 bps: four of six windows need a rebate to reach zero, the best needs 0.86 bps against the 2.00 charged, and at a
  retail 1.00 the six read -1.62 to -0.14 bps. Arithmetic on the recorded windows, not a new run.
- **The cascade window is not reachable from the entry modes this tree has.** Measured on the 2020-04-01 BTC slice: a
  passive entry fills once in 61 quotes (fill rate 0.0164, no markout resolving), and the touch entry at the same inventory
  limit fills all 514 while giving up 1.03 ticks at the touch and a further 1.02 to the mid's drift -- gross -0.318 USD
  against 2.57 USD of taker fees. A traded cascade needs an entry rule keyed on the cascade, which is a new signal row.
- Pinned by `history-01`'s block (three runs) and by the live page's fee table.

## The passive entry is closed by measurement (2026-11-23)

- **The bar registered before the run fails on its two sign conditions.** Five own-side runs on AMD, AAPL and TSLA
  marked out -0.75, -0.33, -0.29, -0.05 and +0.76 ticks, and gross -0.45 to -0.79 bps of maker notional against 2.2 to
  3.2 bps of fee: the fill is adversely selected on four of the five and the gross is negative on all five.
- **The third condition is a data question and cannot move a sign.** The bar asked for three windows; the tracked data
  holds one session per name, and a second one is a paid Databento download rather than a run. It cannot pass the bar
  either, because the first two conditions are signs and both point the same way on every run.
- **What would reopen it is a measurement, not a parameter**: a markout that is positive in the same test, which would say
  the fill is informed rather than selected. Width, age and clip have all been moved and none of them touches the sign.

## An acknowledgement that trails a fill is not a fault (2026-11-23)

- **`OrderLedger::mark_resting` no longer refuses an acknowledgement of an order that already filled part of itself.**
  The engine reports a marketable limit as a fill and then as resting its remainder, and the state machine's linear
  order (Pending, Resting, PartiallyFilled) cannot express "filled, then rested", so the second acknowledgement was
  counted as a backwards transition: 5,889 of a 12,000-order run, with those orders' queue measurements lost.
- **What the fix changed in a run.** `invalid_order_transitions` is absent from `[EXECUTION]` (zero, printed only when
  non-zero) and `queue_profile_orders` rises from 0 to 5,889 -- the acknowledgements that had been swallowed, now read
  once each: on the same window, 517 of them joined a level that was already cleared.
- **The rule is now stated where it is checked**: an acknowledgement moves an order to Resting only from Pending; on a
  working order it applies its fields once and leaves the state; a terminal order keeps its state, because the report
  was accounted for elsewhere (a late fill, an orphan). `order_state_test` pins the repeat, the trailing acknowledgement
  and the engine's own order of events.

## A passive entry for the trend line, and what the conditional fill costs (2026-11-23)

- **`--trend-entry=side=own[,ticks=N][,max_age=VIEWS][,cooldown=VIEWS]`.** The trend line's clip is priced on our own
  side of the book, `ticks` inside the touch and clamped so it never reaches the opposite one, so the order rests and
  the fill pays the maker rate; an entry older than `max_age` views is cancelled and the next one waits `cooldown`
  views. Placing and cancelling read one `entry_price()`, because a cancel rule written against the touch fires on the
  order's own improve distance. The defaults are the touch entry, so every recorded trend number keeps its behaviour.
- **The bias is now a printed field.** AMD 722 passive fills (1.1% of quotes) captured +7.63 ticks and marked out
  -0.75; AAPL 652 (2.9%) captured +1.67 and marked out -0.33, with `taker_trade_count=0` in both. Fills arrive when the
  move is about to stop, which is the conditional-fill bias the design predicted.
- **The commission floor is half the fee at this clip.** The average fill was 20 shares, so 380 of 722 and 326 of 652
  order instances paid the 0.35 floor: 45 and 53 percent of the fee bill. The rate is the whole cost only from 100
  shares upward.
- **The touch control is a warning, not a baseline**: 11,597 taker fills lost 56,732 USD gross. The run also reported
  `invalid_order_transitions=5889`, which was a defect rather than the taker path's shape -- an acknowledgement that
  trailed a fill -- and is fixed in a later entry.
- Pinned by `tests/trend_l2_test.cpp` (the own-side price, the one-tick clamp, the age cancel and the cooldown).

## The two gates in one command, and the terminal out of the path (2026-11-23)

- **`tools/study/fee_gate_table.py`.** One row per instrument with both gates a strategy review asks about: the fee
  against the half-spread at the trades (`screen_spread.measure`) and `R_max` against its 71-print reference
  (`venue_ratio.measure`). It imports the two measures rather than recomputing them, so each ratio keeps one definition
  in the tree; `venue_ratio`'s clip loop is now `fill_ratios`, shared by its own main and the new tool.
- **What the join says that neither half did.** On the three tracked mbp-10 windows the screen's numbers reproduce
  exactly (half spreads 1.5, 0.5 and 2.5 ticks; fee/half-spread 0.23, 0.70 and 0.14) and the capture gate disagrees with
  the spread gate on SPY -- 255 prints a fill against a spread that is not three fees wide. A wide book that barely
  moves is a different instrument from a tight one that moves, which is why both gates are printed.
- **`viability.py --derive` confirms nothing moved**: `r_max recorded=56 measured=56` and `checked 8 recorded verdicts, 0
  mismatch`.
- **The terminal left the execution path.** It lost a command's end four times on 2026-11-23, each time dropping every
  later command for about twenty minutes. `.run.sh daemon` and `.run.sh queue <verb>` make a request a file that a
  detached loop executes through the same `start()`, and `.run.sh job <name> '<command>'` gives a long inspection its own
  verb name so two can run at once. Verified while the terminal was wedged: a `status` and two jobs were enqueued by
  writing files and both ran.
- `tools/live/soak.sh` loses a duplicated `kill`/`wait` (the EXIT trap owns it) and reads its artifact prefix from one
  variable.

## A live session is a verb, and add-liquidity-only gets its baseline (2026-11-23)

- **`tools/live/soak.sh`, started as `bash .run.sh soak <post-only|baseline>`.** The 300 s `ws` shape had been typed into
  a terminal three times from a scratch script, and the third time cost the terminal: the session ran fine in the
  background while the tool's shell executed nothing for twenty minutes. A live session is now a verb, like every gate,
  and the tool takes `STARTING_CASH` (the probe's `eq`, refused when absent), starts both relays, records the market and
  the order leg, and prints the summary grep.
- **The flag's baseline, in the same hour.** `post-only` read 2.21 bps of fees (2.00 maker + 0.21 exit), gross +0.085,
  net -2.12; `baseline` read 5.64 bps (2.00 maker + 3.64 taker on 17,903 USD of aggressive notional, 42% of what it
  filled), gross -0.633, net -6.27. The flagged window's gross is the higher of the two, so the fills the venue declined
  were the bad ones; the saving is 3.43 bps here against 1.15-1.67 against the 2026-11-21 windows, which makes it a
  property of the flow rather than a constant.
- **What a replay can and cannot say about the flag.** On the recording of the flagged window, `--post-only` changes
  nothing without `--order-latency-us`: the quote and the book are the same event, so nothing is marketable on arrival.
  At 100 ms the matcher declines two of nine fills' worth of quotes and the fee rate moves from 6.66 to 2.00 bps -- the
  mechanism, on nine fills, not a measurement. `docs/L2_FLAGS.md`'s latency row says so, so a reader does not run the
  no-op and conclude the flag does nothing.
- Pinned by `docs/TOOLS.md`'s row for the tool and `.clinerules`'s rule for the terminal; no source changed, and no
  recorded number moved.

## The kind of order reaches the venue, and a live fill carries the role it was charged (2026-11-23)

- **`--post-only` was a replay-only flag.** The engine's order line has one slot for the kind of order, and `PostOnly`
  now travels in it: `LiveVenue` writes `ORDER <id> <symbol> <B|S> <price> <qty> PostOnly`, both relays translate that
  slot into the venue's `ordType` (`post_only`, `ioc`, `fok`, and `limit` for Gtc), and a venue that cancels such an
  order answers `CANCELED <id> post_only`, which the engine already counts as `post_only_cancels`. Until this, every
  kind left as `limit`: an IOC rested at the venue and an add-liquidity-only quote was the taker fill it exists to
  prevent, which is the flag's whole purpose silently absent.
- **A live fill now says which role it was charged as.** The relay forwards OKX's `execType` (`M` rested, `T` took) as
  a sixth field after the fee (a fill with no fee states a zero to keep the position), and `LiveVenue` books
  `liquidity_role` from it instead of assuming a Gtc fill was a maker. That assumption put a live run's taker fills in
  `maker_trade_count`: the three windows' 3.36-3.88 bps of maker notional decompose to 21-27% taker by arithmetic on
  the fee amounts, and the venue's own word is what turns that arithmetic into a measurement.
- **No recorded number moved, and one live window measured the change.** Every recorded run is a replay -- the matcher
  sets the role and reports the post-only cancel itself -- and the simulated venue sends neither new field, so the
  shorter `FILL` line and the adapter's rule remain what a session without a venue's `execType` books. Measured `ws` on
  `BTC-USDT-SWAP`, 300 s, cap 4000, rate 2/s, `--post-only`: `[PORTFOLIO] fees_paid=4.58896 maker_fees=4.16071
  taker_fees=0.428249 maker_trade_count=44 taker_trade_count=1` and `[PNL] gross_pnl_usd=0.17664
  maker_notional_usd=20803.6`, which is 2.21 bps of maker notional against the three windows' 3.36-3.80, with 87 of 172
  quotes declined as marketable and both `[RECON]` lines `ok`. The fee saving's ceiling, 1.36 to 1.88 bps against a gross
  capture of 0.92-1.07, was the prediction; 1.15 to 1.67 was the measurement, and the remaining gap is gross.
- Pinned by `venue_order_translator_test` (the four `ordType` values, the role with and without a fee, the post-only
  cancel), `live_venue_test` (the `PostOnly` frame, the venue's role overriding the rule, an unknown role dropping the
  frame) and `venue_relay.py --self-test` (the same shapes).

## A recovery that could not resolve an order refuses to start (2026-11-22)

- **`OrderQuery` gained a third answer, because two were being asked to mean three things.** `Unknown` was both "the venue
  answered that it no longer holds this order" (a fill or a cancel, which the venue cannot tell apart) and "nobody
  answered". A recovery read the second as the first and counted every inherited order as gone -- measured 2026-11-21: a
  relay that replied `PENDING unavailable` left the session with nothing to cancel and no way to know it.
- **`LiveVenue::query_order` answers only from a list this client was actually given.** An `unavailable` reply clears the
  flag, and so does a refresh that times out (`query_timeout_ms`), because a list that predates the question is not an
  answer to it. It reports `Unanswered` until a fresh list arrives.
- **`ResumeSummary` counts that bucket** as `orders_unknown` and prints `unknown=<n>` on the `[RESUME]` line, only when it
  is not zero. `orders` is `live + gone + unknown`; `cancelled` is a subset of `live`.
- **Both resume sites refuse to start on it** -- the single-book path and the two-book one -- with an error naming the
  symbol and the count and pointing at `tools/live/okx_probe.py`, before anything quotes. The account is restored and the
  process exits non-zero; the operator checks the venue with the probe and starts again. This is the fail-closed policy
  the milestone closes on: an order nobody has looked at is a position the session cannot see.
- **The exit is not re-sized after a growth, and that is now written down where the growth is detected.** `grew=<units>`
  means the session no longer knows what it holds, so the session stops and says so -- one attempt, the position left on
  the ledger -- rather than looping a re-sizing state machine over a venue that has just proved it is faster than the
  client.
- **Pinned by** `session_state_test` (a venue that does not answer leaves `orders_unknown=2`, `gone=0`, nothing cancelled,
  and the count reaches the line) and `live_venue_test` (before any answer, and after `PENDING unavailable`, `query_order`
  is `Unanswered`; a fresh list is what clears it, and a dropped frame is not a list). No recorded run moved: a healthy
  recovery prints the line it always did, with no `unknown=` field.

## The demo milestone is closed (2026-11-22)

- **What was proven**: the full live path -- venue market leg, order leg over REST, fill accounting, order state machine,
  rate and session bounds, and a ledger reconciled against the venue's own statement at both ends of every session.
  Measured over three five-minute windows: worst `equity_break_usd=-0.00386365`, one session comparing a **nonzero**
  position with the venue's contract count and agreeing (`venue_units=-1400 ledger_units=-1400 position_break=0`), every
  transport fault named in a field of its own (`late_fill_reports`, `duplicates`, `refused_by_adapter`, `rate_limited`,
  `dropped`, and the exit's `refused=1` / `grew=<units>`).
- **What was not there**: the edge to pay the fee. Gross capture -0.41, +0.92 and +1.07 bps of maker notional against
  3.36, 3.88 and 3.80 bps charged -- net -3.77, -2.95 and -2.73 bps -- and `[PNL]`'s `breakeven_maker_rate` of -4.09e-05,
  9.217e-05 and 1.07135e-04 says the same in the venue's language: the line needs a maker fee at or below about one basis
  point and the demo venue charges two. Recorded as the fifth closed market rather than optimised against, because a fee
  that is a multiple of the edge is the wall the other four hit.
- **The three remaining decisions were closed**: one attempt and stop after a refused or grown exit (policy, written
  where the growth is detected), refuse to start on an unresolved recovery (code, above), and record the fee boundary
  instead of tuning for a positive window (policy). Each is in `docs/RETROSPECTIVE.md` with the numbers behind it.

## A refused exit is not an empty book (2026-11-21)

- **`[WIND-DOWN]` printed `no_liquidity=1` for an order the venue refused.** The summary struct's own comment has always
  said `refused` covers "the venue or a pre-trade control", and only the control was ever set. Two runs read the wrong
  field: the 2026-11-17 40 bps exit, refused by the demo venue for pricing below its floor, and the 2026-11-21 break run,
  whose reduce-only sell was refused because the account was already short. Neither was a book showing no size, and a
  reader sent to the book would find depth there.
- **The refusal is watched for now.** The exit records the id its submission will take -- read *before* the call, because
  an in-process matcher and a stand-in venue both report inside it -- the report stream raises a flag when that id is
  rejected, and the wait ends on the flag rather than spending its budget. The summary then prints `refused=1`, and the
  position stays on the ledger where it is.
- **Pinned by** `pipeline_accounting_test`, whose `LateFillVenue` gained a `refuse_closes` mode: the account is short ten,
  the exit's reduce-only sell is rejected, and the wind-down reads `refused=1` with no `no_liquidity` and the position
  untouched. The first version of that test failed on the id being recorded after the synchronous report -- which is the
  bug the mode exists to catch.
- **A position that grew during the exit is now its own field.** `closed` is `|before| - |after|`, so a working quote that
  fills the other way during the wait reads as nothing having closed: measured on a 300 s session, `[WIND-DOWN]
  position=-1400 closed=0 residual=-11400 ... no_liquidity=1` where a resting sell had filled 10,000 units and the exit's
  own reduce-only order had not filled at all. The line now prints `grew=<units>` beside either verdict, because losing a
  race and finding an empty book are different facts.
- **Pinned by** `pipeline_accounting_test`: the refusing venue also fills a quote during the exit's wait, and the summary
  reads `refused=1`, `grew=10`, no `no_liquidity`, with the position carried at -20.

## The relay's transport is bounded, and reaches the venue through a proxy (2026-11-21)

- **`connect(2)` had no deadline.** A peer that blackholed a SYN held the caller for minutes: measured on this host, a
  balance query sat inside the connect for the whole seventy seconds it was watched, and the Python relay has always
  passed `timeout=15` to `socket.create_connection`, so the C++ port had dropped a bound the reference never lacked. The
  connect is now non-blocking with a ten-second budget for the whole address list (per attempt, one deadline), and the
  error names the budget when it is what ended the wait.
- **A query no longer holds the session lock across the network.** `answer_pending`, `answer_equity` and
  `answer_position` took `session_mutex_` and then called REST inside it, so a connect that hung stopped the venue's read
  loop: the session thread waited at `on_connected` and the engine read `frames_received=0` with no error anywhere but
  `last_error`. What those answers read is immutable -- the placer is built once and the translator is a const reference
  -- so they take no lock.
- **A TLS write on a reset socket ended the process.** `MSG_NOSIGNAL` covers the plaintext writes and `SSL_write` has no
  such flag, so a session died with `exit=141`; `SIGPIPE` is now ignored once at the relay's entry and the same write
  returns the error the caller already handles.
- **`--proxy=HOST:PORT` puts both legs and the placement route behind an HTTP `CONNECT` tunnel**, dialled instead of the
  venue while the venue's own name is what the tunnel asks for and what the certificate is checked against. Measured on a
  host whose resolver answers `www.okx.com` with a link-local address: the market leg carried the public stream, the order
  leg placed over REST, and the run closed flat with `[RECON] at=end position venue_units=0 ledger_units=0
  position_break=0 status=ok`.
- **Pinned by** a new `venue_ws_test`: a connect that cannot complete throws with its budget in the message and spends it
  (a listener whose accept queue is full is the blackhole), a refused port fails at once rather than by deadline, a
  successful connect leaves a blocking socket a read can use, the tunnel asks for the venue's own name and relays its
  bytes, a proxy that answers `403` is named with its status, and a write to a closed peer is an error rather than a
  signal. **The venue-side `break` was measured in the same session**, in the one shape whose two sides count the same
  thing (`ws` on `BTC-USDT-SWAP`): flat read `position_break=0 status=ok`, and one contract sold on the venue with a
  market order read `venue_units=-10000 ledger_units=0 position_break=-10000 status=break` beside
  `equity_break_usd=-9.35721 status=break`. The conversion is right: the venue's `pos` is the contract count (`-1`), and
  `-1 x 0.01 x 1e6` is the `-10000` the line printed. An IOC priced at a bid that moved is how a smaller fault looks --
  a hundredth of a contract fills and the break is `-100` units -- which is the row to read before the line.

## The position line states its unit, and refuses a comparison across two (2026-11-19)

- **A quantity without its unit is not comparable with another quantity.** The position line compared the venue's
  `POSITION <units>` with the ledger's position as if both counted the same thing, and nothing checked that they did:
  the relay converts the venue's contracts with `--contract-size` and `--quantity-scale`, and a relay started with a
  different scale, or pointed at a venue holding another coin, would have printed a `break` whose magnitude meant
  nothing -- or an `ok` by coincidence. The relay's answer now carries the conversion it used, `POSITION <units>
  <units_per_coin> <coin>`, and the engine compares both fields with its own instrument's. A disagreement prints no
  magnitude: `position_break=n/a venue_units_per_coin=… venue_coin=… ledger_units_per_coin=… ledger_coin=… status=unknown`.
- **The coin is compared, not the instrument's name,** because the two processes legitimately name it differently:
  `BTC-PERPETUAL` and `BTC-USDT-SWAP` are one quantity of BTC. The rule that splits a name into its coin lives in one
  place (`base_coin_of`), so the two sides cannot split one name differently, and a name with no separator is taken as
  the coin itself.
- **An answer that states only the number is `unknown`, not `ok`:** an older relay's quantity is one whose unit the
  engine does not know, and a check that could not be made must not read like one that passed.
- **Pinned by** `paper_broker_test` (agreeing, differing scale, differing coin, unstated), `live_venue_test` (the
  four-field answer, a zero scale and a missing coin dropped, `unavailable` clearing the statement) and
  `venue_order_session_test` (the relay's own answer, `POSITION -10000 1000000 BTC`). **Not measured against a venue:**
  the runs of 2026-11-19 were recorded before the field existed, so every line quoted above predates it and would read
  the same with it; a mismatched pair needs a relay lying about its conversion, which is a fault to inject rather than
  a number to measure. **No recorded run moved**, because the agreeing case prints the line it always did.

## A resumed session's positions meet the venue's (2026-11-19)

- **The position line now prints at both ends.** `[RECON] at=start position …` is the one a resumed session needs:
  `--snapshot-in` puts the account's positions back from a file and resolves the venue's pending orders, and the venue
  is the only one that can say whether the positions the file restored are the ones it still holds. The line is the same
  check as `at=end` -- `venue_units`, `ledger_units`, `position_break`, a zero-unit tolerance and an
  `ok|break|unknown` verdict -- printed before anything is quoted, so a resume that restores the wrong side of a
  position says so in its first second rather than after an hour of quoting against it.
- **Measured on 2026-11-19, 60 s, passive_l2:** `[RECON] at=start position venue_units=0 ledger_units=0
  position_break=0 tolerance_units=0 status=ok` and the same at the end, beside `[RECON] at=end venue_equity_usd=4917.96
  ledger_equity_usd=4917.96 equity_break_usd=9.74505e-05 status=ok`, with `[WIND-DOWN] position=10000 closed=10000
  residual=0 total_bps=5` and `[VENUE] duplicates=0`.
- **Measured on a resumed session, against the demo venue (2026-11-19).** `tools/live/resume_live.sh fresh|resume|break`
  is the three runs. The fresh one wrote a snapshot holding one working order; the resume printed `[RESUME]
  symbol=BTC-PERPETUAL … orders=1 live=0 gone=1 cancelled=0 cash_usd=4917.96 legs=0 next_order_id=2` and `[RECON]
  at=start position venue_units=0 ledger_units=0 position_break=0 tolerance_units=0 status=ok` -- the line that was
  unreachable before, because the snapshot flags are refused on `ws` and the reachable shape is `l2_replay --live-orders
  --snapshot-in`. The third run resumes from a copy of that snapshot with a fabricated 10,000-unit position: `[RECON]
  at=start position venue_units=0 ledger_units=10000 position_break=-10000 tolerance_units=0 status=break` beside
  `venue_equity_usd=4917.96 ledger_equity_usd=104918 equity_break_usd=-100000 status=break` -- the `break` verdict on a
  venue, produced by the file rather than by the venue. One caveat, on the live page: the line compares units, which
  `ws` makes comparable (the market symbol is the venue's instrument) and `l2_replay --live-orders` does not.
- **What it costs, and what it cannot reach:** one position query at the start of every live run, including a fresh one
  and a resumed one. The two-book path has no position line and cannot have one: `--live-orders` is refused on a run with
  two books, so there is no venue to ask there.


- **Equity cannot see a quantity.** A session that ended `[WIND-DOWN] residual=-900` had the venue's probe reading
  `pos=0`: the exit had filled and its report arrived after the close's one-second wait, and the account was worth the
  same either way -- the ledger held the position and had paid for it -- so `[RECON] at=end` read `ok`. The adapter now
  asks the venue for its position (`GET /api/v5/account/positions`, signed on the balance's own surface, converted to
  engine units by the translator's contract size and signed, because a short is a negative quantity) and the run prints
  `[RECON] position venue_units=… ledger_units=… position_break=… tolerance_units=0 status=ok|break|unknown`. The
  tolerance is zero units: a quantity is exact, and one contract that came back rounded is the whole of the break.
- **Measured on the demo, 2026-11-18, 60 s, passive_l2:** `[RECON] position venue_units=0 ledger_units=0 position_break=0
  tolerance_units=0 status=ok` beside `[RECON] at=end venue_equity_usd=4921.88 ledger_equity_usd=4921.88
  equity_break_usd=0.00472691 status=ok`. The run ended flat (`working_orders=0`), so this is the `ok` case; the `break`
  case was pinned by `paper_broker_test` -- a venue at 0 against a ledger at -900 -- because a session that closes
  cleanly does not produce it; the entry above produces it on purpose, from the snapshot rather than from the venue.
- **One defect found on the way, and it was observed:** the application's query pump forwarded only `PENDING` and
  `EQUITY` answers and left everything else queued for the pipeline's drain, so a `POSITION` answer sat in the queue until
  the query timed out -- the first run that asked printed `[RECON] position venue_units=n/a ledger_units=0
  position_break=n/a tolerance_units=0 status=unknown`. The pump forwards the third answer now, and the same run reads
  `0/0 ok`.


- **The relay reported an accepted cancel twice.** The REST reply's `CANCELED <id> user` and the venue's `state: canceled`
  on the private `orders` channel are the same ending, so every successful cancellation reached the adapter as two and
  the second was dropped as a duplicate -- 31 of a 60 s session's 37 cancels on 2026-11-18, a quarter of its receive
  traffic spent on repeats. A cancel's success is the request being accepted, not the order ending: both relays answer it
  with nothing, the sandbox venue's cancel reply is the channel's state change alone, and a *refused* cancel still ends
  the order, because the channel has nothing to say about an order the venue refused to cancel.
- **Measured on the demo, 2026-11-18, 60 s, passive_l2, cap 200 rate 2:** `[VENUE] frames_sent=135 frames_received=172
  orders=73 cancels=60 refused_by_adapter=36 reports=183 dropped=0 duplicates=0`, `[EXECUTION] audited_orders=109
  audited_filled_orders=13 audited_cancelled_orders=55 audited_rejected_orders=41 late_fill_reports=14
  late_fill_quantity=50000 strategy_net_position=0 strategy_position_mismatches=0`, `[WIND-DOWN] position=10500
  closed=10500 residual=0 notional_usd=665.05 total_bps=5.00003`, the venue's probe `pos=0` and `[RECON] at=end
  equity_break_usd=0.00282702 status=ok`. The run before it read `duplicates=1`; the session before the change, 31.
- **What the same runs found, and it stays open:** one of them ended with `[WIND-DOWN] position=200 closed=0
  residual=-900 no_liquidity=1` and `working_orders=1` while the venue's probe read `pos=0` -- the exit had filled and its
  report arrived after the close's one-second wait -- and `[RECON] at=end` read `status=ok` because it compares equity,
  not quantity. A positions query on the balance's REST surface, reported as a quantity break beside the equity break, is
  the next measurement.
- **Pinned by `venue_order_session_test`:** a `CANCEL` leaves the REST route silent and the line reaches the engine only
  when the channel reports the cancellation. `relay_order_channel_test` still reads four lines for the sandbox session,
  which never produced a duplicate of its own.


- **The adapter never ended an order that filled.** The venue reports `state: filled` on its `orders` channel and
  neither relay maps that state to a line, so the only thing that ever closed a filled order was a cancel for it being
  refused (`sCode 51400` -> `REJECT`): every live run read `audited_filled_orders=0` and its filled orders were counted
  as cancelled or rejected. The adapter knows the size it sent, so the fills reaching it is the fact -- the last fill is
  forwarded, then a `Filled`, and a later answer for that order is a `duplicates`. A fill that arrives after the end is
  still forwarded, and a fill the venue rounds past the size sent is clamped.
- **Measured on the demo, 2026-11-18, 60 s, passive_l2, `--venue-max-orders=200 --venue-orders-per-second=2`:**
  `[EXECUTION] audited_orders=57 audited_filled_orders=10 audited_cancelled_orders=31 cancelled_after_fill_orders=0
  orders_that_filled=10 filled_orders_by_size=0/0,0/0,0/0,0/0,0/0,10/102700 late_fill_reports=11
  late_fill_quantity=62700 strategy_net_position=0 strategy_position_mismatches=0`, `[VENUE] orders=47 cancels=37
  refused_by_adapter=10 duplicates=31 rate_limited=10`, `[WIND-DOWN] position=12700 closed=12700 residual=0
  total_bps=5.01447`, the venue's probe `pos=0` and `[RECON] at=end equity_break_usd=0.00361225 status=ok`. Ten
  completed orders carried 102,700 units and the eleven late fills the other 62,700 of `trade_report_quantity=165400`.
- **Pinned by `live_venue_test`:** two fills of four and six on a ten-unit order give a `Trade`, a `Trade` and a
  `Filled` whose quantity is the remaining none; a later cancel answer is a duplicate; a fill rounded past the size sent
  is clamped; a fill that does not complete the order leaves it open for its cancel. Reverting the ending for one
  rebuild failed the test at the first completed order.


- **The cap was the wrong shape once refusals reached the strategy.** `--venue-max-orders` counts the orders a whole
  run sends; a maker that replaces its quotes spends that budget in seconds and is refused for the rest of the session
  (383 quotes, 362 refusals, and an exposure that was whatever the twenty placed orders filled).
  `--venue-orders-per-second=N` bounds how fast the session places, on a one-second window read from the adapter's own
  clock, exempting a close as the session cap does and counting its refusals apart (`rate_limited`), because an
  exhausted session says the run is over while a rate refusal says the next window is. It is a scope decision: the cap
  is unchanged, and a run that does not set the rate keeps every recorded number.
- **Measured on the demo, 2026-11-18, 60 s, passive_l2, back to back:** with the cap alone, `[VENUE] orders=21
  cancels=20 refused_by_adapter=186 reports=61 dropped=0 duplicates=11 orders_per_second=0 rate_limited=0` with
  `[EXECUTION] submitted_orders=207 trade_reports=20`; with `--venue-max-orders=200 --venue-orders-per-second=2`,
  `[VENUE] orders=48 cancels=47 refused_by_adapter=19 reports=118 dropped=0 duplicates=34 rate_limited=19` with
  `[EXECUTION] submitted_orders=67 trade_reports=23 orders_that_filled=7 late_fill_reports=13 late_fill_quantity=70600`.
  Both end with `strategy_net_position=0`, the venue's probe `pos=0` and `[RECON] at=end status=ok`; the second closes
  7,600 units to `residual=0` at `total_bps=5.67122`.


- **A refusal before the wire was silent.** `LiveVenue` counted its own caps in `refused_by_adapter` and emitted no
  report, and it did not override `rejected_orders`, so the pipeline could not tell a refused order from a resting one.
  In the 60 s session below, 37 of 58 orders were refused at `--venue-max-orders=20`, the strategy went on quoting
  against all of them, and the venue was sent a cancel for every one -- 57 cancels for 21 placements. `rejected_orders`
  is that counter now, so a refusal becomes a terminal rejection: the audit closes, the strategy drops the order and the
  position gate's reservation goes back.
- **One ending, two reports.** The relay answers a cancel on its REST path and the private `orders` channel reports the
  same cancellation, so one order ended twice; the ledger counted the second as `orphan_order_reports` and the pending
  quantity was released twice. `LiveVenue` drops a second terminal report for an order it has already ended and counts it
  as `duplicates` in `[VENUE]`; a fill that arrives after the end is still forwarded, because that is the order's
  quantity rather than another ending.
- **Measured on the demo, 2026-11-18, 60 s, passive_l2, cap 20:** before, `[VENUE] frames_sent=80 frames_received=102
  orders=21 cancels=57 refused_by_adapter=37 reports=100 dropped=0` with `[EXECUTION] audited_rejected_orders=44
  orphan_order_reports=13`; after, `[VENUE] frames_sent=43 frames_received=70 orders=21 cancels=20
  refused_by_adapter=362 reports=54 dropped=0 duplicates=14` with `audited_rejected_orders=368` and no orphan field,
  `late_fill_reports=8 late_fill_quantity=40400`, `[WIND-DOWN] position=-400 closed=400 residual=0 notional_usd=25.606
  total_bps=4.99964`, the venue's probe `pos=0` and `[RECON] at=end equity_break_usd=-0.00457172 status=ok`. Every cancel
  the second run sent was for an order the venue held.
- **What the numbers also say:** with the refusals visible the strategy re-quotes after each one instead of believing it
  is working, so 383 quotes met the 20-order session cap and 362 were refused. The cap bounds a session rather than a
  rate; whether it should be a rate is the next scope question, and it is recorded rather than changed here.


- **`orphan_order_reports` was carrying two different things.** A live venue answers a cancel for an order it has
  already filled with `sCode 51400` and sends the fill afterwards, so the ledger's per-order audit had already closed
  when the fill arrived. The fill was booked by the account and attributed to the strategy (`e4c6a1a`), but the ledger
  counted it beside a report it could not attribute at all. It keeps the ids it placed for the session now and splits
  the two: `late_fill_reports`/`late_fill_quantity` for a fill of one of its own orders, and `orphan_order_reports` for
  a duplicate terminal report or a fill of an order this process never placed (a resumed session). Both are printed in
  `[EXECUTION]` only when non-zero.
- **Measured by the same case:** `pipeline_accounting_test`'s venue answers a sell quote's cancel with the refusal and
  then the fill; the run reads `late_fill_reports=1 late_fill_quantity=10` with `orphan_order_reports=0`, where it read
  `orphan_order_reports=1` before, and `order_state_test` pins a duplicate terminal report and a foreign fill as the two
  things that stay in the orphan counter. Reverting the split for one rebuild failed both cases at the late-fill count.
- **What a late fill does not re-open:** the order's queue profile, its size bucket and its minimum-commission floor are
  settled at the report that ends the order, and the fill's own fee arrived on the report and was taken by the account.
  A run therefore reports the late fill's count and quantity, not a second measurement of the order.


- **A fill that arrives after the report that ended its order still moves the strategy's position.** The venue
  answers a cancel for an order it has already filled with `sCode 51400`, which the relay reports as a `Rejected`,
  and the fill that made the order unfillable follows it. The strategy forgot the order's id at that report, so it
  booked the cancel and dropped the fill while the account booked both: the demo's close read
  `strategy_net_position=20000` against a flat account with `strategy_position_mismatches=10`. `SubmittedOrders`
  keeps the ids a session submitted, so a fill is attributed by the id it was placed under instead of by whether the
  order is still working. A fill for an id the process never submitted is still ignored -- that is what a resumed
  session holds, and it is the case the mismatch counter exists for.
- **Pinned, not re-run on the venue:** `l2_strategy_test` books a fill that follows a `Rejected` on both L2 strategies
  and still ignores a foreign id; `pipeline_accounting_test` drives a venue that answers a sell quote's cancel with
  the refusal and then the fill, and the run ends `closed=10 residual=0` with `strategy_net_position=0` and
  `strategy_position_mismatches=0` against a flat account. The account, the exposure and the reconciliation were
  already the closed ones; the live re-run that reads the metric is the open measurement.
- **Still open, and measured by the same case:** the ledger's per-order audit closes at the terminal report, so the
  late fill is booked in the account and attributed to the strategy but not in the order's queue profile or its
  minimum-commission bucket, and `[EXECUTION]` counts it in `orphan_order_reports` (1 in the new case). Keeping the
  audit open through a late fill is the next change.


- **An order's entry survives every ending report.** The venue's cancel reply for an order it already filled is
  `sCode 51400`, which the relay answers `REJECT`; the fill that made the order unfillable arrives after it. Erasing
  the adapter's per-order entry on a cancel or a reject dropped those fills (`[VENUE] dropped=16` in one session) and
  left the engine four contracts from the venue. The entry is kept for the session, a placement's reject included, and
  a fill without one is still dropped and counted rather than guessed.
- **Measured:** 2026-11-17, 60 s, passive_l2, the reduce-only market close: `[WIND-DOWN] position=-10000 closed=10000
  residual=0 notional_usd=647.473 spread_bps=0.101008 fees_usd=0.32374 fees_bps=5.00005 total_bps=5.10106`, the venue's
  probe `position: flat`, `[VENUE] orders=4 cancels=3 refused_by_adapter=0 dropped=0`, and `[RECON] at=end
  venue_equity_usd=4956.58 ledger_equity_usd=4956.58 equity_break_usd=-8.49732e-08 tolerance_usd=0.495658 status=ok`.
  The exit cost 0.10 bps against the mark plus 5.00 bps of taker fee, and the run ends flat on both sides.
- **The strategy's inventory still counts the close's fills** (`strategy_net_position=20000` against the flat account,
  `strategy_position_mismatches=10`): the close is the pipeline's order, and the strategy's `on_order_update` takes
  every report. The account, the exposure and the reconciliation are the closed ones; the strategy metric is next.

## The exit is a reduce-only market close, a close is exempt from the order cap, and a fill keeps its side (2026-11-17)

- **A fill that arrives after its cancel is booked in the order's side.** `LiveVenue` erased the order's entry when
  the venue said `CANCELED`, but the demo's venue fills what it can at the moment it cancels the remainder, so that
  report arrived with no entry and took the report's defaults. `live_venue_test` pins the kept entry (a sell's fill
  after a cancel is a sell), and a fill for an order the adapter never sent is now dropped and counted rather than
  guessed.
- **The live exit is a reduce-only market order.** `CLOSE <id> <symbol> <side> <qty>` becomes
  `{"ordType":"market","reduceOnly":true,"sz":"<ceil contracts>"}`; `contracts_to_close` rounds up, so a fractional
  position is closed by one contract and the venue caps it at the position, and the acknowledgement stays the units the
  engine asked to close. `[WIND-DOWN]`'s `spread_bps` for the market exit is the cash the fill moved against the
  position's notional. The replay's close is still the limit IOC.
- **The adapter's order-count cap exempts a reduce-only close.** The passive session placed 65 orders against
  `--venue-max-orders=20` and the cap refused the exit before the wire; a close reduces the position, so it is the one
  order the cap must not stop. The size caps still apply.
- **Measured on the demo (2026-11-17, 60 s):** the close's line read `[WIND-DOWN] position=10000 closed=0
  residual=10000 notional_usd=639.119 spread_bps=40 fees_usd=0 fees_bps=0 total_bps=40 no_liquidity=1`, and the
  venue's probe afterwards read `pos: "-1"` against the engine's `+10000` units; `[VENUE] orders=20` and
  `submitted_orders=39` say the close did not reach the venue. The next instrument is `--order-dump`: the ORDER and
  FILL frames' sides are the only place that can separate a wrong attribution from a venue that filled the other side.

## A live close pulls the working quotes, and a session settles the venue's late fills (2026-11-17)

- **`--close-out` cancels the strategy's working orders before it sends the exit.** The demo's run ended short one
  contract because a quote the engine still counted filled at the venue while the exit was working, and its report did
  not reach the ledger: the venue's probe read `pos: -1` against the engine's `+2000` units. `send_close_order` now
  cancels every order in the ledger, drains the cancels (and any fill they raced) for the same bounded second, re-reads
  the position and sizes the exit against what is left. It is live-only -- the pump is the signal -- so a replay's close
  keeps its synchronous read and no recorded number moved. `pipeline_accounting_test` pins it: a resting quote is
  cancelled before the exit, the exit still fills the carried position, and the summary's working set is empty.
- **A live session drains the order leg after the feed stops.** A quote cancelled at the last moment can fill at the
  venue a few milliseconds later, and the engine used to stop reading when its market clock did: the demo's session
  ended with the engine believing it was flat and the venue short `0.44` contracts, so the close never ran. The app now
  drains the order leg until the venue is quiet -- bounded at one second, ended early by 100 ms of no frames -- before
  the summary and the close, and both then price the position the venue actually holds.
- **Measured on the demo (2026-11-17, 60 s, 20 bps):** `[WIND-DOWN] position=10000 closed=0 residual=10000
  notional_usd=650.005 spread_bps=20 fees_usd=0 fees_bps=0 total_bps=20 no_liquidity=1`, and the venue's probe after the
  run read `pos: "1"` -- the engine's 10,000 units are one contract, so the two sides agree; the earlier run's `pos: -1`
  against `+2000` is gone. The close itself did not fill: the venue's bid was below the 20 bps sell. The reduce-only
  market exit -- which crosses whatever the venue shows and cannot flip the position -- is the next step.

## A live close waits for the venue's fill, and a lost balance query retries (2026-11-17)

- **The close's fill is a report, and the pipeline was reading the position before it arrived.** `--close-out` on the
  demo at 20 bps placed the order and printed `[WIND-DOWN] position=10000 closed=0 residual=10000 spread_bps=20
  fees_bps=0 total_bps=20 no_liquidity=1`; the position probe run straight after the run found the account `before:
  flat`, and the close was the only sell the run sent. The venue had filled the exit; the engine's line was stale,
  because the fill report arrives after the placement is acknowledged and only the run's own drain books it.
  `Pipeline::set_venue_pump` is that drain, and `send_close_order` now gives it a bounded wait -- 1000 ms by default,
  `set_close_out_wait_ms` for a test -- before it reads `closed`/`residual`. The wait is live-only: an in-process fill
  is on the book when the call returns and the pump is skipped, so `pipeline_accounting_test` pins both directions (a
  synchronous close pumps zero times; a book with no size pumps, returns its residual and `no_liquidity=1`), and no
  recorded number moved. The fixed run's line is `[WIND-DOWN] position=10000 closed=8000 residual=2000
  notional_usd=643.442 spread_bps=20 fees_usd=0.257375 fees_bps=4.99995 total_bps=25`: the five taker fills are booked,
  and the exit is priced.
- **The same run leaves a position the engine does not see.** The venue's probe read `pos: -1` where the engine's book
  held `+2000` units, and `close-position` flattened it. One working order the engine still counted was not in the
  venue's pending list, so a fill's report did not arrive before the run stopped draining; the exit's booked 8,000 units
  are also five parts of its contract. The next step is a close that cancels the strategy's working quotes before it
  sends the exit and settles until the venue's position is flat, because the equity line is cash against a marked
  remainder and can read `ok` while the positions disagree.
- **The second `EQUITY` answer was lost, so the query retries inside its budget.** The demo7 run sent 17 frames (9
  orders, 6 cancels, 2 `EQUITY`) and received 23 (22 order reports and one answer): `[RECON] at=start ...
  equity_break_usd=4.28026e-07 status=ok`, `at=end venue_equity_usd=n/a status=unknown`. `LiveVenue::refresh_equity`
  now splits `query_timeout_ms` into two attempts, which covers a lost request or answer; a venue that answers
  `unavailable` is not retried. The fixed run sent 20 frames and received 34, with both ends `status=ok` (`at=end
  equity_break_usd=0.0802475 tolerance_usd=0.497662`), so the retry is pinned by a transport that ignores the first send
  and answers the second, not by the venue.

## The close-out has a crossing band, and the demo's price floor bounds it (2026-11-17)

- **`--close-out-cross-bps=N` puts the closing IOC N basis points through the midpoint.** 0, the default, is the touch
  every recorded number uses; the flag is the live path's, where the venue's book is not the market leg's last snapshot
  and a touch from a second ago can close nothing. The pipeline prices the order at `mid * (1 -/+ N/10000)` and reports
  that distance as `spread_bps`, so the line says the ceiling the run accepted rather than the price the venue filled.
  `pipeline_accounting_test` pins a 50 bps close: the in-process engine fills at its own bid and the summary reads 50.
- **The demo's 40 bps close was refused, and the reason is the venue's floor, not an empty book.** On 2026-11-17 the
  market leg's last mid was 64,250.9 and the venue's mark 64,368.86; the venue's 0.5% floor was 64,047, and a 40 bps
  sell was 63,993 -- 54 under it -- so the venue refused the order. `[WIND-DOWN] position=10000 closed=0 residual=10000
  notional_usd=642.509 spread_bps=40 fees_usd=0 fees_bps=0 total_bps=40 no_liquidity=1` is that miss. 20 bps puts the
  same sell at 64,122, inside the floor and below the venue's 64,36x bid; the run that proves the fill is next, and the
  account was flattened with `close-position` after this one.
- **The same run's `[RECON] at=start` and `at=end` read `venue_equity_usd=n/a status=unknown`.** The relay's `EQUITY`
  answer did not land in that run (the balance itself read 4,979.99 through the Python probe before it), so the two
  comparisons are open alongside the close. No replay or gate number moved: the flag's default is the touch.

## The venue equity is the USDT balance, not the account's total (2026-11-17)

- **`RestPlacer::balance_usd` now reads `details[ccy=USDT].eq`** (falling back to `cashBal`), not `totalEq`. A
  USD-margined strategy's ledger holds one currency; the demo account's 1 BTC and 100 OKB spot are about 73,000 of its
  80,000 `totalEq`, and their prices move a number the strategy never traded. Measured 2026-11-17, flat: `totalEq`
  80,031.54 against a USDT equity of 4,981.77. The stand-in's answer now carries `totalEq` 80,086.85 and USDT 4,986.73,
  so reading the wrong field fails the test rather than passing silently.
- **The earlier 80k figures were that `totalEq`.** Seeding `--starting-cash` with 80k measured the demo's spot holdings
  as if they were the strategy's cash: the -75.65 and -22.15 breaks and the +4.12 `ok` were totalEq movements, not the
  ledger's accounting. On the USDT basis a flat account opened `--starting-cash=4981.77` at `at=start
  venue_equity_usd=4981.77 ledger_equity_usd=4981.77 equity_break_usd=0.004472 tolerance_usd=0.498177 status=ok`.
- **The single controlled run's end is a position, not an accounting break.** It held one contract, `[WIND-DOWN]`
  closed none (`no_liquidity=1`) on the demo's thin book, and `at=end` measured `venue_equity_usd=4980.19
  ledger_equity_usd=4981.52 equity_break_usd=-1.32221 status=break` -- the venue's mark against the run's last
  midpoint. The account was flattened afterwards with the venue's `close-position`. A venue-side close at the end is the
  next iteration.
- No replay or gate number moved; the USDT basis is live-only, and `venue_order_session_test` pins it.


## The live recovery is measured, and a placement's ACK no longer says zero (2026-11-17)

- **The recovery pair on the demo venue.** A replay with `--live-orders` wrote `[SNAPSHOT] orders=1`; the next run with
  `--snapshot-in` printed `[RESUME] orders=1 live=0 gone=1 cancelled=0` -- the venue had already let the order go, so the
  recovery counted it instead of assuming a fill or a cancel. A controlled order placed through the relay's line
  protocol and still working when the snapshot was taken produced `[RESUME] orders=1 live=1 gone=0 cancelled=1` with
  `[VENUE] cancels=1`: the pending query, the cancel, the settle and the second query all ran against the venue.
- **The placement reply carries no `sz`, and the ACK was reading it.** `RestPlacer::lines_for` built `ACK <id> <units>`
  from the reply's `sz`; OKX's `POST /trade/order` answer is `ordId`/`clOrdId`/`sCode`/`sMsg` with no size, so every
  live placement was acknowledged `ACK <id> 0`. The translator's `Request` now carries the contracts it sent, and the
  ACK falls back to it; the live line read `ACK 907654 10000` after the fix, and the stand-in's no-`sz` case pins it.
  The earlier live numbers were wire acceptances and are not re-quoted as strategy results: the zero acknowledgement
  could have changed the strategy's pacing.
- **The two queries work outside a session too:** a line client on the relay's order leg printed `PENDING 0` and
  `EQUITY 80089.191`, and the relay summed `pending_queries=1 equity_queries=1 rest_429=0 query_failures=0`.
- No replay or gate number moved; the fix is on the live placement route, and the gate's `venue_order_session_test`
  pins it.


## A live account's opening balance is a flag, checked before the first quote (2026-11-17)

- **`--starting-cash=USD`.** The opening cash -- and so the exposure ceiling and `[EXECUTION] starting_cash_usd` -- was
  hard-coded at 1000, which is the documented replay account and a wrong base for a funded venue. The flag sets it; the
  default keeps every replay unchanged, and `--snapshot-in` still wins for a resumed run.
- **The venue-equity `[RECON]` line prints before the first quote and after the last one**, as `at=start` and `at=end`.
  The first live run through the route measured `venue_equity_usd=80076.9 ledger_equity_usd=999.816
  equity_break_usd=79077.1 status=break`, which is the gap the flag closes.
- **The tolerance moved from ten basis points of the account to one, and the demo run is why.** With
  `--starting-cash=80076.7` the next run found the venue at 80,001: `equity_break_usd=-75.6537`; at ten basis points the
  printed tolerance was 80.0767 and the line read `ok`, while the ledger and the account were 64 to 76 USD apart. At one
  basis point (8.00124 for that balance) the same gap reads `break`. A third run on the final build, seeded at the
  previous end 80,012.4, measured `at=start venue_equity_usd=79990.3 ledger_equity_usd=80012.4 equity_break_usd=-22.1486
  tolerance_usd=8.00124 status=break` and `at=end venue_equity_usd=79995.2 ledger_equity_usd=80011.8
  equity_break_usd=-16.6734`, with the strategy's own broker reconciliation `status=ok` over three fills. The demo
  account held a position this ledger did not open, so the account-wide check is a break while the strategy's fills
  reconcile -- the precondition for a clean start is a flat account or a `--snapshot-in` that adopts the position, not a
  wider tolerance.
- **The route itself was exercised.** `--venue-demo` delivered 3,713 market frames (657 trades, 3,054 depth updates) in
  the first 60-second run, and the order leg placed 20 orders and read 60 reports; the second run's two quotes were
  cancelled on a thin book. The first run's `[PORTFOLIO]` was 999.816 with `[PNL] net_pnl_usd=-0.183602` over two fills;
  its venue-equity break is the base, not the strategy.
- **No recorded replay number moved:** the flag's default is 1000, the recorded runs omit it, and the equity lines print
  only on a live leg.


## A long demo session can now check its account, ask about its orders, and stay inside a quota (2026-11-17)

- **The venue's balance is compared with the ledger, not with the broker's statement.** A live run now prints a second
  `[RECON]` line after the final mark: `venue_equity_usd` from `GET /api/v5/account/balance`, `ledger_equity_usd` from
  the account's own mark, the signed `equity_break_usd` and the `tolerance_usd` the verdict uses (the larger of 0.05 USD
  and ten basis points of the account, because the two sides mark a position at different prices). The status
  is `ok`, `break`, or `unknown` when either side is `n/a` -- a check that could not be made does not read like one that
  passed. The first `[RECON]` line is unchanged, so every recorded brokered line keeps its fields.
- **`--snapshot-in` now asks a live venue which of its orders survived.** `RestPlacer::pending_orders` GETs
  `/api/v5/trade/orders-pending?instType=SWAP`, with the query string in the signed path, and `LiveVenue::query_order`
  answers from that list. The list is refreshed before the recovery asks and again after each cancel
  (`IMatchingEngine::settle_queries`, a no-op on the in-process engine), so the second query is a question about the
  venue now rather than about the state before the cancel. The engine refuses to resume if the first query goes
  unanswered, naming the relay's `--order-rest`, because "could not ask" must not be read as "nothing is working".
  The simulator's rule is unchanged; it is the live path that used to answer `Unknown` for everything. The decorators
  forward the query, including `RecordingEngine`'s, so `--record-orders` no longer hides a live venue's work.
- **The REST route has a budget.** `RestPlacer` keeps a token bucket per endpoint (ten calls a second by default), and
  an HTTP 429 is retried twice with a longer wait before it becomes a `REJECT`. `rest_429` is counted beside
  `venue_refusals`, because a rate limit is this client's budget and a refusal is the venue's judgment of the order.
  `WireConfig::rest_limits` is what a test sets to prove the wait rather than assume it.
- **`venue_relay --venue-demo`** points the market leg at `wss://wspap.okx.com:8443/ws/v5/public`, the public stream
  that belongs to the demo account's instrument ids. Giving both `--venue` and `--venue-demo` is refused rather than
  resolved by argument order.
- **The evidence is loopback and unit tests, not a venue run.** `tests/venue_order_session_test` drives a stand-in that
  recomputes the signature over the method and the query string (`PENDING 2 11 12`, `EQUITY 998.5`, two 429s then
  `ACK 24`), and pins `rest_calls=11 rest_429=2 pending_queries=1 equity_queries=1` in the summary; a second connection
  with a one-per-hundred-milliseconds bucket measures the wait. `tests/live_venue_test` pins the four new frames and the
  stale-set rules; `tests/paper_broker_test` pins the three equity outcomes. No demo session was run for this change, so
  no live number moved, and the demo's fee schedule and thin book still make its `fees_paid` a statement about the demo.
- Three environment facts are documented on the live page rather than hidden: the demo fee schedule is not the live one,
  `x-simulated-trading: 1` stays hard-coded on purpose, and a thin demo book produces more `51400` cancels.


## The forced-flow family is probed, and both sides sit inside the fee (2026-11-16)

- **`tools/study/cascade_probe.py`.** The one family never measured here was the trade people describe: after a forced
  sell, buy the vacuum. The rule is registered in the tool rather than in a paragraph -- a one-second bucket at or above
  the day's own 99th percentile of net aggressive volume is a burst, entry at the first trade after it, exits at 1, 5, 30
  and 60 seconds, 5 bps a side, a 30 s cooldown -- and the same events run in both directions, because a negative gross
  for the fade is a positive gross for the continuation.
- **Both directions are closed.** On 2020-04-01 the continuation pays **+6.09 bps** gross at 30 s on BTC and **+6.12** at
  5 s on ETH against a 10 bps taker round trip, so the best net of any horizon, instrument or direction is **-3.88**; the
  fade is worse than nothing at -1.46 to -2.56 gross, because the burst does not come back.
- **The depth-conditioned version is run too, and it weakens the family rather than rescuing it.** `--condition depth`
  rebuilds the top of book from the 1.5 GB incremental file and calls a second a vacuum when its two top sizes are at or
  below 0.2 of their trailing 300 s median. The two conditions barely overlap (26 of 723 vacuum seconds are also P99
  bursts on BTC, 2 of 788 on ETH), and on the vacuum population the continuation is **smaller**, not larger: +3.40 against
  +6.09 bps on BTC and +2.54 against +6.12 on ETH, with the best net of any horizon at -6.60. The pass is a command
  rather than a marked block, for the reason the walk-forward is: it reads 1.5 GB, takes about a minute, and `fast` is the
  check every edit pays for.
- **The limits are written beside the record**: one day, two instruments, 26 to 29 ETH fills (below the thirty that
  counts), and trades only -- the 1.5 GB book is there for the depth-conditioned version, which is a separate run.
- **Two things the same pass settled.** The R_max table over `data/v2/options/*.csv` reproduces the five series' recorded
  numbers exactly -- 56, 46, 41, 24 and 9 against the reference of 71 -- so the options line closes on the same quotient
  twice rather than once on prose and once on a memory. And the same screen over `data/v2/screen/*.csv` puts 33 of 60
  equities above the 71 reference at a 100-share clip, which reopens nothing: that table is an upper bound that charges the
  median spread at every fill, while the equity lines were closed on realised capture after the queue and adverse
  selection.
- The record is in `docs/archive/studies.md` with the marked block that re-derives it, and `docs/TOOLS.md` lists the tool.

## The equity studies get a page of their own (2026-11-16)

- **`docs/performance/history-04-the-fee-is-not-the-wall.md`.** The eight equity-window runs in part 3 were the whole of
  the four minutes a `docs` run spent on that page -- about thirty-five seconds each -- so a prose edit there paid for
  windows it did not touch. The 723 lines that quote them moved unchanged: the two windows, the latency sweep, the screen,
  the markout, the account's fee schedule and the MU sweeps. Part 3 keeps a `## Where that last step is recorded` pointer,
  so the narrative still runs from "what this leaves" into the queue-model section.
- **Measured after the move**: part 3 has 14 check blocks and part 4 has 5, `docs
  docs/performance/history-03-studies.md` costs 65 s and `docs docs/performance/history-04-...` 44 s, and the whole set
  107 s -- so a prose edit to part 3 is 65 s instead of 107 s. `fast` is unchanged at about 1 min 50 s, because it runs
  every page: the saving is per-page, which is the number ADR 0003's "to revisit" asked for before the split was written.
- **What moved with the text**: six anchors in `docs/archive/studies.md` and `docs/using/06-reading-the-output.md`, four
  `path:line` citations in `tests/fixtures/verdicts.json` (now part 4's `:21`, `:37`, `:40` and `:58`; the crypto case
  stays at part 3's `:160`), a nav row, the part-4 row of `docs/PERFORMANCE_HISTORY.md`, and the "of 3" in parts 1 and 2.
- **The move was a script over line ranges, not a transcription**, because a block lost in a hand move is silent. What
  proves nothing was lost is `check_docs` re-running all five moved blocks; `check_links.py` proves every anchor in both
  directions, and `mkdocs build --strict` proves the new page is in the site's tree.
- **A correction of the previous two entries, and of this one's first version.** The 4 min 22 s and the "four minutes
  of document half" those entries quote were measured beside another job that was compiling the sanitizer tree, and the
  same numbers taken alone are **1 min 47 s** for `fast` and 107 s for the whole `docs` set. The per-run cost of an
  equity window is about five seconds on this host, not the thirty-five the older header carried. Both figures are in
  `tools/gate/verify_l2.sh`'s header, `docs/DESIGN_REVIEW.md`, `docs/PERFORMANCE.md` and `.clinerules` now, taken with
  nothing else running.
- The first attempt ran the same move twice, because the terminal swallowed the commands and `/tmp` lost the script, and
  the second run truncated part 3 to 522 lines. `git checkout` restored it, and the script now refuses unless the source
  is 1,890 lines, line 523 is the expected heading, and the destination does not exist.

## The gate's costs are measured, not remembered (2026-11-16)

- **One set of numbers, in the four places that state a cost.** Measured on eight cores on 2026-11-16: tier 0 is 3 s,
  `code` 26 s, `full` 26 s, `asan` 6 s of tests plus 4 s of replay on a built tree (632 s when the sanitizer tree is
  cold), and `fast` 4 min 22 s end to end, of which the document half is about four minutes. `tools/gate/verify_l2.sh`'s
  header, the tier table in `docs/DESIGN_REVIEW.md`, section 3.7 of `docs/PERFORMANCE.md` and the table in
  `.clinerules` section 3 now say the same thing, because a cost that lives in four places and is remembered in each is a
  cost that is wrong in three of them.
- **Two of the old numbers were wrong by two orders of magnitude.** `docs/PERFORMANCE.md` said a `fast` costs `~3.6 s`
  and `bash verify_l2.sh all` `~18 s`, and it gave `all` a row of its own when `all` is the sum of three tiers rather
  than one. `docs/DESIGN_REVIEW.md` said tier 1 was `~60 s`. ADR 0003 records this exact defect being fixed once ("the
  previous one had been wrong by an order of magnitude for months and nothing was checking it"); it had survived in a
  second and a third file, which is why the numbers now say where they were taken.
- **A correction of the previous entry.** That entry said a cold sanitizer run costs "the compiler and the 200k replay".
  Measured apart, the replay is 4 s: the compiler is all of it, and the incremental tier is ~10 s.
- No C++, no recorded number and no size count moved.

## The sanitizer tier builds in parallel (2026-11-15)

- **One flag, and the tier's cold cost is now measured.** `tier3()` called `cmake --build "$asan_build_dir"` with no
  `-j`, so a cold sanitizer tree compiled one translation unit at a time: the same tier took roughly fifteen minutes on
  this host, while the review's table quotes the *incremental* cost and so never showed it. The call now passes
  `-j"$(nproc)"`, a cold `bash tools/gate/verify_l2.sh asan` on eight cores measured **632 s end to end** with
  `tier3 OK`, and the 38 test binaries inside it finished in under a minute -- so the remaining cost is the compiler and
  the 200k replay, not the check.
- **Nothing in the tier's assertions moved.** Same preset, same `ASAN_OPTIONS`/`UBSAN_OPTIONS`, same three greps for a
  sanitizer finding, same `tier3 OK` line. A parallel build changes how long the evidence takes, not what counts as it.
- `docs/DESIGN_REVIEW.md` states the cold and the incremental cost separately now, and says the build is parallel, so a
  reader cannot read a fifteen-minute run as a check that grew.

## The Python relay is the reference, not the route (2026-11-15)

- **The live page leads with the relay binary.** The `ws` sandbox section and the three copy-paste commands place orders
  through `./out/build/linux-debug/venue_relay`, and the numbers beside them are the C++ route's own 120-second run
  (`placed=20`, `trade_reports=27`, `[RECON] status=ok`), not the Python run's.
- **`tools/live/venue_relay.py` stays, and the documents say what it is for.** It is the implementation the engine's
  order session was ported from, kept so a reader can diff the port against its source; its `--self-test`, stand-in
  venues and frame dump are what the C++ tests were written from, and tier 1 still runs the self-test. It is documented
  as the secondary route: one `/// details` fold on the live page (with the 2026-11-14 Python numbers, kept as the
  record they are), the `live/` row and the inventory line of `docs/TOOLS.md`, and the "What stays in Python" bullet of
  ADR 0006.
- **The defects the demo venue taught are attributed.** The contract that fills in parts, the fill with no fee and the
  fill with no symbol were found on the Python route before the port; the paragraph now says so, because the fixes are
  the port's reason to exist and the runs that found them are the Python ones.

## The order leg's frame dump, and the acceptance tiers get one word (2026-11-15)

- **`--order-dump=FILE`.** Every frame both ways, appended, one JSON object a line -- the file the Python route used to
  explain its first silent sandbox order, where the venue had answered twice and the client had dropped both. The login's
  `apiKey`, `passphrase` and `sign` are replaced by `***` on the way in, because a file that exists to explain a silent run
  must not become the copy of the key that leaked; the frame is JSON-escaped, so a quote inside one cannot end the line
  early; and a path that cannot be opened is an error naming it rather than a silent no-op.
  `tests/venue_order_session_test` points the dump at a file, then asserts that both directions appear and that neither
  the api key nor the passphrase does.
- **`--order-no-subscribe` stays out, and here is why.** The Python route has it for the *business* endpoint, which refuses
  the `orders` subscription; this route targets the private channel, where the subscription is the half that carries
  state. A placement-only shape would be a different route -- no socket at all -- rather than a flag on this one.
- **`.run.sh` gained `full` and `asan`**, so the two acceptance tiers `all` is made of are one word in this environment
  instead of a hand-typed command the terminal may not deliver.
- The counts moved and nothing else did.

## The placement route's latency is counted before it is designed (2026-11-15)

- **Three counters and one number.** `RestPlacer` now counts every call it makes, the milliseconds spent in connect plus
  the TLS handshake, and the milliseconds of the whole placement, and the summary prints them as `rest_calls`,
  `rest_connect_ms_avg` and `rest_place_ms_avg`. The counts are taken on the failure path too, because a call whose
  answer never arrived paid for its handshake as well -- and those are exactly the calls a kept-alive connection would
  have saved.
- **Measured on the demo venue**, 120 s, 25 REST calls: `rest_calls=25 rest_connect_ms_avg=94 rest_place_ms_avg=228`
  beside `orders=14 cancels=11 placed=14 venue_refusals=3 transport_failures=0` and `[RECON] status=ok`. So connect plus
  the handshake is about two-fifths of a placement (arithmetic on those two averages), which is the share a pooled
  connection would have to beat. **The decision is not to pool it now, and the number that would change it is the
  stale-cancel share** -- a faster cancel reduces the `51400` refusals a REST round trip produces (62 of 104 cancels in
  the reconnect run above) -- while a pool is a connection state machine with its own framing, reuse and failure modes,
  and nothing this line reports needs it today. The count stays in the summary so a later session can re-open the
  question with a new number rather than an argument.
- `tests/venue_order_session_test` pins the shape rather than the timing: the summary carries `rest_calls=6` and a
  `rest_connect_ms_avg=` field after its six REST calls, and a placement's total is never less than the connection inside
  it.
- The counts moved (133 files, 28,397 lines; the live group 4,725) and nothing else did.



- **The reconnect path is now measured against the venue, not only on a loopback.** `--order-drop-after-seconds=N` closes
  and reopens the private connection every N seconds -- the order leg's twin of the market leg's `--drop-after-seconds` --
  and a 180-second demo run with it armed logged in five times and subscribed five times over four reconnects:
  `orders=20 cancels=46 replies=87 refused=0 write_failures=0 sessions=5 placed=20 venue_refusals=31 transport_failures=0
  logins=5 subscribes=5 reconnects=4 unreadable=0`. The engine reconciled across all of it: `[RECON] status=ok` with
  `quantity_breaks=0 report_count_difference=0 fee_difference=0.000000`. A gap costs a re-subscription rather than a fill,
  because the channel re-sends a live order's cumulative state when it comes back.
- **Four fills against the previous run's twenty-seven is the cap, not the channel.** `--venue-max-orders=20` refused 29 of
  the 49 submissions this strategy made, and a cancel-and-replace cycle spends that cap quickly. Said out loud because the
  two runs are otherwise the same command.
- **The REST client's three untested branches are tested.** A stand-in that answers `Transfer-Encoding: chunked`, answers
  with a body that is not JSON, and closes without answering pins `dechunk`, the parse refusal and the closed-connection
  case: the first is the `ACK` it is, and the other two are `REJECT`s counted as transport failures rather than as
  silences. `tests/venue_order_session_test` grew all three, and its stand-in venue now serves every connection until the
  test stops rather than exactly two, which is what lets a scheduled drop be observed.
- No recorded number moved; the source counts did.



- **The order leg's wire is one file.** `src/lines/line_socket.h` holds what both ends of the adapter's protocol need --
  `write_all`, `write_line` and `dispatch_lines` -- and `TcpVenueTransport` and the relay's order channel call it. Before
  this, "send every byte with `MSG_NOSIGNAL`" was written out four times and "split on the newline, strip a `\r`" twice,
  which is how one end starts accepting a carriage return the other never sends without anything failing.
- **The market decoder left the relay.** `src/lines/venue_market_decoder.{h,cpp}` holds `VenueDecoder` and `RelayStats`;
  `venue_relay.cpp` fell from 865 lines to 657 and now holds the venue client, the row queue, the recorder and the two
  entry points. `venue_relay --self-test` decodes the same shapes, caps a snapshot the same way and counts the same gaps.
- **Two things deliberately not done.** The two `split_fields` are not one function: the translator splits an engine line on
  spaces and drops empty fields, the recorder splits a CSV row on commas and keeps them, so merging them would move a
  behaviour change into a refactor. And there is no wider pass: the rules in `.clinerules` forbid a clean-it-all-up
  rewrite, so each change above is a move with a countable witness (four copies to one, twice; 865 lines to 657).
- The counts moved (133 files, 28,340 lines; the live group 4,668) and nothing else did.



- **ADR 0006's fourth slice is accepted against the venue, not only on a loopback.** With OpenSSL present -- a build flag
  rather than a code path -- the C++ relay served both legs of a 120-second demo session with no Python in it: the market
  leg from the venue's public feed, and the order leg on `--order-listen 127.0.0.1:8766 --order-rest https://www.okx.com
  --order-inst-code=10459`. The engine's side: `[VENUE] orders=20 cancels=32 refused_by_adapter=16 write_failures=0
  reports=103 dropped=0`, `[EXECUTION] trade_reports=27 fill_rate=0.132222`, `[PORTFOLIO] equity=998.5 fees_paid=1.0772`,
  `[PNL] gross_pnl_usd=-0.423298 net_pnl_usd=-1.50049`, and `[RECON] status=ok` with `quantity_breaks=0
  fee_difference=0.000000`. The relay's side: `placed=20 venue_refusals=19 transport_failures=0 sessions=1
  unreadable=0`. `placed=20` is this slice's own quotient: twenty placements signed with an ISO-millisecond timestamp and
  sent as HTTP, none of them over the socket.
- **Three defects the venue found, each fixed where it lives.** The 200 ms receive timeout is what keeps a caller's clock,
  and both handshakes read its first expiry as "the venue hung up": one run reported `transport_failures=26` while the
  loopback test stayed green, and the handshakes now retry against a fifteen-second deadline of their own. The keepalive
  sent `{"op":"ping"}`, which the private channel refuses as `60012 Illegal request` -- it takes the bare text `ping`, as
  the Python relay always did. And a `notice` or an `error` event was counted as `unreadable`, so a healthy session
  printed a number that reads as a defect; an `error` event is printed now and neither is counted.
- **What the run does not claim**: 19 of the 32 cancels were refused `51400`, because a REST round trip is long enough
  that the venue had often filled the order first. That is the route's shape rather than its defect, and each refusal was
  answered `REJECT <id> venue` instead of dropped. Twenty-seven fills in two minutes is a wire acceptance, not a rate.
- The live page gained the run, the command that reproduces it and the three defects; `[RECON] status=ok` is unchanged.



- **ADR 0006's fourth slice: `RestPlacer`.** This venue answers `50026` to its websocket trade operation and places over
  REST, so `venue_order_wire` now posts the translator's own `args[0]` as the body of `POST /api/v5/trade/order` and
  `/api/v5/trade/cancel-order` on the TLS stream the tree already has, signing with
  `venue_auth::rest_signature` -- whose timestamp is ISO 8601 **milliseconds**, the opposite of the login's epoch seconds.
  The answer becomes the adapter's lines: `ACK <id> <units>` from the reply's `sz`, `CANCELED <id> user` for a cancel,
  and `REJECT <id> venue` for a refusal, whose `sCode` and `sMsg` are printed rather than swallowed. A transport failure
  is a `REJECT` too, so no submission goes unanswered.
- **One translation, two transports.** `OrderTranslator::request_of` returns the request's two halves -- the operation, the
  id, and the `args[0]` object as text -- and `encode` joins them for the websocket. `encode`'s output is unchanged, which
  `venue_order_translator_test` checks, and the REST body is therefore the very object the socket op would have carried.
- **`venue_relay --order-rest=BASE_URL`** drives it: the private socket still logs in and subscribes the `orders` channel
  (state comes over it), and placement goes over REST. The summary gained `placed=`, `venue_refusals=` and
  `transport_failures=` beside the counters it had.
- **The order leg's engine socket is a line socket, not a WebSocket one.** `TcpVenueTransport` writes one line a frame and
  splits what comes back on newlines, which is the Python relay's order route too; the market leg's opening handshake had
  been reused for it and would have been a peer that never introduces itself. The two accept paths now share
  `listen_on`, and the printed line carries the port the kernel gave.
- **What is not covered**: the build used for this change has no OpenSSL, so a `wss://` venue is refused by name -- the
  evidence for this slice is a loopback socket and a line smoke, not a venue. Reaching the demo venue still needs the
  Python relay on such a host, which is the fallback ADR 0006 recorded.
- `tests/venue_order_session_test` gained the REST half: a stand-in endpoint that recomputes the signature from the secret
  it holds over the bytes and timestamp it received, an `ACK 11 100000` from a reply whose `sz` is ten contracts, the
  exact body pinned as a literal, a `REJECT 12 venue` for a sub-code refusal, `CANCELED 11 user` for a cancel, and the
  socket's own `FILL 7` still arriving beside it.
- No recorded number moved; the source counts did.



- **ADR 0006's third slice is complete.** `src/lines/venue_order_wire.{h,cpp}` is the connection the state machine runs
  beside: it dials the venue, reads on a 200 ms timeout so the session's keepalive is due on its own clock, answers both
  heartbeat forms this venue uses (a control ping and a text `ping`), and reconnects -- logging in and subscribing again --
  on the Python relay's schedule (one second, doubling to eight).
- **One socket implementation, not two.** The market leg's URL, TLS stream, WebSocket frames and opening handshake moved
  out of `venue_relay.cpp` into `src/lines/venue_ws.h`, which both legs include; the relay's file-local copies are deleted.
  That is the move `venue_json.h` made for the reader in `706ec3c`, and it is what lets the order leg's test run a
  stand-in venue through the frames the client actually writes instead of a string comparison.
- **`--order-listen` serves the order channel from C++.** The relay accepts the engine's WebSocket connection, translates
  `ORDER`/`CANCEL` and returns the venue's reports as the adapter's lines, so `LiveVenue` over `TcpVenueTransport` cannot
  tell this route from the simulated one. A line the translator refuses is answered as `REJECT <id> venue` rather than
  dropped, and the summary prints what crossed plus the session's own counters. The three credentials come from the
  environment; the leg's flags are `--order-listen`, `--order-venue`, `--order-inst-code`, `--tick-size` and `--lot-size`.
- **A request submitted before the login is held, not dropped.** The relay accepts the engine's connection before the
  venue's login has come back, so the first order of a session can arrive with no socket to write it to; it waits in a
  bounded queue and goes out after the login, and only a frame that could not be *written* is counted as a failure and
  discarded -- an order sent twice is worse than one nobody answered.
- **What it does not do**: placement still needs the relay's REST route, which is slice four, so a run against the demo
  venue -- whose websocket trade operation answers `50026` -- still uses `tools/live/venue_relay.py` for the order leg.
  Nothing here has been run against a venue: the evidence is a loopback socket.
- `tests/venue_order_session_test` gained the socket half. Its stand-in venue answers the handshake, recomputes the
  login's signature from the secret it holds, acknowledges the subscription, and pushes one fill; the assertions are the
  driver's lines (`FILL 7 6421.5 10000 0.1`, a `REJECT` for a size below one contract) and a forced drop that has to
  produce a second login and a second subscription (`sessions=2`, `reconnects=1`, no write failure).
- No recorded number moved; the source counts did.



- **One parser, two users.** `src/lines/venue_json.h` -- extracted from the relay for the order translator -- is now what
  the relay itself reads: its file-local `Json`/`JsonParser` is deleted and it includes the shared header. The C++ relay's
  `--self-test` decodes the same captured shapes as before and the market decoder is untouched, so the removal is one
  definition fewer rather than a behaviour of its own.
- The duplication ADR 0006 named as deliberate and temporary is therefore gone in the same stretch that landed the state
  machine, which is when the record said it would go. A second parser that drifts is the failure the record exists to
  stop, and a header both sides include is how it stops.
- No recorded number moved; the source counts fell by the parser's length, and the size table and MAINLINE's file group
  state the new totals.

## The order session's state machine moves in (2026-11-15)

- **ADR 0006's third slice, state machine first.** `src/lines/venue_order_session.{h,cpp}` holds what to send when the
  connection comes up (the login, over the timestamp its signature is computed from), what the venue's frames mean (its
  login answer turns into the `orders` subscription, an `orders` push turns into the adapter's lines), the keepalive
  cadence, and the reconnect that logs in and subscribes again. It writes to a `Sink` the caller supplies, so the machine
  runs in one process under a test -- the claim the relay's Python stand-in makes across a socket, which a straight port
  of the socket code could not inherit.
- **An account's channel is filtered here, not in the translator.** The venue's `orders` subscription covers every
  instrument the account trades, so an entry for another one is counted as `other_instruments` rather than answered to the
  engine: a fill booked against the wrong symbol is a position nothing explains.
- **`decode_orders_entry` splits out of the translator** so the session can check the instrument before decoding one
  entry; `decode()` loops over the entries and calls it, with no behaviour change -- `venue_order_translator_test` is
  untouched and green.
- `tests/venue_order_session_test` drives the machine in one process: the stand-in recomputes the login's signature from
  the secret it holds and finds the relay's pinned vector at the pinned timestamp, the subscription follows the venue's
  answer rather than a timer, a drop forces a second login and a second subscription with a different timestamp, the ping
  fires at twenty seconds and not at nineteen, and a frame it cannot read is counted rather than answered.
- **What it does not do**: the TLS/WebSocket plumbing is not wired to it yet, so the live path still needs the relay, and
  the relay's file-local JSON reader is deleted when that wiring lands. The ADR says which part of the slice is landed.
- No recorded number moved; the source counts did.

## The order translator moves in, on the relay's own test cases (2026-11-15)

- **ADR 0006's second slice: `src/lines/venue_order_translator.{h,cpp}`.** The engine's `ORDER` and `CANCEL` lines become
  the venue's `args[0]` object, and the venue's replies become the `ACK`/`FILL`/`CANCELED`/`REJECT` lines the adapter
  decodes. The rules are ported rather than reinvented: a size below one contract is refused where both numbers can be
  named, a size rounds **down** to the venue's lot because rounding up turns a demonstration into a position, a price is
  rounded onto the tick because the venue refuses one step off its grid, and a fill is one per increment of the decimal
  `accFillSz` -- never an integer read, which is the defect the first live session found in the Python relay.
- **The fee rides on the fill.** `fillFee`, or the order's cumulative `fee` when there is no per-fill one, is negated into
  the engine's sign so a charge is positive and a rebate negative -- the same convention the fixed relay uses, now in the
  engine.
- **One JSON reader, extracted rather than copied.** The relay's recursive parser moved to `src/lines/venue_json.h`, which
  the translator includes. The relay keeps its file-local copy until slice three deletes it with the session; that
  duplication is in the ADR rather than hidden.
- `tests/venue_order_translator_test` pins the relay's own cases as literals: the request's shape with and without
  `instIdCode`, a cancel naming the instrument, the below-one-contract refusal with both numbers, rounding down, the tick,
  the ACK's working size, a decimal fill (0.45 → 4500 units) and its repeat counted rather than booked, the fee and a
  rebate, a counter that goes backwards, a top-level refusal with and without an id, and an unknown channel.
- No recorded number moved; the live path still needs the relay for slices three and four.

## The venue's order session starts moving into the engine (2026-11-15)

- **ADR 0006 accepts the move, in four slices.** The market leg's session is already C++ -- TLS, the WebSocket client, the
  reconnect loop -- while everything about an *order* is still in `tools/live/venue_relay.py`. A live path that needs a
  Python process to hold its session is a second implementation of the engine's liveness, and the two have never run in
  one process. The record splits the port into authentication, the translator, the session and the REST placement, each
  with its own test; it amends ADR 0005's cost list rather than superseding it, because 0005's decision stands.
- **The authentication is the first slice, and it is in.** `src/lines/venue_auth.{h,cpp}` is SHA-256, HMAC-SHA256 and
  base64 written dependency-free beside the adapter, for the reason `ws_feed.cpp` writes SHA-1 and base64 for the opening
  handshake: linking OpenSSL into the engine's line library would make every build depend on it. The two preimages are
  the venue's -- Unix epoch seconds for the login, ISO 8601 milliseconds for REST -- and `tests/venue_auth_test` pins
  RFC 4231's first and sixth cases (the sixth covers a key longer than the block, the branch a port silently skips), the
  relay's own pinned login signature, and a REST signature computed with an independent implementation. The two
  signatures are asserted to differ: a client that shares one preimage across both surfaces is one error code away from
  looking like a bad key.
- **What it does not do**: the session, the translator and the REST placement stay in the relay for now, so the live path
  still needs it and the live page still says so. The remaining slices and their tests are named in the ADR; slice
  three's is a C++ stand-in venue that verifies the signature it receives and drops a connection once.
- No recorded number moved; the source counts did.

## The demo venue's own book, and the three defects it found (2026-11-14)

- **The acceptance run ADR 0005 asked for.** Ninety seconds of the demo venue's public `BTC-USDT-SWAP` channel as the
  market leg, `LiveVenue` placing over REST with its `orders` channel subscribed for state, both legs recorded, and the
  exposure cap set: 15 fills at one contract an order, `max_abs_exposure_usd=656.583` with
  `exposure_over_collateral_fills=0`, `[RECON] status=ok` with `quantity_breaks=0 fee_difference=0.000000`, and the
  account closing at `equity=997.376` with `[PNL] gross_pnl_usd=0.373523 net_pnl_usd=-2.62371` -- gross positive, net
  not, because the fee is 2.99724 of a 0.373523 gross.
- **A contract fills in parts, and the venue says so in decimals.** `accFillSz: "0.45"` on a one-contract order was read
  with `int()`, which raised inside the decoder and dropped the private connection -- every state message after the first
  fractional fill was lost, and the session recorded three of its fills.
- **A live fill carried no fee.** The adapter's `FILL` frame had no field for one, so `fees_paid` was zero while the
  venue had charged `fee: "-0.14999409"` on the same fill: a live `[PNL]` was gross of the fee the project is about. The
  frame now carries the venue's own `fillFee`, negated into the engine's sign, so a rebate arrives negative.
- **A live fill carried no symbol, and that is why the equity looked like cash.** `LiveVenue` stamped owner, side and
  role but not the symbol, so the position was booked under the empty string: `[EXPOSURE] legs=:-14300:-0` and
  `[PORTFOLIO] equity=1950.98` -- cash, with a 1.43-contract short worth nothing in the account. The symbol now comes
  from the order that was sent.
- **Two guards for the case where there is genuinely no price.** The closing mark is the last *positive* midpoint, so a
  book whose side emptied cannot overwrite it; and with no mark at all the run prints `[MARK] missing symbol=… quantity=…
  reason=no_positive_midpoint equity_is_n/a`, `equity=n/a marks_complete=0`, `[EXPOSURE] … unmarked=1` and `n/a` for
  `gross_usd`, while `[PNL]`'s `net`, `gross` and the two per-fill rates follow the same flag and `closed_pnl_usd` stays a
  number. The run still finishes and reconciles: this is a statement about a number, not a refusal.
- `tests/portfolio_test` pins the guard (a zero-price mark keeps the previous one; a declared-unmarked account prints
  `n/a` on both lines and the exposure names the leg it could not price), and `tests/live_venue_test` pins the symbol on
  a report next to the owner, the side and the optional fee.
- No recorded number moved; the source counts did, and the size table and `MAINLINE.md` carry the new total.

## The live leg's record had no clock (2026-11-14)

- **A live venue's answers now carry the market clock.** `LiveVenue` decoded `ACK`, `FILL`, `CANCELED` and `REJECT`
  without a timestamp at all -- the wire has no field for one -- so `--record-orders` wrote `ts=0` on every report row. A
  replay of that file then released every answer at its first event and stopped at the third submission: three replays,
  the same order, one tick apart. The adapter stamps each report with the run's market clock, one microsecond before the
  event it is applied at, because `RecordedVenue` releases strictly past a timestamp and an answer that arrived between
  two events has to reach the strategy at the event it reached it live. `tests/live_venue_test` pins both halves: no
  clock keeps `ts=0` (a test with no market), a clock carries the market time of the frame it came from.
- **The step the live page named does not work, and that is measured rather than argued.** One `ws` session through the
  live adapter, recorded twice and replayed with `--replay-orders`, cannot reproduce: the same command run twice on one
  market recording placed 154 and 261 orders and wrote two different order legs, while the two market legs were
  byte-identical. Which answers have been drained when an event is handled is the receive thread's timing, so a live
  recording is an audit trail rather than a replayable stream. The loop that reproduces is the matcher's: recording and
  replaying then run the same reader over the same rows, `[ORDER-REPLAY] submissions=305 cancels=290 reports=655
  rows_left=0`, ending on the same `[PNL]`.
- **The live page's sandbox command could not place a single order.** Its third positional is the engine's
  `quantity_scale`, and it was `1` against a relay whose default is a million units a coin: every order was 1e-6 of a
  coin and was refused as below one contract. The command now matches the two scales (`active_l2 1000000`,
  `--order-size=10000`, one contract an order), and the page states what that gives: `[VENUE] orders=15 cancels=15
  reports=30`, `[EXECUTION] trade_reports=7`, `[ORDER-LOG] submissions=15 reports=30`.
- No recorded number moved; the source counts did -- `app` by 16 lines and `lines` by 16 (the stamp, its wiring and the
  test), so the size table and `MAINLINE.md` state 26,395.

## The order leg on the venue's two surfaces at once (2026-11-14)

- **The REST route subscribes the `orders` channel.** The demo venue answers `50026` to its websocket trade operation
  and places the same order over REST, so `--order-rest` had been a placement-only route: it opened no socket, and a
  fill on the venue's `orders` channel had nowhere to arrive. `serve_venue_orders` now opens the private socket for the
  subscription beside the placer -- two connections, because that is what the venue offers rather than a choice of the
  relay's -- and `--order-no-subscribe` restores the placement-only shape. The three combinations are a function,
  `venue_order_route`, whose rows `--self-test` pins without a key or an endpoint.
- **The two halves print on two lines.** A REST run's summary keeps `placed`/`venue_refusals`/`transport_failures` and
  adds `order state channel: logins=… subscribes=… reconnects=… messages=…` when the socket is open, so a run whose
  placements were answered and whose reports never arrived says which of the two connections was silent.
- **A fill is still unmeasured, and now for one reason.** The de-duplication the channel needs is its cumulative
  `accFillSz` counter, and the size reaches the venue in the venue's units. What is left is the price: the replay is from
  2020 and the venue's book is 2026, so an order sits far from the touch and nothing crosses. The next step is one `ws`
  session recorded twice -- book and orders -- for `recorded_venue` to replay on its clock.
- No C++, no recorded number and no size count moved.


## The agent's own protocol, measured (2026-11-14)

- **A long command now has one runner.** `tools/gate/detach.sh <name> [seconds] -- <command>` starts the command
  detached from the agent's shell, writes `/tmp/<name>.log` and drops `/tmp/<name>.exit` with the status when it is over,
  so "is it finished" is a file read rather than a question to a shell that loses completion. It exists because a
  foreground command that outlived the tool's budget was killed by the next one, cutting a post-commit sync between its
  manifest and its copy and leaving an empty `diff.patch` for review.
- **The rules that cost the most to learn are now in section 3 of `.clinerules`**, with the timings measured on this
  host: `code` ~20 s, `fast` ~4 min (the header's 100-130 s was a quieter machine), and the document half costing
  ~3.5 min because `check_docs.py` walks the studies page and the multi-day check.
- **Polling has a rule against the loop detector**: read `/tmp/<name>.exit` first, alternate with real work, and never
  ask the same unchanged file five times -- that is what stopped a session this week.
- **The front doors no longer call the size mapping unsolved.** `docs/index.md` and `README.md` both said the quantity
  mapping was "measured and named rather than solved"; the relay converts it now, so both say that instead.
- **The gate's own header says what a `fast` costs here.** `verify_l2.sh` claimed ~110 s for the document half and ~2 min
  for `fast`; measured on this host the same run is ~3.5 min and ~4 min, so the header says that and names the runner.
- **A commit is not finished when `git commit` returns.** The hook is a child of the committing shell; the reply waits
  for the four `ok` lines and `synced <hash>`, and a log that stops at `copying ...` is re-run with
  `TOYQUANT_SINCE=<the previous manifest commit>`.
- No C++, no recorded number and no size count of `src/` moved.
- Gate: `docs OK` on `TOOLS.md`, `lint_prose` 0 problems, `check_links` 0 problems, `mkdocs build --strict` green,
  `fast` green (`tier0 OK` / `tier1 OK`).

## The sandbox's four open items (2026-11-14)

- **The adapter's cap can be priced.** A count and a size bound say nothing about a strategy that sends few, large
  orders, so `LiveVenue` gained `--venue-max-notional=USD`, computed with `fill_notional_usd` -- the helper the ledger
  books with -- and refused before anything reaches the wire. `tests/live_venue_test` fires it twice: on a price-quoted
  instrument, and once where the engine's own `quantity_scale` is what makes twenty million units 200 USD.
- **The relay converts the engine's numbers into the venue's.** `sz` was sent as if a contract were an engine unit and
  `px` as if the venue took any decimal. The translator now divides by `--quantity-scale` and the contract's
  `--contract-size`, rounds down to `--lot-size`, rounds the price onto `--tick-size`, and refuses a size below one
  contract with both numbers. `venue_relay.py --self-test` and `tests/relay_order_channel_test` measure it end to end:
  100,000 engine units are ten contracts, and the sandbox's half fill comes back as 50,000 units.
- **The `orders` channel is de-duplicated.** The venue re-sends an order's state on every change and again after a
  reconnect, and `accFillSz` is cumulative; one fill is booked per increment of it, the repeats are counted as
  `deduplicated`, and a counter that goes backwards is refused rather than booked.
- **`--replay-orders` replays a recorded session on its own clock.** `RecordedVenue` holds every report and releases it
  when the run's clock passes its timestamp -- the reading ADR 0005's amendment chose -- releases the tail at the end
  and counts it as `flushed`, and the run prints `[ORDER-REPLAY]`. Measured on the 200k slice: recording and replaying
  the same session give the same `[PNL]` (`net_pnl_usd=0.0071445`) and the same `[EXECUTION]`
  (`submitted_orders=305`, `trade_reports=15`), with `[ORDER-REPLAY] reports=655 rows_left=0 pending=0 flushed=0`.
- **Two commands in the quickstart could not run as written.** `--record /tmp/one` and `--record-orders /tmp/session`
  are prefix flags the parser accepts only as `--name=value`; both are fixed, and `lint_prose.py` now fails a bash
  fence that writes one with a space.
- Gate: `code OK` and `docs OK`, `lint_prose` 0 problems (84 flags, 120 files / 26,363 lines, 47 documents),
  `check_links` 0 problems, `mkdocs build --strict` green, `fast` green (`tier0 OK` / `tier1 OK`).

## The data page matches the checkout it describes (2026-11-14)

- **`docs/DATA.md` opened on a directory that no longer exists.** "This directory has two purposes, `scenarios/` and
  `runtime/`" was true before the 2020-2026 market data arrived, and the page is almost entirely about `data/v2/`. The
  opening is a three-row table now, and its useful column says `data/v2/` is ignored: a copy of it is local, so the
  inventory further down is a snapshot rather than a fact about the repository.
- **The inventory named six families and the checkout has seven.** `options/` -- five equity-option series, 124 KB --
  was missing, `crypto/` said 12 files and 4.6 GB where `ls` and `du` give 21 and 8.5 GB, and the "removed" paragraph
  claimed three 2020 snapshots that are on disk again. The table says what its numbers were measured on, which is what
  a page about ignored files can honestly claim.
- **The buffer rule is written down.** `.clinerules` now carries it: exploration lands in the studies record or an
  ADR, the guides wait until the core path works, and after that every change is a local increment -- a rewrite held
  back to the end is a second tree nobody reviewed.
- The slice examples lost their tab indentation, and two lead-ins were counts that no longer matched what followed
  them ("Two things" over three rows, "Six properties" over five); no command changed. No C++, no recorded number and
  no size count of `src/` moved.
- Gate: `tier0 OK` / `tier1 OK` (`fast`), `lint_prose` 0 problems, `check_links` 0 problems, `mkdocs build --strict`
  green.

## The two path figures are drawn by a script, and an SVG include is checked (2026-11-14)

- **The front door's diagram was unreadable in the site's dark palette.** The site's own text colour won over the
  figure's `classDef` colour, so every label came out pale on a pale box, and the quickstart's two-row figure was laid
  out side by side as well -- two subgraphs with `direction LR` and no edge between them are one row to Mermaid. Both
  are SVG now, drawn by `tools/study/figure_path.py`: dark ink on a pale fill and a mid-grey connector, fixed rather
  than inherited, so the same file reads on a white page, on Material's dark scheme and as the image README shows.
  `--check` runs in tier 1, the generator refuses a label that cannot fit its box, and the root carries `width` and
  `height` so an `<img>` has an intrinsic size.
- **`charge.svg` was never on the page.** An SVG included on its own is not a Markdown block: Python-Markdown closed
  the paragraph after `<svg>`, so the built page carried `<p><svg></p>` and then a paragraph per `<text>` element. It
  sits in a fold, which is why no reader had said so. Every include is wrapped in `<figure>` now, `lint_prose.py`
  fails a bare one, and the two figure checks run in the gate.
- The two `.mmd` files are gone, and README's hand-kept Mermaid copy is an image of the same SVG: one source, not two.
- No C++, no recorded number and no size count moved.
- Gate: `code OK`, `docs OK` on the three pages, `lint_prose` 0 problems, `check_links` 0 problems,
  `mkdocs build --strict` green, `fast` green (`tier0 OK` / `tier1 OK`).

## The tools page becomes an inventory, and the lint counts it (2026-11-14)

- **`docs/TOOLS.md` named ten of the fifty-five files under `tools/`, and it is the page a reader is sent to for a
  filename.** `docs/MAP.md`'s "find the tool behind a number" row points there; the directory table described each
  directory in a phrase, so `compare_runs.sh` -- the tool whose `--markdown` emits the marked blocks `check_docs.py`
  re-runs -- could not be found from its name. The page now carries a folded inventory: one row per file, what it
  prints, and whether the gate runs it.
- **Two cells of that table were wrong.** `live/` was marked as never run by the gate while tier 1 runs
  `venue_relay.py --self-test`, and `study/` counted five self-tests and left out `viability.py`, the verdict fixture.
  `verify_l2.sh`'s own header said "three tool self-tests" where it runs seven.
- **One heading counted six where the page below it had eight.** `L2_FLAGS.md` said "the six flags that need more than a
  line" over eight sections, `MAP.md` copied the six and left out the order leg and the live order leg, and the
  venue-side page's two links pointed at the old anchor. All three now say eight.
- **The design review had a sentence cut mid-clause**, `... restarted from the` running straight into the next heading
  with no blank line between them; it now ends "one walk rather than one walk per level", which is what the paragraph
  on the hint below measures.
- **The inventory is a checked claim, not a list.** `lint_prose.py` walks `tools/` and fails the build when a file has
  no line in the page; fed an empty page it reports all 55, so the check is known to fire. `--markdown` joins the flags
  that belong to a tool rather than to the binary.
- **The design review was keeping a second copy of two counts.** Its "46 scripts ... and its five lints" row now points
  at the inventory and names the six lints, its tier sketch gained `check_adr.py`, and `docs/MAP.md` counts the guide's
  eight pages instead of seven.
- No C++, no recorded number and no size count moved.
- Gate: `code OK` and `docs OK` on the six pages the change names, `lint_prose` 0 problems (82 flags, 4 queue models
  and 120 source files / 26,201 lines against 47 documents), `check_links` 423 links in 51 documents / 0 problems,
  `mkdocs build --strict` green, and `fast` green (`tier0 OK` / `tier1 OK`).

## The guide's pages: one figure, one table, three commands (2026-11-14)

- **The quickstart now shows the three paths the rest of the guide is about.** Its demo path was three invocations of one
  recording loop; it is one replay, the same engine over a socket (`ws`), and the carry -- and the recording loop with the
  `[PNL]` equality `fast` asserts is a fold under those commands. The relay's flags (`--order-rest`, `--record-orders`,
  `venue_relay.py`) are named nowhere in the quickstart's main view and live only on the live page.
- **The eight-line scoreboard is the quickstart's only table outside a fold.** The charge chart and the two lines measured
  after the eight moved into one fold beside it, so the page is one diagram, one table and three commands, which is what
  its own subtitle claims.
- **The live page states the two channels before the commands.** REST places and cancels; the private `orders` channel
  pushes state; the diagram draws the second as a dotted return edge, and the note under it carries the two ways a first
  sandbox run looks wrong by a factor of a hundred -- `acctLv=2` for `50026`, `ctVal=0.01` against a venue-unit
  `--order-size`. The troubleshooting table already had both rows on its first screen.
- **The page that owns the files now owns them.** `Scenarios and reports` gained the table of what a run leaves on disk
  (`--record`, `--record-orders`, `--snapshot-out`, `data/runtime`, the report's PNG), and the legacy analyser fold moved
  there from `Reading the output`; that page keeps the five terminal lines and two folds (the interval tool, and what a
  `[PNL]` mark leaves out).
- **`Writing your own strategy` ends with the loop** instead of a static link out: run the micro-scenario, read `[RECON]`,
  take the two lines that fail first to `Troubleshooting`, then `code` and `fast`.
- Three anchors moved with their headings, and the two documents that pointed at them were updated in the same commit
  (`docs/L2_FLAGS.md`, `docs/ROADMAP.md`) rather than left to `check_links`. Gate: `docs OK` on the seven pages,
  `lint_prose` 0 problems, `check_links` 416 links in 51 documents / 0 problems, `mkdocs build --strict` green,
  `fast` green.

## The documents that still said "no exchange connectivity" (2026-11-14)

- **Three documents described a boundary that had moved, and the code had it.** `--live-orders` puts `LiveVenue` over a
  socket and the relay signs the venue's own REST calls, so the design review's opening claim ("No exchange
  connectivity"), its §6 list ("No connectivity, no FIX"), `MAP.md`'s "what it deliberately does not" paragraph and the
  roadmap's coverage row all described an instrument that no longer existed. Each now states what exists: one order
  channel to a demo venue, the login, subscription and reconnect held by the relay, and no quoted number measured
  through it.
- **`IMatchingEngine` had four implementations while the design review said one.** The interface table now names
  `MatchingEngine`, `PaperBroker`, `LiveVenue` and `RecordedVenue` -- three engines a run can be configured with and one
  reader -- and the architecture's source map gained the leg's row plus a paragraph: a venue is another engine rather
  than a branch in the loop.
- **The roadmap's coverage row moved** from "live venue, TLS and authentication: missing; current boundary" to done
  behind a relay, with what is still deferred (a venue that is not a demo, FIX, the relay's recovery matrix lifted into
  the engine) kept in the P3 list. The change-log row for `--live-orders` already recorded the work; only the table that
  summarised it was behind.
- **`GLOSSARY.md` gained the order leg's terms** -- the leg itself, `owner`, `liquidity_role`, `ctVal` and
  reconciliation -- because the front door promises three engines behind one venue interface and the reference tier had
  no venue term at all.
- **`LOW_LATENCY.md` keeps its "no live venue round trip" limitation and now says why it survives a working order leg:**
  the leg exists, the round-trip time does not.
- Gate: `docs OK` on the seven pages, `lint_prose` 0 problems, `check_links` 420 links in 51 documents / 0 problems,
  `mkdocs build --strict` green, `fast` green.

## The sync that was running and failing for ten days (2026-11-14)

- **The workflow was persisted, and it was broken in a way nobody could see.** `.git/hooks/post-commit` already called a
  sync -- a script outside the repository, at a path in the operator's home directory. Its default repo was the pre-rename checkout
  directory, so from the moment that directory stopped existing every commit wrote
  `ERROR: <that directory> is not a git repository` into `/tmp/tq_gdrive_sync.log` and nothing else. The Drive folder
  stayed at `7035823` (2026-11-03) while the tree moved through two hundred commits. The log is the whole explanation,
  and nobody had a reason to open it.
- **The replacement is in the tree and checked after the copy.** `tools/export/push_windows.sh` runs
  `bundle.py --sync` into `out/export`, copies the four files to the Windows folder named by `.drive-target` (ignored:
  a machine path is not a repository fact), and then compares each file's **remote size and sha256** with the local
  ones. Measured on the first run: `ok code.txt 2236301B 21fe6c715cd4`, `ok docs.txt 1117433B`, `ok manifest.txt
  26376B`, `synced 2a06abc -> <the folder named by .drive-target>`.
- **`tools/export/install_post_commit.sh` installs the hook *before* its final `exit 0`.** The first version of this
  block was appended after `exit 0` and would never have run; the installer now inserts before the last `exit 0` and is
  idempotent, so a fresh clone is one command from the same workflow.
- **`.clinerules` section 6 carries the rule and the post-mortem**: sync before every reply, a missing destination is a
  failure that names it, the hook's log is read (`tail -3 /tmp/toyquant-sync.log`), and the retired script is named with
  the exact error it wrote.
- The two `ToyQuant_*.txt` files in the Drive folder are the old artifacts and are now older than `manifest.txt`; they
  are left in place rather than deleted, because deleting outside the repository is not this agent's call.
- **The workflow notes are files, not pages.** The reviewer's brief is `.reviewer.md` and the Drive target is
  `.drive-target`, both at the repository root and both ignored: in the repository so a later session finds them, out of
  git so no clone, no CI run and no bundle carries them (`git ls-files` is the bundle's file list, so an ignored file
  cannot leak into a copy). `.clinerules` states the rule -- a workflow, a machine path or a policy is not a new
  document -- and the sandbox's own output in the documents is recorded on the roadmap as a later request rather than
  started here.


- **A review needs the change, not the tree.** `--sync` writes the two bundles and the manifest into the folder named by
  `TOYQUANT_DRIVE_DIR` -- the folder a Drive client uploads from -- and adds `diff.patch`: the commit the previous
  manifest carried to `HEAD`, over the bundled paths only. A reviewer that has the manifest and the diff can say what
  changed and whether the bundle is current, without reading 3.35 MB of source.
- **A sync that cannot find its folder fails, naming it.** The command exits 1 and prints the variable and the
  directory; a reply that reports a task finished while the sync did not run is wrong, which is now rule 6 of
  `.clinerules` rather than a habit.
- **`--since`** overrides the base the diff starts at, and the self-test exercises it: an emit from `HEAD~1` must produce
  a diff containing `diff --git`, a manifest with a `since` line must still check, and a second sync must pick the base
  up from the manifest it just wrote (`previous_commit`), so the loop is incremental by construction.
- **The order the reviewer gets things in** is written down: manifest + diff; the three front doors on a first read;
  a full bundle only when a diff cannot answer. The reviewer does not write -- its output is proposals, and the edit
  happens in the tree under the gate.
- **Also fixed**: the `## Definition of done` heading that the previous documentation edit had swallowed, so the
  numbered list that follows it had no title.


- **The problem was a copy with no age.** A codebase dump sitting in a Drive folder was ten days behind -- it described
  a README three times the length of the real one -- and nothing in it carried the commit or a hash, so its staleness
  could only be found by accident. That is the rule this repository applies to numbers, applied to a document: if it
  cannot be re-derived, it is not a result.
- **`tools/export/bundle.py` writes two bundles and one manifest.** `code.txt` (`src/`, `tests/`, `tools/`, the two CMake
  files) and `docs.txt` (`docs/`, `README.md`, `CHANGELOG.md`, `mkdocs.yml`, `.clinerules`, `LICENSE`), each file
  preceded by a banner with its path, line count and sha256, plus a manifest that starts with the commit and one
  `sha256<TAB>lines<TAB>path` row per file. The file list is `git ls-files`, so ignored paths (`data/v2`, `secrets/`,
  `out/`) are absent by construction and the commit is the one those bytes came from.
- **`--check` is the point.** It re-hashes the tree, names every file that changed, appeared or disappeared, and notes
  how far the snapshot's commit is behind HEAD; exit 1 when anything differs. Its `--self-test` proves the check fires
  by corrupting one hash and dropping two entries in a temporary bundle, so the tool is a check rather than a promise.
- **Measured on this tree at `66f299e`**, in bytes and at four bytes a token: `code.txt` 2,217,592 B (~554k tokens),
  `docs.txt` 1,104,519 B (~276k), `manifest.txt` 26,185 B (~6.5k). The manifest plus the three front doors
  (`README.md`, `docs/index.md`, `docs/MAP.md`) is 47,605 B (~11.9k), so a dump is **70x** the artifact an assistant
  should be given first -- and the first version of this sentence said "a thousandth" and "330x", which is arithmetic
  nobody had done.
- No C++, no recorded number and no size count moved. The tool is manual like the study tools, and `docs/TOOLS.md` says
  so in the row that lists it.


- **The account was the blocker, and the fix exposed the surface.** `50026` was not a transient: three attempts with
  backoff, integer and string instrument code, identical, with the clock within 120 ms. REST answered the real refusal --
  `51010 "You can't complete this request under your current account mode"` -- and the account was `acctLv=1`, spot-only.
  Switched to margin, REST places an order (`code 0, sCode 0, "Order placed"`, an order id, then a cancel that answers
  `code 0`), while the websocket trade operation still answers `50026`. Placement over REST, state changes over the
  private channel: that is the venue's shape, not a preference.
- **The relay grew the second route without a second translation.** `--order-rest URL` sends the translator's own
  `args[0]` as the REST body, so `OrderTranslator` is unchanged and the two transports share it. `--order-inst-code`
  passes the venue's `instIdCode` rather than deriving it, and `rest_timestamp_now()` exists beside `timestamp_now()`
  because the two surfaces want opposite formats (`50112` against `60004`).
- **Measured, on the engine's side**: `[VENUE] frames_sent=2 frames_received=2 orders=1 cancels=1 reports=2 dropped=0`,
  `[ORDER-LOG] submissions=1 cancels=1 reports=2`, `[RECON] status=ok`; the relay printed
  `placed=1 venue_refusals=0 transport_failures=0`; the venue answered
  `{"code":"0","data":[{"clOrdId":"1","ordId":"4096362816047378433","sCode":"0","sMsg":"Order placed"}]}` and its cancel.
  The frame dump holds all four lines, so the run is re-read rather than remembered.
- **What it did not measure**: a fill. The `orders` channel is not subscribed on the REST route, and the size is in the
  replay instrument's units against a venue contract of 0.01 BTC at 2026 prices, so a fill would be a number about
  nothing. Both are named on the live page as the next pieces.
- No C++ changed, no recorded number moved, and the tool's self-test carries the new pins (the three answer shapes, the
  translator's body being the REST body, the two timestamp formats).

## The sandbox order that was answered twice and heard neither time (2026-11-13)

- **The order left under the replay's name.** It carried `instId: "BTC-PERPETUAL"` -- the Deribit instrument the replay
  runs on -- to a venue whose name for the thing is `BTC-USDT-SWAP`. `OrderTranslator.encode` now sends the relay's
  `--instrument` and keeps the engine's symbol for its own bookkeeping, so the mapping lives in the layer ADR 0005 says
  knows a venue's shapes; `--self-test` pins it with an order whose symbol differs from the instrument.
- **The venue answered, and the client dropped the answer twice.** The reply was `{"op":"order","code":"50014","msg":
  "Parameter id can not be empty.","data":[]}`. Two faults: the request carried no `id`, so the venue could not correlate
  it, and `OrderVenueClient.handle` returned early on the empty `data` list before `OrderTranslator.decode` could turn a
  refusal into a `REJECT`. The engine was told nothing and the run read as a venue that ignored an order -- which is what
  the previous entry in this file claimed, and it was wrong about the venue.
- **Two flags make the next silence attributable.** `--order-dump=FILE` writes every frame both ways, one JSON object a
  line, with the login's key and signature redacted (the self-test asserts nothing of the secret reaches the file);
  `--order-inst-type` chooses the `orders` subscription's type.
- **What the run says now, and why no fill follows.** With the id, the venue's complaint becomes specific -- `instIdCode` --
  and `--order-inst-code` passes the venue's own contract code rather than deriving one. Past that the answer is
  `50026 "System error. Try again later."` three times, a second apart, as a number and as a string, with the host clock
  within 120 ms of the venue's: no type error, no transient, no drift. The same order over REST -- which needs ISO-8601
  milliseconds where the websocket login needs epoch seconds, `50112` against `60004` -- answers `sCode 51010, "You can't
  complete this request under your current account mode."`, and the account reports `acctLv=1`: spot-only, where a
  perpetual swap is not a spot instrument. So `50026` was that refusal collapsed into a system error, and the blocker is a
  venue-side account setting -- single-currency margin, `acctLv` 2 or up -- not a request. The round trip is proven
  (`[VENUE] frames_sent=2 frames_received=2 reports=2`, both rejections booked); the size stays open, being in the replay
  instrument's units and not the venue's. The error codes this line has produced are folded on the live page, one page
  down from the command.
- No C++, no recorded number and no size count changed; `--self-test` carries the new pins and the gate runs it.

## The live venue becomes the run's venue (2026-11-13)

- **Three flags, one decision.** `--live-orders --venue-endpoint=HOST:PORT --venue-max-orders=N` replaces the
  in-process matcher with `LiveVenue` over `TcpVenueTransport`, and the broker, the order recorder and the gateway stay
  around it unchanged -- the same decorator stack every recorded run uses, which is what ADR 0005 decision 1 bought.
  The three are required together and refused outside `l2_replay` and `ws` and on a two-book run. The live leg's replies
  are applied on the consuming thread -- the transport's receive thread only queues a line -- so a fill that arrives
  between two events is in the account before the next decision, and no accounting state is touched from two threads.
  `[VENUE]` is the run's own label for it: frames sent, frames received, reports decoded, refusals, and no latency.
- **The first run reported every fill and booked none of them.** `[VENUE]` showed twelve decoded reports while
  `[BROKER]` showed `fills_seen=0` and `[PNL]` was all zeros. The venue's frames carry an order id and nothing else, so
  the reports the adapter built had no `owner` (the pipeline books only its own), the default `side` -- which is `Buy`,
  so a sell fill would have moved the position the wrong way -- and no liquidity role (so neither count moved and
  `[RECON]` reported a break). All three are now stamped from the order that was sent, a `Cancelled` or `Rejected`
  forgets the order, and a fill keeps it.
- **A test that had never run.** `tests/live_venue_test.cpp` returned 0 two thirds of the way down, so the four-reply
  mapping, the unreadable-frame counts and the two no-op calls were dead code: the adapter's report mapping had never
  been exercised. The early return is gone, those blocks run, and the owner, side and role assertions are added.
- **What a run says now**, against the relay's simulated venue: `[VENUE] orders=4 cancels=4 frames_sent=8
  frames_received=12 reports=12`, `[BROKER] fills_seen=4`, `[RECON] status=ok report_count_difference=0`, and a
  `[PNL]` carrying a real maker notional. Run by hand -- the app-level run is not in the gate yet, because it needs the
  relay as a child process and the 200k slice; what the gate holds is `tests/live_venue_test`.
- **The counts moved**, because the wiring is C++: 26,201 source lines (`app` 8978, `lines` 3872), 120 files and 35
  test binaries unchanged.

## The sandbox's first contact, and the three things it corrected (2026-11-13)

- **The credentials never touch git.** They live in `secrets/okx-demo.env`, which `.gitignore` excludes by directory,
  at mode 600 inside a 700 directory; `git check-ignore -v` names the rule, `git status` does not show the file, and
  `git ls-files secrets` is empty. The tools read `TOYQUANT_API_KEY`, `TOYQUANT_API_SECRET` and `TOYQUANT_PASSPHRASE`
  from the environment (`set -a; . secrets/okx-demo.env; set +a`), never from a command line, because a command line
  reaches a run's `[Mode]` line and every recording it writes.
- **The login timestamp was the wrong shape, and only the venue could say so.** The self-test pinned a vector and a
  local stand-in venue that recomputed the same function, so both agreed on `2026-11-13T00:00:00.000Z`; the venue
  refuses that form with `60004 Invalid timestamp`, refuses epoch milliseconds with `60006 Timestamp request expired`,
  and accepts **epoch seconds** with `{"event":"login","code":"0"}`. `timestamp_now` is epoch seconds now, the golden
  vector is re-derived for it, and the stand-in venue checks the digits rather than the ISO shape.
- **The port moved, because the venue says it is going.** Port 8443 answers a login with a `notice` that the port will
  be retired. The default is `wss://wspap.okx.com:443/ws/v5/private?brokerId=9999`, and the login and the `orders`
  subscription are accepted there.
- **What that buys, measured against the venue**: `logins=1 subscribes=1 reconnects=0`, no error, no unreadable
  message, and not one order sent -- the adapter is not wired into the app yet, so this proves the private route and
  not the order leg. The acceptance run in ADR 0005's amendment is still ahead.

## The volatility floor, measured before it was built (2026-11-13)

- **A slice driver, because the bucket join had nothing to join.** `tools/study/slice_pnl.py` cuts a day's depth
  and trade files into slices of a fixed length in one pass and writes the per-slice result beside the per-slice
  true range on the same `window_start`. The pass is the point: a snapshot slice is about 130 MB, so the driver cuts
  a slice, replays it, deletes it and moves on rather than scanning five million rows once per slice, and a slice
  with no trade is never written at all. It replays with `--paper-broker` so a slice that does not reconcile is
  `recon=bad` instead of silently kept, counts a slice with no trade rather than replaying it, counts a trade with
  no book row as `no_depth` rather than losing it, and refuses to turn a failed replay into a row of zeros. Its
  self-test runs a fake binary, and the kept-slice case pins that the empty half hour has no file.
- **The answer is no, and it is a measurement rather than a preference.** Three cuts: five Deribit days at half an
  hour a slice (240 slices, 5,549 fills), the same two of those days at ten minutes (288 slices, 3,135 fills), and
  the two at half an hour again (96 slices, 3,131 fills). Every slice reconciles, and every cut splits at its own
  median range into two halves that both lose money a fill; the quiet half's loss is 0.66 to 0.82 of the busy
  half's in all three. In the five-day cut that is 0.00410 USD a fill (121 windows) against 0.00515 (119), and at
  ten minutes 0.00295 (144) against 0.00447 (144). The quiet minutes are not the ones the fee eats, so a floor would
  remove the cheaper ones; no floor is built, and `active_l2_market_maker.h` now says so in the comment that used to
  call the window a known gap. Measured, not gated: the five-day run is nine minutes and the ten-minute one four,
  so the command lives in the tool and the numbers here and in the commit.
- **Two bugs the join caught, and why the gate has them now.** The first run reported `recon=bad` on every slice
  because the replay tuple had no `--paper-broker`, so no `[RECON]` line existed to read. The first series keyed
  every slice on 0 because the sibling tool takes its window in seconds and the driver carried microseconds -- the
  join failed loudly instead of producing a table, and the self-test now asserts the series keys and that the
  per-slice ranges equal one pass over the whole file.
- **Two holes a review found, and closed before the numbers were quoted.** An empty slice used to be cut to disk
  and deleted, which cost 130 MB a half hour for nothing: the writer is opened lazily now. And a slice with trades
  but no book rows was silently missing from the table, which would have made the sum of the slices quietly smaller
  than the day: it is counted as `no_depth`, and both are pinned in the self-test.

## The tools for a parameter that has to travel (2026-11-13)

- **A walk-forward over the sweeps that already exist.** `tools/study/walk_forward.py` fits a swept parameter on
  every window up to a fold and scores the chosen cell on the fold's own window, so the number a document quotes has
  not seen its own answer. It reads the CSVs `tools/sweep/*.sh` already write -- one per window, in the order they
  happened -- states its selection rule (highest mean over the training windows, ties to the first tuple in the
  file), reports `replicated=yes` only when every fold's out-of-sample number is positive, counts an unreadable cell
  as `missing` rather than as zero, and refuses a single window because one window has nothing to be out of sample.
- **A windowed true range, for the volatility gate that could not be one.** `tools/study/volatility_window.py` cuts
  a trade file into fixed windows and prints each window's high minus low in ticks: the "tick range over a fixed time
  window" that `src/strategy/active_l2_market_maker.h` names as the thing its per-second volatility halt cannot be.
  Both averages are printed -- over every window and over the windows with at least two prints, since a window with
  one print has a range of zero -- with `singles=` saying how many those were. Its `--bucket` mode joins that series
  to a per-window results table and prints the two sides of the median, their counts and their means, and no verdict:
  the split is printed so a reader can see where it fell, and whether a difference is a result is the document's
  question.
- **Both run in the gate.** `verify_l2.sh`'s tool check runs their `--self-test`s, which pin the fold chosen on the
  past, the choice that travelled, the missing cell, the tie rule of the median split and each tool's refusals. A
  first walk-forward over the two Deribit windows (2020-04-01 chosen on, 2020-06-01 scored on) prints
  `replicated=no` with the chosen cell negative out of sample, which is the grid's own result arriving by the route
  that can be repeated rather than by a re-reading.
- **What did not change**: no C++ file, no test binary, no recorded number, and the same 120 source files and 26,201
  source lines. The tools read `data/` and write under `/tmp`; a missing input is an error naming the file.

## The relay's private route (2026-11-13)

- **The order channel gets the backend the live leg needs.** `tools/live/venue_relay.py` serves `--order-listen` from
  either backend now. `--simulate-orders` is unchanged; without it the relay dials the venue's private websocket, sends
  a login its own code signs (`timestamp + "GET" + /users/self/verify`, the base64 of an HMAC-SHA256, with Unix epoch
  seconds as the timestamp -- corrected on the first sandbox contact, below), waits for the acknowledgement, subscribes
  `orders`, and then translates in both directions through the same
  `OrderTranslator` the simulation uses -- so the adapter, its transport and the four replies are untouched. A drop is a
  new session: the client logs in and subscribes again, and the summary printed when the engine hangs up counts
  `logins`, `subscribes`, `reconnects`, `messages`, `other_instruments`, `unreadable` and `write_failures`. A request
  whose write failed is counted rather than re-sent, because an order sent twice is worse than one nobody answered.
- **The credentials are read, never printed.** `VenueCredentials` reads `TOYQUANT_API_KEY`, `TOYQUANT_API_SECRET` and
  `TOYQUANT_PASSPHRASE` from the environment, masks all three in its `repr`, and refuses naming the variable that is
  missing -- which is also what `--order-listen` now says when it has neither a simulation nor a credential.
  `--order-venue` overrides the endpoint; the default is the venue's demo environment, and no run here has reached it,
  so a first sandbox run is what confirms it.
- **The self-test is in the gate, and it checks the signature against something outside the tool.** One pinned vector
  (secret, timestamp and expected base64 computed once and pasted in) and one local stand-in venue -- a real socket, a
  real handshake, the login verified from the bytes that arrived, one deliberate drop after the subscription, then an
  order and a cancel answered in the venue's shapes. `tests/relay_order_channel_test` still covers the simulated
  backend across two processes; this covers the private route with no key and no endpoint. Three mutations were run to
  show the checks fire rather than pass: a wrong preimage, a wrong signature on the wire and no drop each failed it.
- **The acceptance criterion moved, and the record says so.** ADR 0005 gained an amendment: the session's order leg
  replays on the recorded clock, because `RecordedVenue` answers on submission and a live fill does not, so the verbatim
  comparison of `[PNL]` and `[EXECUTION]` was never reachable as written; the ledger-only reading is recorded as the
  option that was not chosen. Nothing in `src/` changed, so no count and no recorded number moved.

## The live-venue step, the second half (2026-11-12)

- **The relay's order channel, and the first proof that the two halves talk.** `tools/live/venue_relay.py` now serves the
  order leg as well as the market data: it accepts the adapter's lines, translates them into the venue's order messages,
  and translates the venue's answers back into the four replies the adapter decodes -- ack, fill, cancel and reject.
  `--simulate-orders` answers as a sandbox venue, deterministically: half the order fills at its own price, anything above
  `--order-cap` is refused, a cancel ends the order. `tests/relay_order_channel_test.cpp` starts that relay as a child
  process, connects the real `TcpVenueTransport` and the real `LiveVenue` to it, and pins the round trip across two
  processes and one translation: the acknowledgement, the fill, the refusal of an oversized order, the user cancel, and
  the counters on the engine's side against what crossed. The relay's own `--self-test` pins the translation's shape
  field by field, including six lines it has to refuse. What is deliberately absent is the route to a venue's private
  channel: it needs a key and a login handshake, so `--order-listen` without `--simulate-orders` is refused rather than
  accepted and ignored.
- **The order leg written down and read back.** `src/lines/order_recorder.{h,cpp}` writes `<prefix>_orders.csv` -- `seq,
  kind, order_id, symbol, side, price, qty, ts, exec_type, cancel_reason, reject_reason, liquidity_role, fee, owner, tif`
  -- one row per submission, per cancel and per execution report, a price as the shortest decimal that parses back to the
  same double, and a field holding the comma the format splits on refused rather than escaped.
  `src/lines/recorded_venue.{h,cpp}` is the pair: `RecordingEngine` decorates a venue and records what it was asked and
  what it answered, `RecordedVenue` is an `IMatchingEngine` that answers from the file instead. `tests/order_recorder_test`
  pins the header and two rows literally, the round trip -- the replayed reports are the recorded ones field for field, a
  rebate's negative fee included -- the `rows_left` count that says a replay stopped short, and four refusals, each of
  which names the file and the line. `--record-orders=PREFIX` is the flag; it writes `[ORDER-LOG]` with its path and its
  three counts, and it is refused on a mode with no venue and on a two-book run, because a recording is one file per
  venue.
- **What did not move.** No core file changed, no recorded run re-derived differently, and no number in the documents
  moved except the counts they state -- 35 test binaries, and the source lines this step added. The reader is tested and
  the app has no mode that replays an order leg yet: that is the next step, and it needs a recorded session from the
  sandbox to replay against.

## The live-venue step (2026-11-11)

- **A venue adapter that no socket can see yet.** `src/lines/live_venue.{h,cpp}` is the third implementation of
  `IMatchingEngine` -- the interface `PaperBroker` and `OrderGateway` already implement -- and it takes its wire as an
  injected `IVenueTransport`, so the adapter knows nothing about sockets. An order becomes one frame of its own protocol,
  the adapter refuses past its own order count or size bound before anything is written, a wire that fails is counted as a
  failure rather than a sent order, and the four replies a venue can send (ack, fill, cancel, reject) become the
  `ExecutionReport`s the ledger already consumes. `tests/live_venue_test.cpp` pins all of it with a recorder in place of a
  socket: 33 test binaries now, and no recorded number moved.
- **The wire is real now, and still local.** `src/lines/tcp_venue_transport.{h,cpp}` is an `IVenueTransport` over TCP: one
  frame per line out, the venue's replies split on newlines on a receive thread, and a connection that has gone away
  reported rather than written into. `tests/tcp_venue_transport_test.cpp` puts the adapter on loopback against a listener
  the test opens on an ephemeral port -- no endpoint, no credential, no market -- and pins the frame that crosses, the ack
  and the fill that come back, the counters at both ends, and a refused write after the venue hangs up.
- **It is still not wired into a run.** There is no flag and no venue endpoint yet, which is deliberate: ADR 0005 puts the
  shape before the wire. What remains inside the step is the relay's order channel -- the relay speaks WebSocket and this
  transport speaks lines -- then the venue's sandbox.


## The evidence milestone (current)

The project stopped asking whether the instrument could be built and started using it to answer. Eight markets were
closed by measured quotients — crypto perpetuals, the micro E-mini, SPY, US large caps, options at two tenors, a funding
carry and a low-turnover rule — and the machinery that keeps those answers honest became the deliverable: a four-tier
gate that re-runs the command behind every quoted number, prose lints that re-count what a paragraph states, and a link
checker over three tiers of documents.

### Highlights

- **The gate is what makes a number a claim.** Tier 0 runs the unit tests, tier 1 the L2 invariants and 53 recorded
  runs, tier 2 hashes 1.5 GB of standard output against a stored reference, tier 3 repeats the tests under the
  sanitizers. A documented figure that moves fails the build, which is why the documents can be trusted by a reader who
  has not run anything.
- **Eight closures, each with its binding constraint recorded as data.** `tests/fixtures/verdicts.json` carries the
  inputs and the gate each line is recorded as binding on, and `tools/study/viability.py` re-derives every quotient on
  every gate run, so a copied number cannot drift.
- **An interval on a mean, and the range next to it.** `tools/study/pnl_interval.py` resamples a series and reports the
  interval, the median, the spread and a thin-sample flag; applied to five monthly windows it showed the interval around
  a mean is narrower than the spread the draws themselves have, which is what stops a single window being read as a
  result.
- **A kill switch that pays for its own exit, and a floor that is now priced.** A latched switch closes what the book
  holds and reports what that cost — 5.39 bps, fee plus half a tick — which is the same number a crossing pays, arrived
  at from the other end of the trade.
- **Three tiers of documents, and a map.** Twelve documents at one level became nine files and three directories: the
  answer and the argument, the reference, and the evidence, with a table that says what each document owns and what it
  must not contain.

## The live paper milestone

The websocket reader, the paper mode and the recorder half of a live path, which is roadmap phase 2: the
seam it closes is the one the roadmap names, that a live feed is another reader and paper trading is the
existing book and matching engine against it. So the live path is the L2 replay path with its two readers
moved onto a socket, and everything downstream of them is the same code. Its entry point is the `ws` mode in
[User Guide](docs/USER_GUIDE.md), and the numbers the other documents quote are re-derived by
`bash tools/gate/verify_l2.sh fast`, which this milestone leaves unchanged.

### Highlights

- **A websocket feed behind the reader interface.** `src/market/ws_feed.*` speaks RFC 6455 -- the handshake
  and text frames -- with no external dependency, and implements `IMarketEventReader` as two readers over one
  connection, so the book, the strategy, the engine, the recorder and the paper broker cannot tell a live
  session from a replayed one.
- **The wire is the replay format.** One CSV row per text message, its kind read from its column count, in the
  two shapes `--record` writes, so a recording streamed back over the feed reproduces itself byte for byte.
- **A desk loop with no credential.** `tools/live/websocket_sender.py` stands a WebSocket server in for a venue and
  streams a trade and a depth file, interleaved by their own timestamps, so the whole path runs locally. A
  unit test closes the same loop inside `ctest`, with a server thread standing in for the venue.
- **One mode, reused.** `ws` accepts every `l2_replay` flag -- the recorder, the feed thread, the profiler and
  the paper broker -- because it is that mode with the readers swapped, and `run_l2_replay_mode` is now
  `run_l2_session_mode`, which owns both sources.
- **A bounded session and a bounded handshake.** `--ws-seconds=N` ends a capture whose peer never closes the
  socket, draining what already arrived first, and the connect and the opening handshake share a five-second
  budget, so a host that accepts the connection and then says nothing fails the run rather than hanging it.
- **A relay that survives an idle socket and a slow reader.** Both heartbeat forms are answered -- a text
  `ping` and a control ping -- which a six second run never exercises; the hand-off to the engine is bounded,
  so a slow reader slows the venue down instead of growing the process, and the wait and its high-water mark
  are printed with the relay's summary. The feed reports its own `ring_waits`, and both recorders are flushed
  as they go, so a stopped session loses at most the last few thousand rows. Two test hooks make the two
  paths executable against a venue: `--ping-seconds=N` sends a ping on a fixed cadence even while data
  flows, and `--drop-after-seconds=N` closes and reopens the venue connection.
- **A thirty-minute live soak that replays into itself.** OKX's public `BTC-USDT-SWAP` channel, the paper
  broker and both recorders for 1,800 seconds: 697,400 frames, 6,719 trades, 690,679 depth updates, with no
  sequence gap, no reconnect, no parse error and no ring wait, flat RSS in both processes, and a replay of the
  engine's recording that reproduces the live summary field for field. The heartbeat path stayed idle, so it
  is covered by a local stand-in rather than claimed against the venue.
- **A live merge that cannot deadlock on a trade gap.** A two-hour run with a forced reconnect every 25
  minutes stopped at the second drop: the merge waits for the next trade, depth keeps arriving, and the
  4,096-row depth ring (10.7 s at the measured 384 rows/s) filled while the receive thread blocked on a push
  the merge could not drain. The live input rings are now 131,072 rows, about 5.7 minutes of depth, and the
  feed reports `ring_high_water`. The structural fix -- one ordered stream instead of two blocking readers --
  is recorded in the roadmap.
- **The two-hour rerun, with the whole live path exercised.** 5,704,944 frames, 144,755 trades, 5,560,187 depth
  updates, four forced reconnects with zero sequence gaps, 355 pings answered by 355 pongs, no ring wait, a
  ring high-water of 3,917 against the old 4,096 capacity, five snapshot batches (the initial one and one per
  reconnect), a reconciliation that stayed `ok`, and a replay of the engine's recording that reproduced the
  live summary field for field.
- **One ordered stream on the live path.** `WebSocketFeed` now has one input ring and one reader: the receive
  thread assembles a run of depth rows that share a local timestamp, holds a trade that arrives during the
  run until the batch ends, and pushes both in that order. A trade gap can no longer stall depth delivery,
  which is the deadlock the two-ring merge reached; the ring is sized at 131,072 rows and reports
  `ring_high_water`. `--ws-ring-capacity=N` sizes it and `--idle-wait-us=N` trades a little latency for a core.
- **The edges around it.** The relay leaves subscription acknowledgements out of a capture, resets its backoff
  after a session that actually lasted, and the C++ twin gains `--ping-seconds` and `--drop-after-seconds`; the
  recorder flushes every 4,096 events; and a quote scaled below the instrument's minimum is raised to it, so
  the engine stops refusing the strategy's own size.
- **The client's half of the order path.** `OrderGateway` (`src/lines/order_gateway.*`) is a second decorator
  over the engine, wrapped outside the broker: it keeps the table of orders from submission to answer, absorbs
  a retry under the same client id, counts an order the venue has not answered within a timeout of market
  time, and re-sends it under the same id where the venue's duplicate check makes the retry safe. Reported as
  `[GATEWAY]`; with every knob at zero the run reports the numbers of the same run without it, and with a
  timeout and a retry the run reports the numbers of the same run with neither.
- **A participation cap that is only a capacity cap.** `--participation-limit=X` caps one aggressive
  external-BBO fill at X of the displayed best size. The fill price stays the touch and the remainder follows
  the order's time in force, so the simulator does not acquire a price-impact model or a new calibration.
- **IOC and FOK as order semantics, not strategy hacks.** `TimeInForce` lives on `StrategyOrder` and
  `exchange::Order`; the matcher returns its unfilled quantity, and the engine cancels an immediate remainder
  with `CancelReason::Ioc`. A FOK pre-checks external plus internal capacity, including the participation cap
  and self-trade stopping, then fills completely or cancels with `CancelReason::Fok` without a print.
  `--time-in-force=gtc|ioc|fok` applies it to a run; `tests/matching_engine_test.cpp` covers the cap, IOC,
  FOK-unfilled, FOK-filled and the split external/internal FOK. GTC remains the default, so existing runs are
  byte-for-byte unchanged.
- **A three-command demo path.** The User Guide's "A short demo path" runs `verify_l2.sh fast`,
  `risk_check.sh` and a one-cell `trend_sweep.sh` in the order their prerequisites require, with the exact
  lines to expect. It adds no strategy or script: the negative trend PnL is presented as the measured result
  rather than a failure, and the gate, the risk invariants and the cost of a taker signal are all visible in
  one sitting.
- **A smaller matching core.** The buy and sell sides now share one side-generic matcher, and the real
  external-BBO fill and the FOK pre-check share one touch predicate and one participation cap. The pipeline
  also has one `submit_order` step instead of a ninety-line loop body. This is a behavior-preserving
  refactor: the buy/sell report order, queue counters and all pinned run outputs are unchanged, and the gate
  stays green.
- **A named report path and a smaller order path.** The `Pipeline` constructor now installs a callback that
  calls `handle_report()` instead of holding a hundred-line lambda, and quote-distance statistics moved to
  `record_quote_observation()`, so generic order submission no longer carries market-making metrics. The
  report fan-out still runs in the same order and every pinned output is unchanged.
- **Market events named at the boundary.** `Pipeline::process_event` now dispatches through four named
  `handle_event` overloads instead of an `if constexpr` chain inside one visitor. Trade and BBO handling stay
  byte-for-byte the same, while a future non-MM event or strategy path has a named place to land.
- **Fill accounting as its own step.** `handle_report()` now keeps the report order and delegates one of
  our fills to `apply_market_maker_fill()`, which owns position, reservation, exposure, edge and markout
  updates. A future non-MM path can replace that fill accounting without copying the ledger and strategy
  callbacks.
- **Fill state in one component.** `FillAccounting` now owns position, position-gate reservations, maker
  notional, exposure and the trade counters; `Pipeline` reads it through accessors and keeps only the
  config inputs that also feed the report and engine. This removes eight mutable fields from the
  coordinator and gives a non-MM path one boundary to replace.
- **One event clock and one settlement step.** The L1, L2-view and L2-trade paths now share
  `begin_event()` (funding plus the event clock) and `advance_mid()` (midpoint, quote cycle and the
  five-cycle markout drain), and queue consumption is reported through `record_queue_activity()`. A
  venue report and a submission refusal settle through the same `settle_order()` call, so the
  minimum-commission charge cannot be forgotten on one of them. The helper bodies are the code they
  replaced, in the same order; no reported field changes.
- **A strategy that is not a market maker.** `trend_l2` is the other workflow on the same pipeline: an EWMA
  cross sets a target position, clips at the touch fill it, and a stop, a time stop or the exit band closes
  it. A cooldown after a stop keeps the next view from reopening the same position. It shares the pipeline's
  pre-trade gates instead of reimplementing them; on the 200k-row Deribit slice it filled 55 taker clips on
  both sides and reconciled `ok`. `--trend-params` exposes the signal, and `tools/sweep/trend_sweep.sh` replays a
  grid of it and writes the fills, PnL, fees and risk pauses to a CSV.
- **A pre-trade risk pipeline, and the measurement that keeps it honest.** The kill switch, the exposure cap
  and the position gate moved out of `submit_strategy_actions` into `RiskPipeline`, an ordered list of five
  checks; the two new ones are a fat-finger band around the midpoint and a token bucket on submissions. A
  run that names none of them is byte-for-byte the run it was, and a permissive setting leaves `[EXECUTION]`
  identical. The first measurement refused 161 and 33,685 orders; the second number was a retry storm, because
  a refusal was retried on the next view. `on_order_rejected` plus an exponential backoff in `trend_l2` (60
  views, doubling to 3,840) cut those to 13 and 15. A venue or broker refusal at submission takes the same
  path, in addition to the cancelled report that removes the order from the open set. The rate bucket, the
  exposure cap and the position gate also let a reducing order through, so a position that is too large can be
  worked down instead of trapped. `tools/sweep/risk_check.sh` holds the four runs.
- **A timeout is a query, not a fake cancel.** `OrderQuery` lets the gateway ask the venue whether it still
  holds an order that has not answered; `[GATEWAY]` reports `unacknowledged`, `timeouts_queried`,
  `timeouts_working` and `timeouts_unknown`. The ledger keeps the order `Pending` until a real report, because
  `Working` and `Unknown` are not a fill or a cancel. `tests/order_gateway_test.cpp` covers a timeout with no
  retry: the query is `Unknown` before the order lands, `Working` after it rests, and no terminal report is
  invented.
- **A terminal order state, including rejection.** `ExecType` gains `Rejected` and `RejectReason` (venue,
  broker, timeout, risk), and the pipeline, ledger and strategies share one `is_terminal` predicate: a
  terminal report releases the quantity the position gate reserved, closes the audit entry and removes the
  order from the strategy's open set. `Pipeline::handle_rejected_order` now reports a venue or broker refusal
  as `Rejected` rather than `Cancelled`, so a strategy can tell a refusal from a cancel it asked for;
  `[EXECUTION]` appends `audited_rejected_orders` only when there is one. `Cancelled` also carries a
  `CancelReason` (`user`, `venue`, `ioc`, `fok`, `self_trade`, `timeout`), so `trend_l2` backs off on a venue or
  self-trade cancel but re-quotes immediately after its own. The ledger now has an explicit state machine
  (Pending -> Resting -> PartiallyFilled -> Filled/Cancelled/Rejected); an illegal transition and a report for
  an already-closed audit are counted rather than applied, and `[EXECUTION]` prints those counters only when
  non-zero. `tests/order_state_test.cpp` checks that the next order is accepted after a refused one, which is
  the pending-reservation leak the old path could hide.
- **The account's equity series, behind a flag.** `--metrics` samples the marked equity once per
  60-second bucket at the point every event path already advances the midpoint, and `[PERFORMANCE]` reports
  the drawdown, how long the curve took to win its peak back, the annualized volatility, and the Sharpe,
  Sortino and Calmar ratios, scaled by the 525,600 buckets a 24/7 year holds. `Portfolio::equity_at` is the
  read-only valuation `mark_to_market` now shares, so a sample cannot move the unrealized PnL the kill switch
  reads in the middle of an event. Off by default, so every recorded line is byte for byte what it was.
- **The distribution behind the PnL.** `--metrics` also books every closing fill as a round trip: the side
  the position was opened on, its entry and exit price and time, the closing PnL, and what the trip cost —
  the exit fill's fee plus the share of the entry fees the position was carrying. `[ROUND_TRIPS]` reports the
  win rate, profit factor, expectancy, the average win and loss, the average holding time, and the aggressive
  and total notional against the starting collateral as turnover. A fill that closes part of a position and
  opens the rest splits its fee by quantity, so the sum of the trips plus the carried entry fees of the
  position still open is the run's realized PnL rather than a second opinion about it.
- **The distribution split by side.** `--metrics` also prints one `[ROUND_TRIPS_SIDE]` line per side that
  completed a round trip, with the same win rate, profit factor, expectancy, average win and loss and holding
  time. The aggregate hides which side a win came from: on the 2M slice one win in twelve is five longs with
  that single win at -0.022 USD a trade, and seven shorts with none at -0.010. The account-level notional and
  turnover stay on the aggregate line, because a fill belongs to the account rather than to the side of the
  position it closed.
- **An execution line with no view on direction.** `execution_l2` works one parent order from
  `--parent=SIDE:QUANTITY:SECONDS` with `--algo=market|twap:N|pov:X`, and the pipeline measures the parent
  itself rather than reading the strategy back: `[EXECUTION_ALGO]` reports what filled and the three terms of
  implementation shortfall in basis points of the arrival notional. The first measurement is a negative result
  about the *schedule*: on two non-adjacent windows no schedule beat a single crossing by 2 bps, and `twap`'s
  signed slippage flipped from +11.7 to -9.8 bps while the fee stayed at 4.6 to 5.0. The fee is the only term
  with a stable sign, which is what the maker-entry question would move and the schedule does not.
  `[EXECUTION_ALGO]` also reports the window's time-weighted midpoint, and against that benchmark `twap` lands
  within three basis points on both days while the single crossing's distance from it is the drift itself --
  which is what turns "the two windows disagree" into "the single crossing is a ten basis point bet, and the
  schedule pays a stable half point".
- **Posting instead of crossing, measured and rejected.** `--algo-entry=post:N` rests each slice at the near
  touch and crosses what it has not filled after `N` seconds. On two windows and four timeouts the fee does fall
  with patience (5.00 → 3.40 → 1.80 bps) but the units acquired fall with it (100 % → 60 %) and the opportunity
  cost rises faster (0.00 → 10.85 bps against the benchmark), so no timeout beats crossing. The taker fee is a
  floor on this venue rather than a parameter, and the remaining lever is a venue whose maker and taker rates
  differ by less than the fill risk costs.
- **Capacity, measured and flat.** A ladder from 1,000 to 100,000 contracts on both windows: the shortfall in
  basis points does not move at all, because every term is proportional to quantity, and what size changes is
  how many crossings the order needs (one on the deep day, six on the thin one, thirty-four to ninety-seven for
  the time-sliced schedule). No size flips the ranking, so "a parent larger than the display" stops being an
  open question rather than becoming a favourable one.
- **What the crossing side cost.** `--metrics` records the midpoint each order decided at and reports
  `[TAKER]`: the arrival slippage of every aggressive fill in ticks, basis points and dollars, the drift from
  the fill to the midpoint 5, 20 and 60 quote cycles later — positive when the move covered the spread, the
  opposite sign of the passive `adverse_selection` — and the implementation shortfall of the IOC/FOK orders
  split into the filled part, the fees, and the opportunity cost of the quantity they never got. A passive
  quote we cancel is deliberately not one of those orders: cancelling declines a fill rather than missing
  one. On the 200k-row slice with `trend_l2` under IOC, 55 crossing fills paid 1.53 ticks of arrival slippage
  and the drift never recovered it: -1.41, -1.55 and -1.44 ticks at 5, 20 and 60 cycles. `--markout-horizon=N`
  makes the cycle count behind the passive markout and the opportunity term a parameter; the default stays 5,
  so every recorded run reads the same.
- **The tick curve, drawn in five points instead of inferred from three.** The crossing floor is the fee plus half
  a tick, and the futures venue quotes every symbol on one 4 bps schedule while spacing its ticks by a factor of ten
  thousand, so it is where the curve can be measured rather than argued: `BTCUSDT` 0.014 bps of tick, `DOGEUSDT`
  0.52, `XRPUSDT` 1.63, against the two perpetuals at 0.78 and 3.76. Four of the five land on half a tick to three
  decimals. The fifth is the finding: `DOGEUSDT` pays 1.35 bps, five ticks, because its best ask holds thirty-six
  DOGE -- seven dollars -- so the parent walked the ladder in forty-five children, and shrinking it to seventy
  dollars still cost 0.73. The formula is `fee + max(half a tick, impact)`, and the depth term is the whole cost on
  a book whose touch is smaller than the venue's own minimum notional would suggest. That also re-reads the flat
  capacity ladder: it was flat because that book is deep, not because size never matters.
- **The maker hint, settled on a venue whose tick is negligible -- and an earlier claim of mine narrowed.** The
  BTC/ETH gap of 6.9 bps could not be attributed with 1 to 86 fills per window. The venue whose order book archive is
  free and daily from March 2023 offers 115 to 246 fills per window instead, and it also removes the variable under
  test: its BTC and ETH ticks are 0.015 and 0.033 bps of price, both a rounding error beside the 2 bps maker fee.
  There the two symbols are indistinguishable -- a gap of +0.10 bps across eight windows, with the two daily means on
  opposite sides of zero -- so the difference **cannot be the asset**: the venue that removed the tick removed the
  gap. What I first read as a measured gap on the coarse-tick venue (+7.44 bps over two days) does not survive four
  days: the daily gaps are -3.27, +8.14, -0.27 and +6.74, a standard error of 2.7 bps, so the sign and size there is
  what a few hundred fills cannot pin. The line stays closed either way: the fine-tick venue's gross is negative in
  all eight windows (-0.50 to -1.87 bps, before the fee) and the coarse-tick venue's ETH contract is negative on all
  four of its days. Frequency turned out not to be a knob at all: the same strategy holds a round trip 0.03 to 0.14
  seconds where the tick is 0.015 bps and 60 to 720 seconds where it is 0.78.
- **The dated basis, which the funding differential was mistaken for.** The twelve-month measurement that closed the
  carry line compared two venues' *perpetual funding*, where the differential flips sign inside a day. A dated future
  is a different animal: it carries a fixed gap to spot that must close at delivery. Six monthly samples of Deribit's
  free first days price 19 dated legs and 22 calendar spreads. The dated legs average **+252 bps annualised, 17 of 19
  positive**, and fail the registered gates on one count -- the near contract inverted twice, at -186 bps annualised
  in June as BTC fell from 77,889 to 72,043, which is the crash regime the BIS carry paper warns about. The calendar
  spreads, which need no spot at all, are **positive in 22 of 22 samples with a mean of +399 and a worst of +67 bps
  annualised** -- and that is recorded as a hypothesis rather than a result, because the gates were registered on the
  dated legs and swapping instruments after seeing both is how a sample gets picked to fit. The open limits are the
  ones a VWAP cannot answer: the dated books trade a hundredth of the perpetual's activity, so the spread paid to
  enter is unmeasured, and the six samples are one bull regime whose premium compressed 10x in July.
- **The revival test: can better execution rescue a negative edge? No.** The closing claim that execution cost is the
  one lever that does not decay was tested by asking whether any *quoting policy* on the fine-tick venue earns before
  the fee: nine combinations of width (2, 4, 8 ticks) and patience (refresh every 1, 4, 16 price moves), both symbols,
  same 200k windows. **Every cell is negative.** BTC improves monotonically with width and patience, from -1.39 to
  **-0.21 bps**, and is still nineteen basis points short of the 2 bps fee; ETH does not improve at all, sitting at
  -1.0 to -1.2 bps in all nine. So the difference execution can make -- 10 to 20 bps a round trip -- multiplies an edge
  and does not create one, and the loss here is in the *fill*, not in the plumbing: it is adverse selection, and
  execution does not select.
- **Funding extremes as an event signal: a lottery with a story attached.** The last unmeasured class of information was
  the funding rate read as a crowd indicator -- when longs pay heavily, sell the perpetual and collect both the funding
  and the squeeze. The rule was registered before the numbers: a z-score of the rate against ninety days, a short above
  it and a long below, a hold from one settlement to the next, 8 bps of taker crossings, and three gates. Over 3,863
  usable settlements of BTCUSDT **all three gates fail at every threshold**, and the failure has a shape worth keeping:
  the **mean is positive and the median negative** at 1.5 and 2 sigma (at 2 sigma: +3.36 against -4.81 bps, 119 of 251
  trades positive), and at 3 sigma the mean is **-1.03**. The whole positive mean is **one year** -- 2022 contributes
  +136 bps while the other six are a scatter between -18 and +31. The held-out period says the same thing more plainly:
  **2024-2026 averages -11.63 bps** over 117 trades. A rule that pays in the crash you cannot foresee and loses in the
  six years you can is not an edge, it is a bet on regime.
- **That measurement's first version was wrong, and wrong in the flattering direction.** It priced the entry from the
  close of the hour that *starts* at the settlement and credited the position with that same settlement's funding, so a
  trade that began an hour late was paid for a settlement it never held through -- and the rule selects exactly the
  settlements whose rate is largest. Aligning the entry to the settlement instant and crediting the next settlement,
  which is the one the hold spans, moved the two-sigma mean from +7.29 to +3.36 bps and the held-out mean from -1.06 to
  -11.63. The verdict survived; the number had been paying itself.
- **Selling options: measured three times, and only the third one counts.** The first pass priced the straddle from an
  at-the-money approximation on strikes up to 6.4 % away from spot and called the variance premium negative on ten
  samples. The audit split those by moneyness and read +19.9 points on the four that were genuinely at the money --
  which is the same error one level down. The measurement that counts chose the strike as the nearest 100, inverted
  Black-Scholes per leg, took the nearest listed expiry at least twenty days out, and covered **92 straddles from
  2020-01 to 2026-09**: implied **58.4 %** against realised **58.1 %**, a premium of **+0.2 volatility points**, and a
  short straddle that is a coin flip -- **51 of 92 positive, mean -105 USD, median +319, left tail -25,450 against a
  best of +8,367**. Implied exceeds realised in 63 % of months and the seller is still not paid on average, which is
  what a risk premium priced at fair value looks like. Two years pay, five do not.
- **The queue model on a US equity book, and the fee term that is actually a policy choice.** `conservative` moves a
  queue only when the level trades; the tape says that is a model of a book where nobody cancels: on AAPL and on AMD
  **5.0 and 4.0 percent of the units leaving a price level leave by trading and 95 or 96 percent are cancelled**, so
  `prorata` is the default this venue supports. And the F4 sentence "the model decides the sign as well" is true
  against the commission and not against the account's schedule: at **0.94 to 1.00 ticks a fill** the four models
  fill 2,294 to 4,775 times and capture 0.655 to 1.073 ticks a unit, and **all four lose money**, 2,026 to 2,207 USD
  over the same two hours. What the policy owns is the **minimum per order**, 0.354 to 0.418 of the fee: a twentyfold
  clip dilutes it to 0.270 but never to zero, because its numerator is the number of order instances rather than
  their size, and slowing the requote from every one price move to every sixteen cuts it to 0.187 while the capture
  falls by exactly as much -- capture plus markout stays at 0.78, 0.77, 0.75, 0.74 across four refresh settings.
  Reproduced by `tools/sweep/equity_schedule_sweep.sh`, and recorded with the caveat that the tick fields and
  `closed_pnl_usd` do not reconcile.
- **The maker credit is worth 141 USD a window, and the column that decides the line does not move.** Four schedules on
  the same fills: the commission alone charges 380.33 USD, the account's tiered terms without the maker credit 1,228.80,
  with it 1,087.53, and the same with no minimum per order 632.77 -- while the **gross closed P&L is -1,119.04 USD, or
  -1.03 ticks a unit, in all four rows**. So the best terms any of those arrangements offers (0.582 ticks a unit) still
  loses 1,746.86 USD on the window, and a fee of zero would still lose 1,119.04. **The quoting is negative before the
  fee**, which is the sentence the OKX maker line produced at -0.64 and -0.99 bps and the reason no rebate arrangement
  can close this line. It also reconciles what the previous bullet left open: `net_pnl_usd` is the gross closed P&L
  less `fees_paid` to the cent, and the tick decomposition does not add to the gross because a window's closes include
  what a five-cycle markout cannot see.
- **A broker's volume tier is a schedule term, and on a quoting strategy the fee floor takes it back.** The
  schedule gained a sixth key, `commission`, so a run can be priced at the tier the account is actually on
  rather than at the venue registry's rate; the screen tool takes the same key, so one string prices the screen
  and the replay. Measured on the AAPL window at 0.0035, 0.0020 and 0.0015 a share: **the total fee is identical
  to the cent at all three**, 1.001 ticks a unit and the same net, because a quote's order fills about 24 shares
  at a time and pays a quarter of its commission against the whole 0.35 floor. The tier is a 43 percent cut of
  the commission line and a **zero percent** cut of the bill for a quoter; it pays in full only where orders fill
  in full. Raising the clip to where the floor would stop binding at a complete fill buys about 15 percent.
- **The tier and the clip, measured against each other: the fee is smallest where the money is worst.** Seven runs
  of the same window across three tiers and four clips: at a clip of 100 the tiers are **indistinguishable** (1.001
  ticks a unit, the same net to the cent), at the clips a complete fill would need for the floor to stop binding
  (175 and 234 shares) the fee falls to 0.881 and 0.847 while the gross closed P&L falls 45 and 107 percent, and at a
  clip of 1,000 the fee is best at 0.766 and the net is worst at -8,798, because the clip that buys a smaller fee buys
  six times the inventory. **A quoter that optimises its fee is optimising the wrong column.**
- **The screen now prices breadth.** `tools/study/screen_spread.py` takes a capital and prints, per symbol, the round trip
  in basis points at 20 names and the widest book that stays under a stated ceiling, using the capacity arithmetic
  rather than a copy of it: AAPL at 50,000 USD and a three-tick trade spread is 3.79 bps at 20 names and 50 names
  wide under 10 bps.
- **The book reports what it is exposed to, gross against net.** `[EXPOSURE]` prints one
  `SYMBOL:quantity:notional` per leg, sorted by symbol, then the sum of the sizes, the size of the sum, their ratio
  and the fees paid, valued at the last quote each symbol showed and read without moving the account. The tag is not
  `[PORTFOLIO]` because the account's own line already carries that one, and two different lines behind one tag is
  a grep that lies. It is the
  first half of roadmap item 9 (multi-instrument, portfolio and netting): the account has keyed positions by symbol
  since the carry line needed it, so what was missing was the read that a single position cannot answer — **gross is
  what the fees are charged on and net is what the position risk is** — and a two-leg book that nearly cancels now
  states a gross of twenty-one times its net in the unit test rather than in a paragraph. A closed leg is not an
  exposure and an unpriceable one is still reported with its quantity. The pipeline half, a run that trades more
  than one book at once, is still one engine and one strategy per run and remains the next slice.
- **A capacity table, so breadth is priced before it is chosen.** `tools/study/capacity_table.py` turns capital,
  name count, share price, spread and tier into round-trip basis points and a year of drag, and prints the clip
  at which the floor stops binding -- `0.35` divided by the rate, which is 100 shares at tier 1, 175 at tier 2 and
  234 at tier 3, so a book of that many names needs that many times the share price in capital before a single
  order pays its own commission. Its cells are checked against hand arithmetic in tier 1 of the gate.
- **Line nine is forfeited, and the reason recorded is the measured one.** The thirty mid-cap names were priced at
  0.165701 USD and not bought, and the equity quoting line is closed on two measurements rather than on resolve: the
  gross is negative before any fee, and the sixth gate takes the one positive window to nothing at one millisecond each
  way. Writing "a pass line was registered, so nothing was run" would have been a category error -- the line is what a
  run is judged against, and the 17 cents was the price of the run.
- **A round trip now reports what it paid.** `[ROUND_TRIPS]` gains `fees_usd`, `fees_per_round_trip_usd`
  and `fees_bps_of_turnover`: the fees booked to the closed round trips, their mean, and those fees over the notional
  the run traded. `expectancy_usd` is already net of them, so the comparison that decides whether a run can pay at all
  is now two adjacent fields -- fees per round trip against gross edge -- instead of a subtraction a reader had to do
  from `[PNL]`. On the 200k slice a single round trip reports 0.004 USD of fees against 30 USD traded, 1.33 bps, which
  is the maker rate the run paid. The ratio divides by the run's own notional and not by `quantity * price`: the latter
  is not a notional on a fixed-face-value contract, and the first version of this field made that mistake and printed
  0.003 bps where the answer was 1.33.
- **The execution line reports which fee rate it paid, not just how much.** `[EXECUTION_ALGO]` gains `maker_share`,
  `maker_fees_bps` and `taker_fees_bps`: the share of the parent's filled quantity that rested, and the fee charged
  on each role, whose sum is the existing `is_fees_bps`. Every venue here prices the two roles differently -- 1.5
  against 3.5 bps on a dated future, 2 against 5 on a swap -- and the whole difference between the post-first
  experiment's fee of 1.80 bps and a crossing's 5.00 came from this mix, so a fee without it is not interpretable. A
  role the venue does not report is charged to neither side and stays visible as the gap against `is_fees_bps`. The
  invariant is asserted in `pipeline_accounting_test`: the two halves add up to the total on a parent worked half by
  resting and half by crossing.
- **Lead-lag, the last shape that would pay for speed, priced and closed.** BTC prints first and the alts follow within
  a few hundred milliseconds -- the one structure where a fast client is paid for reading one symbol and trading
  another. One free day of Binance's USDT-perp tape, 2024-03-28, 5,664 BTC triggers at the top percentile of size, five
  followers: **every follower is within 0.06 bps of the control, and the control itself is 0.03 to 0.11**. A round trip
  costs nine basis points, so the largest number in the table is a hundred and fiftieth of what it would need to be.
  That closes the fifteenth structure, and with it the shape of the whole answer: what is paid per action does not clear
  the fee charged per action, and what is paid per unit of time does not clear the noise it arrives through. Two things
  survive and neither is a strategy -- execution cost (CME 1.2 bps a round trip against 4 to 24 in crypto, and resting
  instead of crossing saves 10 to 20 of it) and breadth (two hundred half-basis-point trades a day across twenty symbols
  is a question about infrastructure, not alpha).
- **Three candidates that would need speed, priced before any code was written.** Each is a place where being fast is
  worth money for a measurable reason, and each was measured from free data. **The CME calendar**, held long the near
  and short the far over 120 sessions, drifts **+0.54 bps a day (+64.6 in total, +11.4 a month) against a round trip of
  1.94 bps** -- the cost is trivial and the drift is real, but the daily standard deviation is **29.76 bps, 55 times the
  drift**, so a holder is paid half a basis point to sit through thirty. **Large aggressive prints**, the footprint of a
  forced seller, do show continuation -- **+0.71 bps a second after a big buy, -0.68 after a big sell, against -0.09 for
  a random trade** -- and it is **eight times too small** for a round trip that costs 8. **A stale quote on one venue**,
  measured on 40.2 million aligned quote states on 2024-03-01, shows an executable gap between Binance's linear perp and
  Deribit's inverse perp that is **positive 99 % of the day at a median of +2.99 bps** -- a level, not an opportunity,
  which is why the harvestable part is the deviation from that level and not the level itself. The deviation, measured
  against each minute's median over 3.1 million merged states: **median 0.00, p99 +2.39 bps, and above the 9 bps a
  crossing round trip costs for 0.012 % of the day -- 16 spells, median 63 ms, under a minute in total.**
  **Together the three say the same thing**: the CME slope drifts 11.4 bps a month against a 1.94 bps round trip but
  carries 30 bps of daily noise; a forced seller continues 0.7 bps against an 8 bps round trip; a stale quote deviates
  2.4 bps at the 99th percentile against 9. Speed decides which side of those numbers you are on, not what they are.
- **The calendar basis passed its gates as a level and fails as a return -- my error, and the correction.** Ninety-one
  monthly samples from 2019 to 2026 price the calendar spread at a mean **+696 bps annualised, 85 of 91 positive**, and
  against a measured round trip of 20.5 to 30.2 bps it clears the three registered gates seven times over. That was the
  first passing line in this repository and it was wrong: **a level is not a return**. Holding each pair one month and
  pricing the same two contracts a month later gives 60 realised observations -- **mean -6.7 bps a month gross, 28 of
  60 positive, worst -330, best +386, and -30.7 bps after one round trip**. The arithmetic is simple once written:
  what converges to spot at its expiry is a future against spot; the gap between two futures converges to *the gap at
  the near expiry*, which is not zero. A calendar spread bets on the **slope** of the curve, and the slope moved up to
  330 bps in a month here. What survives is the instrument the level stood in for: **one dated future against spot**,
  where convergence is contractual and the entry basis is the return -- +195 to +382 bps annualised on the six-sample
  run, and the venue itself publishes 1.5 bps to rest and 3.5 to cross on a dated future with 25x maximum leverage,
  which is a 4 % initial margin a leg. What is left to measure is the path, not the price: the June 2026 inversion at
  -186 bps annualised, the margin on both legs, and the spot leg's venue.

- **`--parent` on the best-bid-offer path, and the two things that path gets wrong.** The futures venue publishes
  percentage bands rather than a price ladder, so an execution run there reads the touch, which is all a crossing
  child needs. Two consequences. That path keeps the venue's millisecond event time where the book path carries
  microseconds, so the strategy's deadline, the measurement's deadline and the reported `span_seconds` are converted
  at the call site and the unit is named in the parent spec. And its touch callback carries no timestamp at all, so a
  time-sliced `--algo` would silently emit one child: the mode now refuses anything but `--algo=market` instead of
  half-running. The specification is also looked up by symbol rather than fixed to BTCUSDT, with the positional
  quantity scale as an override of the symbol's own lot step, so one unit is 0.001 BTC, 0.1 XRP or one DOGE.

## The measured line

The line that adds the queue model, the fee model, the two latency legs, the order trace and the queue
distribution, the strategy sweeps, and the four-tier gate. Its entry point is the
[Design Review](docs/DESIGN_REVIEW.md), and every number it quotes is re-derived by
`bash tools/gate/verify_l2.sh fast`.

### Highlights

- **Queue-aware fills.** Four queue models from a conservative lower bound to an optimistic upper bound,
  plus an arrival share, so a fill depends on the queue in front of a quote rather than on its price.
- **Fees charged on the instrument's face value**, after a model that charged price times quantity on an
  inverse contract overstated every fee by about 630 times and turned a strategy comparison into a
  measurement of the overcharge.
- **Two latency legs with opposite signs.** A late arrival quotes wider and fills less; a late cancel leaves
  a stale quote standing and is the leg that costs money.
- **Mechanism visibility:** a per-order trace, and a queue distribution over every order. On the mainline
  window the median quote never came within 3.9% of the front of its queue, and every order that reached the
  front traded.
- **Strategy sweeps:** quote width, requote threshold, order size and inventory limit, plus a four-strategy
  comparison with enough fills to read. The fee-to-edge ratio does not move across any of them.
- **A four-tier gate:** unit tests, pinned invariants and documented numbers, a full-file output hash, and an
  AddressSanitizer/UndefinedBehaviorSanitizer tier.

## v1.0-demo — the frozen demo

Tagged `v1.0-demo`, and kept for reproducing the earlier results and as a regression target. The simple
line: Binance Trade+BBO replay, legacy CSV and UDP inputs, the L1 strategy set, and an L1 fill that is a
price event because that path models no queue.
