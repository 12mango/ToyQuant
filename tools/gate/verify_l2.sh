#!/usr/bin/env bash
#
# Tiered behaviour check for the L2 replay path.
#
# The full-file comparison is the strong check, but it reads 1.5 GB, so it is not worth running on
# every edit. This splits the checks into what a change has to pass immediately and what it has to
# pass before it is accepted, and the times below are measured on the development host rather than
# estimated:
#
#   tier 0  (~1 s)   the unit tests, because a red test must not be able to pass this gate
#   tier 1  code half (~20 s)  about twenty invariants on the 200k slice and the L1 fixtures -- events, orders,
#                    fills, PnL, queue consumed, the recorder round trip, direct against threaded, the paper
#                    broker and its reconciliation, the two-book run, the session and halt paths, the depth
#                    guards -- plus the source-level checks (every field written, the layering, the eight
#                    verdicts) and the seven tool self-tests plus the two figure checks
#   tier 1  docs half (~2-4 min)  every number a document quotes, re-derived; the prose, link and layering lints;
#                    `mkdocs build --strict` and the site's repository name. The spread is the host: on a quiet one
#                    the set costs ~110 s, and on a loaded one ~3.5 min, which is why this half is never run in the
#                    foreground. Almost all of it is two pages' blocks: `docs/performance/history-03-studies.md`
#                    replays Databento equity windows at about thirty-five seconds a run, and the multi-day check
#                    replays one snapshot slice a day. A page with no equity block costs about six seconds
#   tier 1  whole (~4 min on a loaded host)  both halves; this is what `fast` runs, so `fast` is backgrounded
#   tier 2  (~15 s)  full 1.5 GB stdout hash against a cached reference
#   tier 3  (~8 s)   the same tests and one short replay under ASan and UBSan
#
# The halves are separate verbs because they are paid for by different kinds of change, and paying minutes
# to learn nothing about the line that was just edited is not a gate, it is a tax:
#
#   smoke            two replays, six fields each: ~1 s, the cheapest thing that can see a broken line
#   code             tier 0 plus the code half: ~20 s, the loop for a change under src/ or tests/
#   docs [paths]     the document half, on the whole set or on the pages named: ~6 s a page, ~3.5 min for the set
#                    on a loaded host
#   fast             tier 0 plus both halves: ~4 min, once per task and before a commit; run it through
#                    `tools/gate/detach.sh` rather than in the foreground
#   all              everything: the acceptance gate
#
# An incremental build after touching one source file is ~16 s on this host, so the smallest honest code
# loop is build plus `smoke`, then build plus `code`, and the document loop is `docs <page>` on the pages that
# changed. Tier 0 first because a red unit test must not be able to pass a gate that only compares replay
# output; that bug happened, and the tier exists because of it.
#
# Tier 1 catches essentially everything a reader or book change can get wrong, because it compares
# event counts, order and fill counts, realised PnL and consumed queue position. Tier 2 is what makes
# a timing claim defensible: it rules out every explanation except cost. Tier 3 is the one standard C++
# check the project did not run: a leak in the block reader, an out-of-range slot in the ladder or a
# signed overflow in the parser is invisible in a normal build.
#
# Usage:
#   tools/gate/verify_l2.sh smoke            # two replays, six fields: the smallest real assertion
#   tools/gate/verify_l2.sh code  [binary]   # tier 0 and the code half of tier 1, the code loop
#   tools/gate/verify_l2.sh docs  [pages...] # the document half, on everything or on the pages named
#   tools/gate/verify_l2.sh fast  [binary]   # tier 0 and tier 1, once per task and before a commit
#   tools/gate/verify_l2.sh ref   [binary]   # store the current full-file output as the reference
#   tools/gate/verify_l2.sh full  [binary]   # tier 2 only, against the stored reference
#   tools/gate/verify_l2.sh asan            # tier 3 only, the sanitizer build
#   tools/gate/verify_l2.sh all   [binary]   # all tiers, for accepting a change
#
# Binary defaults to out/build/linux-debug/toy_quant. Output is one line per tier on success, and
# the expected/actual values on failure. Exit status is 0 when every requested tier matched.

set -u
cd "$(dirname "$0")/../.." || exit 1

# Every verb but `docs` takes the binary as its second argument. `docs` takes pages instead, and the one verb whose
# second argument is not a binary is the one that broke: `verify_l2.sh docs docs/PERFORMANCE.md` read the page as the
# binary, so every block that names its own binary looked for `docs/ring_bench`. A page argument is not a path to a
# program, so the binary for that verb comes from GATE_BIN and defaults like the rest.
DEFAULT_BIN=out/build/linux-debug/toy_quant
BIN="${2:-$DEFAULT_BIN}"
DOC_CHECK_LOG="$(mktemp)"
FIELD_CHECK_LOG="$(mktemp)"
PROSE_CHECK_LOG="$(mktemp)"
TOOL_CHECK_LOG="$(mktemp)"
VERDICT_CHECK_LOG="$(mktemp)"
RISK_CHECK_LOG="$(mktemp)"
TRADES=data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz
L1_TRADES=data/v2/fixtures/test_aggTrades_5k.csv
L1_BBO=data/v2/fixtures/test_bookTicker_5k.csv
FULL_DEPTH=data/v2/crypto/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv
SLICE_200K=/tmp/depth_200k_base.csv
SLICE_2M=/tmp/depth_2m.csv
# The second day exists only as a gzipped snapshot file, so its slice is cut through zcat. It backs the
# out-of-sample check in docs/PERFORMANCE_HISTORY.md, whose numbers the gate re-derives. All three slices are
# built on demand, so the cut costs a second once and nothing afterwards.
SNAPSHOT_0501=data/v2/crypto/deribit_book_snapshot_25_2020-05-01_BTC-PERPETUAL.csv.gz
SLICE_0501=/tmp/snap_0501_200k.csv
# The second book of the two-instrument run: the same venue, the same day, a different instrument. Its depth
# file is gzipped because that is how it arrives, so its slice is cut through zcat like the snapshot above. The
# pair exists so that the one read a single position cannot produce -- gross exposure against net -- is measured
# on a real replay rather than only on an account built by a test.
ETH_TRADES=data/v2/crypto/deribit_trades_2020-04-01_ETH-PERPETUAL.csv.gz
ETH_DEPTH=data/v2/crypto/deribit_incremental_book_L2_2020-04-01_ETH-PERPETUAL.csv.gz
SLICE_ETH_200K=/tmp/eth_depth_200k.csv
# The funding block in docs/PERFORMANCE_HISTORY.md replays ten minutes around the 2024-03-28 08:00 UTC funding
# settlement, because a Binance day file is cut on UTC midnight and midnight is itself a settlement instant, so a
# window that begins with the day applies that settlement to an account that holds nothing. The two slices are one
# streamed pass each over the day files, cached in /tmp like the rest, and skipped when the day is not on disk.
BINANCE_TRADES_0328=data/v2/crypto/BTCUSDT-aggTrades-2024-03-28.csv
BINANCE_BBO_0328=data/v2/crypto/BTCUSDT-bookTicker-2024-03-28.csv
SLICE_SETTLE_TRADES=/tmp/btc_trades_settle.csv
SLICE_SETTLE_BBO=/tmp/btc_bbo_settle.csv
SETTLE_FROM=1711612500000
SETTLE_TO=1711613100000
REF_HASH=tests/fixtures/l2_full_ref.sha256

# The slices are cut from the tracked depth file. /tmp is volatile, so cut them again on demand
# rather than failing with a confusing "input file does not exist".
ensure_slices() {
  [ -f "$SLICE_200K" ] || head -n 200001 "$FULL_DEPTH" > "$SLICE_200K"
  [ -f "$SLICE_2M" ] || head -n 2000001 "$FULL_DEPTH" > "$SLICE_2M"
  if [ ! -f "$SLICE_ETH_200K" ] && [ -f "$ETH_DEPTH" ]; then
    zcat "$ETH_DEPTH" | head -n 200001 > "$SLICE_ETH_200K"
  fi
  if [ ! -f "$SLICE_0501" ] && [ -f "$SNAPSHOT_0501" ]; then
    zcat "$SNAPSHOT_0501" 2>/dev/null | head -n 200001 > "$SLICE_0501"
  fi
  # The out-of-sample table in docs/PERFORMANCE_HISTORY.md quotes one replay per day, each against a slice of that
  # day, and the blocks name the slice. Cutting them here means a day whose slice is missing fails with a run rather
  # than a confusing "input file does not exist", for the same reason as the three above.
  for day_file in data/v2/crypto/deribit_book_snapshot_25_*_BTC-PERPETUAL.csv data/v2/crypto/deribit_book_snapshot_25_*_BTC-PERPETUAL.csv.gz; do
    [ -e "$day_file" ] || continue
    case "$day_file" in
      *2020-05-01*) continue ;;
    esac
    day=$(printf '%s' "$day_file" | sed -n 's/.*_\([0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}\)_BTC-PERPETUAL\.csv\(\.gz\)\{0,1\}$/\1/p')
    [ -n "$day" ] || continue
    day_slice="/tmp/snap_$(printf '%s' "$day" | tr -d -)_200k.csv"
    if [ ! -f "$day_slice" ]; then
      case "$day_file" in
        *.gz) zcat "$day_file" | head -n 200001 > "$day_slice" ;;
        *) head -n 200001 "$day_file" > "$day_slice" ;;
      esac
    fi
  done
  # The two slices the funding block names, cut from the Binance day files the same way and skipped when those are
  # not on disk. The window starts five minutes before the 08:00 UTC settlement and ends five minutes after it, so
  # the replay is holding a position when the settlement arrives; the cut is one pass per file through the tool the
  # document names, rather than an inline filter that a reader would have to trust.
  if [ ! -f "$SLICE_SETTLE_TRADES" ] && [ -f "$BINANCE_TRADES_0328" ]; then
    python3 tools/fetch/slice.py window --input "$BINANCE_TRADES_0328" --column 6 \
      --from "$SETTLE_FROM" --to "$SETTLE_TO" --output "$SLICE_SETTLE_TRADES" > /dev/null
  fi
  if [ ! -f "$SLICE_SETTLE_BBO" ] && [ -f "$BINANCE_BBO_0328" ]; then
    python3 tools/fetch/slice.py window --input "$BINANCE_BBO_0328" --column 6 \
      --from "$SETTLE_FROM" --to "$SETTLE_TO" --output "$SLICE_SETTLE_BBO" > /dev/null
  fi
}

# Sort the tokens so the comparison does not depend on the order the summary lines happen to print in.
tokens() { tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' '; }

L2_WANT=$(printf '%s' "incremental_batches=117288 submitted_orders=406 trade_reports=1 queue_ahead_consumed=55460 realized_pnl=-0.002 equity=1000" | tokens)
L1_WANT=$(printf '%s' "submitted_orders=13734 trade_reports=2277 fill_rate=0.175872 realized_pnl=-33.7861 equity=966.196" | tokens)

tier0() {
  local build_dir log
  build_dir=$(dirname "$BIN")
  log=$(ctest --test-dir "$build_dir" 2>&1 | grep -E 'tests passed')
  if printf '%s' "$log" | grep -q '100% tests passed'; then
    echo "tier0 OK"
    return 0
  fi
  echo "tier0 MISMATCH"
  echo "  ${log:-ctest produced no result; is $build_dir built?}"
  ctest --test-dir "$build_dir" --rerun-failed --output-on-failure 2>&1 | tail -20 | sed 's/^/  /'
  return 1
}

tier1() {
  # `tier1 [full|code|docs] [documents...]` runs the whole tier, the code half of it, or the document half. The split
  # exists because the two halves are paid for by different kinds of change: the code half asserts about twenty
  # invariants on a 200k-row slice and costs about twelve seconds, while the document half re-derives every number a
  # document quotes and costs about seventy-five -- sixty of them in one page, because its block replays Databento
  # windows rather than Deribit slices. A loop that pays seventy-five seconds to learn nothing about the line it just
  # edited is not a gate, it is a tax.
  local mode=${1:-full}
  if [ $# -gt 0 ]; then shift; fi
  local l2=$L2_WANT l1=$L1_WANT rc=0
  # Declared unconditionally because a mode skips blocks, and `set -u` refuses to read a counter that the skipped
  # block would have declared: the summary asks about every one of them whichever half was run.
  local fields_check=0 prose_check=0 verdict_check=0 risk_check=0 rec_ok=0 thr_ok=0 broker_ok=0 multi_ok=0 \
        session_ok=0 close_ok=0 halt_ok=0 depth_guard_ok=0 doc_check=0 tool_check=0
  # Every metric field a summary prints has to be written somewhere. A field that is read but never
  # written looks exactly like a field whose value is zero, and this project shipped four of those in
  # one line before. Source-level, so it costs nothing and runs on every edit.
  local fields_check=0
  if [ "$mode" != docs ]; then
  python3 tools/gate/check_written_fields.py > "$FIELD_CHECK_LOG" 2>&1 || fields_check=1
  fi
  # Prose is not covered by check_docs.py, which only reads marked number blocks, and a paragraph that
  # describes a stale model set is exactly what that leaves behind. This compares the claims against the
  # source: flags, queue models and tool paths.
  local prose_check=0
  if [ "$mode" != code ]; then
  python3 tools/gate/lint_prose.py docs > "$PROSE_CHECK_LOG" 2>&1 || prose_check=1
  # A link is the only thing in the documents that a reader follows rather than reads, and nothing checked one until a
  # split moved a hundred of them: the file has to exist and the `#anchor` has to match a heading in it. Counting the
  # openings as well catches the other half -- a link whose parentheses do not balance is invisible to a pattern that
  # wants a well-formed one, so it would silently drop out of this check instead of failing it.
  python3 tools/gate/check_links.py >> "$PROSE_CHECK_LOG" 2>&1 || prose_check=1
  python3 tools/gate/check_layering.py >> "$PROSE_CHECK_LOG" 2>&1 || prose_check=1
  # `docs/adr/` keeps one fact in two places -- the record's status line and the index table -- so the pair is checked
  # like every other pair in this repository. It is the cheapest check here and the only one that can catch a decision
  # that was replaced and never said so.
  python3 tools/gate/check_adr.py >> "$PROSE_CHECK_LOG" 2>&1 || prose_check=1
  # The site build is the one document check this gate did not run, and the gap had teeth: `mkdocs build --strict`
  # refuses a link that resolves outside `docs_dir`, so a relative link to `../CHANGELOG.md` was invisible here and
  # red in CI -- every local tier green on a site that could not be built. Skipped rather than failed when mkdocs is
  # not installed, because the checks in this tier must not depend on a Python package; CI installs it, and there the
  # check is not optional.
  if command -v mkdocs > /dev/null 2>&1; then
    mkdir -p out/lat
    mkdocs build --strict -d out/lat/site_gate >> "$PROSE_CHECK_LOG" 2>&1 || prose_check=1
    # The site carries the repository's name in its title and in the header link, and both come from one place:
    # `site_name` and `repo_name` in mkdocs.yml. A stale value there publishes a site that names a repository that
    # no longer exists -- which is exactly what the published site did for two weeks after the rename, because
    # nothing checked the built pages rather than the source they came from.
    if grep -rl 'ToyQuant-v1' out/lat/site_gate > /dev/null 2>&1; then
      echo "FAIL the built site still carries the old repository name: $(grep -rl 'ToyQuant-v1' out/lat/site_gate | head -3 | tr '\n' ' ')" >> "$PROSE_CHECK_LOG"
      prose_check=1
    fi
  fi
  fi
  if [ "$mode" != docs ]; then
  # The eight closed lines are claims about arithmetic too, and a copied number drifts the way a copied claim does.
  # This re-derives every quotient from the recorded inputs, checks that the first gate to fire is the one the line
  # is recorded as binding on, and refuses an optimistic bound read as an opening. Source-level and data-free, so it
  # costs nothing and runs on every edit.
  local verdict_check=0
  python3 tools/study/viability.py > "$VERDICT_CHECK_LOG" 2>&1 || verdict_check=1
  # The pre-trade controls are measured rather than asserted in prose: the no-flag run must reproduce the
  # baseline, a permissive setting must not change it, and each named control must actually refuse an order
  # and still reconcile. tools/sweep/risk_check.sh holds the four runs and their invariants.
  local risk_check=0
  bash tools/sweep/risk_check.sh "$BIN" "$TRADES" "$SLICE_200K" BTC-PERPETUAL > "$RISK_CHECK_LOG" 2>&1 || risk_check=1
  l2=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 conservative \
        --fast-validation 2>&1 | grep -oE 'incremental_batches=117288|submitted_orders=406|trade_reports=1|queue_ahead_consumed=55460|realized_pnl=-0.002|equity=1000' | tokens)
  l1=$("$BIN" replay "$L1_TRADES" "$L1_BBO" BTCUSDT 0 optimized 1000000 2>&1 |
        grep -oE 'submitted_orders=13734|trade_reports=2277|fill_rate=0.175872|realized_pnl=-33.7861|equity=966.196' | tokens)
  [ "$l2" = "$L2_WANT" ] || rc=1
  [ "$l1" = "$L1_WANT" ] || rc=1
  # The recorder's assertion, which is what the recorder half of Stage A claims: the stream a replay
  # delivered, written back out in the replay shape and replayed again, is the same run. The
  # comparison drops the two kinds of line that have to differ, the input paths in the [Mode] line
  # and the [REC] line that only the recording run prints, and requires every other number to match
  # exactly: event counts, orders, fills, the queue that was consumed, PnL and the final equity.
  local rec_ok=0 rec_prefix=out/lat/rec_gate
  mkdir -p out/lat
  # The recording run is paced at 200x, so the assertion covers the claim as Stage A words it — a
  # paced session becomes a replayable file — while the schedule stays a fraction of a second of wall
  # clock and the tier never waits out the window. The replay of the recording is unpaced, because
  # what is being compared is the decisions, not the sleeps.
  rec_a=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --live-pacing=200 --record="$rec_prefix" 2>&1 | grep -v -e '^\[Mode:' -e '^\[REC\]' -e '^\[LIVE\]')
  rec_b=$("$BIN" l2_replay "${rec_prefix}_trades.csv" "${rec_prefix}_depth.csv" BTC-PERPETUAL 0 active_l2 1 \
        prorata --fast-validation 2>&1 | grep -v -e '^\[Mode:' -e '^\[REC\]' -e '^\[LIVE\]')
  [ "$rec_a" = "$rec_b" ] || rec_ok=1
  # The threading assertion: the same slice through a feed thread and a ring has to report the same
  # numbers as the direct path, because threading changes when a decision is taken and not what it is.
  # The [LATENCY] line is allowed to differ, being the measurement rather than the result, and it has
  # to carry exactly one hand-off per delivered event, which is checked against the run's own event
  # count instead of a number copied out of this script.
  local thr_ok=0 thr_raw thr plain
  thr_raw=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --feed-thread 2>&1)
  plain=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        2>&1 | grep -v '^\[Mode:')
  thr=$(printf '%s' "$thr_raw" | grep -v -e '^\[Mode:' -e '^\[LATENCY\]')
  [ "$thr" = "$plain" ] || thr_ok=1
  thr_events=$(printf '%s' "$thr_raw" | grep -oE 'events=[0-9]+' | head -1 | sed 's/events=//')
  thr_handoffs=$(printf '%s' "$thr_raw" | grep -oE '^\[LATENCY\] handoffs=[0-9]+' | sed 's/.*=//')
  [ -n "$thr_events" ] && [ "$thr_events" = "$thr_handoffs" ] || thr_ok=1
  # The broker sits between the strategy and the engine, so a broker with no faults configured has to
  # produce the run without it, apart from the two lines that are the broker and the reconciliation, and
  # the reconciliation has to agree with the portfolio. A faulted broker, on the other hand, has to be
  # visible: a fault injection whose faults cannot be seen is worse than none, because the run carrying it
  # looks exactly like a clean one.
  local broker_ok=0 broker_raw broker_plain broker_break
  broker_raw=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --paper-broker 2>&1)
  broker_plain=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        2>&1 | grep -v '^\[Mode:')
  [ "$(printf '%s' "$broker_raw" | grep -v -e '^\[Mode:' -e '^\[BROKER\]' -e '^\[RECON\]')" = "$broker_plain" ] ||
    broker_ok=1
  printf '%s' "$broker_raw" | grep -q '^\[RECON\].*status=ok' || broker_ok=1
  printf '%s' "$broker_raw" | grep -q '^\[RECON\].*quantity_breaks=0 max_quantity_break=0 net_quantity_break=0 gross_quantity_break=0' ||
    broker_ok=1
  printf '%s' "$broker_raw" | grep -q '^\[BROKER\].*orders_rejected=0 fills_seen=[0-9]* fills_split=0 fills_dropped=0' ||
    broker_ok=1
  # One lost execution report has to show up as a break, in the same columns the check above reads as
  # zero. Which column it lands in depends on the fills the window happens to have, so only the verdict
  # is asserted here.
  broker_break=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
        --paper-broker --broker-drop-every=1 2>&1 | grep -oE '^\[RECON\].*status=[a-z]+$')
  printf '%s' "$broker_break" | grep -q 'status=break' || broker_ok=1
  # The two-book run: a second instrument into the same account. Three different claims, each its own number.
  # Both streams were delivered -- the event count equals the sum of the two books' own counts, which a merge
  # that dropped or duplicated a stream could not satisfy. Both books decided, one [EXECUTION] line each, which
  # is the count a single-book run cannot produce. And the account reports one book of two legs valued at each
  # instrument's own face value: ten dollars for a BTC contract, one for an ETH one, so a gross of 11 against a
  # net of 9 is what says the two legs are not the same position. The block is skipped when the second
  # instrument's data is not on disk, the way the funding block below is, because the archive is not in the tree.
  local multi_ok=0
  if [ -f "$ETH_TRADES" ] && [ -f "$SLICE_ETH_200K" ]; then
    local multi multi_events multi_booked multi_books multi_lines
    multi=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
          --symbol-b=ETH-PERPETUAL --trades-b="$ETH_TRADES" --depth-b="$SLICE_ETH_200K" 2>&1)
    printf '%s' "$multi" |
      grep -q '^\[EXPOSURE\] symbols=2 legs=BTC-PERPETUAL:1:10,ETH-PERPETUAL:-1:-1 gross_usd=11 net_usd=9 gross_to_net=1.22222 fees_usd=0.0202$' ||
      multi_ok=1
    multi_lines=$(printf '%s' "$multi" | grep -c '^\[EXECUTION\] symbol=')
    [ "$multi_lines" = 2 ] || multi_ok=1
    multi_books=$(printf '%s' "$multi" | grep -oE '^\[L2 DATA\].* books=[0-9]+' | grep -oE 'books=[0-9]+' |
      sed 's/books=//' | head -1)
    [ "$multi_books" = 2 ] || multi_ok=1
    multi_events=$(printf '%s' "$multi" | grep -oE '^\[L2 DATA\].* events=[0-9]+' | grep -oE 'events=[0-9]+' |
      sed 's/events=//' | head -1)
    multi_booked=$(printf '%s' "$multi" | grep -oE '^\[BOOK\] .*events=[0-9]+' |
      awk -F'events=' '{sum += $2} END {print sum + 0}')
    [ -n "$multi_events" ] && [ "$multi_events" = "$multi_booked" ] || multi_ok=1
    # The cap is the account's, not each book's. With ten dollars the BTC book fills to ten and the ETH book is
    # refused every quote, because its own dollar would be the eleventh: the exposure line then reports one leg
    # rather than two, and the fees are the one book's. Two books each comparing their own notional with the same
    # cash would report two legs and eleven dollars here, which is the bug this assertion exists for.
    printf '%s' "$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata \
      --fast-validation --max-exposure-usd=10 --symbol-b=ETH-PERPETUAL --trades-b="$ETH_TRADES" \
      --depth-b="$SLICE_ETH_200K" 2>&1)" |
      grep -q '^\[EXPOSURE\] symbols=1 legs=BTC-PERPETUAL:1:10 gross_usd=10 net_usd=10 gross_to_net=1 fees_usd=0.0144$' ||
      multi_ok=1
  fi
  # The session's two halves: a run that writes its state, and a run that takes it over. What has to hold is that
  # the resumed run's account *is* the account the first run reported -- every field of [PORTFOLIO] and [EXPOSURE],
  # because a snapshot that loses one of them (a fee, a trade count, an entry price) leaves a different account and
  # nothing else would notice. The resumed run replays the same file and holds its own orders until the snapshot's
  # clock, so it has no decisions left to take and its summary is the state it was handed.
  local session_ok=0 session_dir
  session_dir=$(mktemp -d)
  "$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
    --snapshot-out="$session_dir/session" > "$session_dir/first.log" 2>&1 || session_ok=1
  "$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
    --snapshot-in="$session_dir/session" > "$session_dir/second.log" 2>&1 || session_ok=1
  grep -q '^toyquant-session 2$' "$session_dir/session" || session_ok=1
  local first_account second_account first_book second_book resume orders live gone
  first_account=$(grep -E '^\[PORTFOLIO\]' "$session_dir/first.log")
  second_account=$(grep -E '^\[PORTFOLIO\]' "$session_dir/second.log")
  first_book=$(grep -E '^\[EXPOSURE\]' "$session_dir/first.log")
  second_book=$(grep -E '^\[EXPOSURE\]' "$session_dir/second.log")
  [ -n "$first_account" ] && [ "$first_account" = "$second_account" ] || session_ok=1
  [ -n "$first_book" ] && [ "$first_book" = "$second_book" ] || session_ok=1
  resume=$(grep -E '^\[RESUME\]' "$session_dir/second.log")
  printf '%s' "$resume" | grep -q 'legs=1' || session_ok=1
  orders=$(printf '%s' "$resume" | grep -oE ' orders=[0-9]+' | grep -oE '[0-9]+')
  live=$(printf '%s' "$resume" | grep -oE ' live=[0-9]+' | grep -oE '[0-9]+')
  gone=$(printf '%s' "$resume" | grep -oE ' gone=[0-9]+' | grep -oE '[0-9]+')
  # Every order the snapshot listed is resolved into one of the two buckets, so the three numbers cannot drift
  # apart: an order that is neither live nor gone is an order the recovery did not look at.
  [ -n "$orders" ] && [ "$orders" = "$((live + gone))" ] || session_ok=1
  # The same two halves with two books: one account, one record per book, each with its own clock and id sequence.
  # The claim is the same -- the resumed run's account is the account the first run reported -- and the extra
  # assertion is that the snapshot names both books and that each of them resolves its own orders.
  if [ -f "$ETH_TRADES" ] && [ -f "$SLICE_ETH_200K" ]; then
    local pair_dir pair_first_account pair_second_account books resume_lines
    pair_dir=$(mktemp -d)
    "$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
      --symbol-b=ETH-PERPETUAL --trades-b="$ETH_TRADES" --depth-b="$SLICE_ETH_200K" \
      --snapshot-out="$pair_dir/session" > "$pair_dir/first.log" 2>&1 || session_ok=1
    "$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
      --symbol-b=ETH-PERPETUAL --trades-b="$ETH_TRADES" --depth-b="$SLICE_ETH_200K" \
      --snapshot-in="$pair_dir/session" > "$pair_dir/second.log" 2>&1 || session_ok=1
    books=$(grep -c '^book=' "$pair_dir/session")
    [ "$books" = 2 ] || session_ok=1
    resume_lines=$(grep -c '^\[RESUME\]' "$pair_dir/second.log")
    [ "$resume_lines" = 2 ] || session_ok=1
    pair_first_account=$(grep -E '^\[PORTFOLIO\]' "$pair_dir/first.log")
    pair_second_account=$(grep -E '^\[PORTFOLIO\]' "$pair_dir/second.log")
    [ -n "$pair_first_account" ] && [ "$pair_first_account" = "$pair_second_account" ] || session_ok=1
    [ "$(grep -E '^\[EXPOSURE\]' "$pair_dir/first.log")" = \
      "$(grep -E '^\[EXPOSURE\]' "$pair_dir/second.log")" ] || session_ok=1
    rm -rf "$pair_dir"
  fi
  rm -rf "$session_dir"
  # The end of the session, priced. A run that stops holding something has marked it at the last midpoint; --close-out
  # sends it out at the touch and reports what the exit cost. The assertion is the identity that holds whether or not
  # the exit filled: what was carried is what closed plus what is left, and the line says which of the three happened
  # (filled, refused, no liquidity at the touch) instead of leaving a reader to guess.
  local close_ok=0 close_line close_position close_closed close_residual
  close_line=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
    --close-out 2>&1 | grep -oE '^\[WIND-DOWN\].*$')
  [ -n "$close_line" ] || close_ok=1
  close_position=$(printf '%s' "$close_line" | grep -oE ' position=-?[0-9]+' | grep -oE '\-?[0-9]+')
  close_closed=$(printf '%s' "$close_line" | grep -oE ' closed=[0-9]+' | grep -oE '[0-9]+')
  close_residual=$(printf '%s' "$close_line" | grep -oE ' residual=-?[0-9]+' | grep -oE '\-?[0-9]+')
  [ -n "$close_position" ] && [ -n "$close_closed" ] && [ -n "$close_residual" ] &&
    [ "${close_position#-}" = "$((close_closed + ${close_residual#-}))" ] || close_ok=1
  # A latched kill switch flattens. The two halves had to be fixed to make this true: the switch refused every order
  # including the one that would close the position, and the account was never marked during a run so the loss it
  # reads sat in the position rather than in the PnL. Here the limit is below the run's own loss, so the switch fires,
  # closes the contract the book was holding, and the account ends flat -- `[EXPOSURE] symbols=0` is the assertion,
  # because a halt that leaves the position open is a control that stops without reducing anything.
  local halt_ok=0 halt_line
  halt_line=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
    --kill-switch-loss-usd=0.001 2>&1)
  printf '%s' "$halt_line" | grep -q 'kill_switch_triggered=1' || halt_ok=1
  printf '%s' "$halt_line" | grep -q 'kill_switch_closed_quantity=1' || halt_ok=1
  printf '%s' "$halt_line" | grep -q '^\[EXPOSURE\] symbols=0 legs=none' || halt_ok=1
  # And a limit the run never reaches leaves it exactly as it was: the control is opt-in, and a run that arms it above
  # its own loss keeps the numbers every document quotes.
  printf '%s' "$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
    --kill-switch-loss-usd=0.02 2>&1)" | grep -q 'kill_switch_triggered=0' || halt_ok=1
  # A depth file that never delivers a depth level used to produce a beautifully formatted run of zeros and an exit
  # code of zero -- a measurement of nothing that read like a result. It is an error now, and so is a book that was
  # given no market events in a run that trades several. Both are checked by their message, because the point is that
  # the run refuses rather than that it fails.
  local depth_guard_ok=0 guard_dir
  guard_dir=$(mktemp -d)
  printf 'exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n' > "$guard_dir/header_only.csv"
  "$BIN" l2_replay "$TRADES" "$guard_dir/header_only.csv" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
    > "$guard_dir/empty.log" 2>&1 && depth_guard_ok=1
  grep -q 'delivered no depth events' "$guard_dir/empty.log" || depth_guard_ok=1
  # A crossed snapshot is not a market. The book already refused the top of one; now it counts it, so a run that
  # skipped thousands of crossed books cannot read like a run whose strategy chose not to quote. The count rides on
  # [L2 DATA] and is absent when it is zero, which is why the clean run is asserted by its absence.
  printf 'exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n' > "$guard_dir/crossed.csv"
  printf 'deribit,BTC-PERPETUAL,1585699200245000,1585699200355684,true,bid,100.5,10\n' >> "$guard_dir/crossed.csv"
  printf 'deribit,BTC-PERPETUAL,1585699200245000,1585699200355684,true,ask,99.5,10\n' >> "$guard_dir/crossed.csv"
  printf 'exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n' > "$guard_dir/trades.csv"
  printf 'deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,100.0,10\n' >> "$guard_dir/trades.csv"
  "$BIN" l2_replay "$guard_dir/trades.csv" "$guard_dir/crossed.csv" BTC-PERPETUAL 0 active_l2 1 prorata \
    --fast-validation 2>&1 | grep -q '^\[L2 DATA\].* crossed_book_batches=1' || depth_guard_ok=1
  # And the window every recorded number comes from has none: the count is a statement about the data, so a slice that
  # acquired a crossed book would change what those numbers mean.
  "$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation 2>&1 |
    grep -q 'crossed_book_batches' && depth_guard_ok=1
  rm -rf "$guard_dir"
  fi
  # The numbers the documents quote are claims about this code, and hand-copied claims drift: one did,
  # a "thirty times per fill" that a re-measurement turned into ten. They are checked at the same gate
  # as the invariants, on the same slices, so a documented value that stops being true fails a run.
  local doc_check=0
  if [ "$mode" != code ]; then
    # A named document is what a documentation edit is about, and the whole set is what a commit is about: the verb
    # passes `docs` by default and the paths when it has them, so a page costs its own seconds rather than the set's.
    local doc_paths=docs
    if [ $# -gt 0 ]; then doc_paths="$@"; fi
    # shellcheck disable=SC2086
    python3 tools/gate/check_docs.py --binary "$BIN" $doc_paths \
      > "$DOC_CHECK_LOG" 2>&1 || doc_check=1
  fi
  # The IEX HIST reader walks the pcap, IP, UDP and IEX-TP framing that a decoder sits on top of. The file it
  # walks in this test is built by the tool rather than downloaded, because the archive is behind an account,
  # so the container walk is the part that can be checked on every edit. It says nothing about DEEP messages,
  # which is why the tool decodes none.
  local tool_check=0
  if [ "$mode" != docs ]; then
  python3 tools/fetch/fetch_iex_hist.py --self-test > "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  # The capacity table is arithmetic over the fee schedule, and that schedule is what every equities row in the
  # documents is priced with: a table of money is worth what its arithmetic is, so its own hand-checked cells run
  # here rather than being trusted.
  python3 tools/study/capacity_table.py --self-test >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  # The interval tool is the one that decides whether a mean is quotable at all, and its own arithmetic is checked
  # here for the same reason: an interval that was wrong in the direction of confidence would make every row it
  # touches look more settled than it is. Its replay mode is exercised by the marked block in
  # docs/PERFORMANCE_HISTORY.md, which the doc check above runs, so this tier does not replay the windows twice.
  python3 tools/study/pnl_interval.py --self-test >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  # The walk-forward is the tool that decides whether a swept parameter was a finding or the window it was found
  # on, and its selection rule, its tie rule, its handling of a missing cell and its two refusals are arithmetic a
  # document quotes: they run here so a change to any of them fails a gate rather than a re-read.
  python3 tools/study/walk_forward.py --self-test >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  # The volatility window is the tick range over a fixed window that `src/strategy/active_l2_market_maker.h`
  # records as the thing its per-second speed cannot be. A range, the two averages and the median split are what a
  # document would quote, and the split's tie rule is the kind of detail that drifts silently, so it is checked.
  python3 tools/study/volatility_window.py --self-test >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  # The slice driver is what turns "are the quiet minutes the losing ones?" into two files that can be joined:
  # its per-slice parse of a run's own fields, its rule that a slice with no trade is counted rather than replayed,
  # and its refusal to turn a failed replay into a row of zeros are what a reader trusts, and a fake binary in its
  # self-test is cheaper than the ninety-six replays a real run needs.
  python3 tools/study/slice_pnl.py --self-test >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  # The relay owns the venue's message shapes and the order channel's translation, which nothing else in the tree
  # can check: its hand-built login signature against a pinned vector, a peer that verifies the login that crossed
  # a socket, the refusals, and the reconnect that re-logs-in and re-subscribes are what keep the live leg honest,
  # so they run here rather than being trusted. `tests/relay_order_channel_test` drives the simulated backend with
  # the real adapter; this runs the private route's own checks with no key and no endpoint.
  python3 tools/live/venue_relay.py --self-test >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  # A figure that no longer matches the script that draws it is a picture of a system that changed, and neither the
  # site build nor the prose lint reads a `.svg`. Both figures are checked here for that reason: the same contract,
  # and the path figures are the ones a reader meets before any number.
  python3 tools/study/figure_path.py --check >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  python3 tools/study/figure_charge.py --check >> "$TOOL_CHECK_LOG" 2>&1 || tool_check=1
  fi
  local label=tier1
  [ "$mode" = code ] && label=code
  [ "$mode" = docs ] && label=docs
  if [ $rc -eq 0 ] && [ $rec_ok -eq 0 ] && [ $thr_ok -eq 0 ] && [ $broker_ok -eq 0 ] &&
     [ $multi_ok -eq 0 ] &&
     [ $session_ok -eq 0 ] &&
     [ $close_ok -eq 0 ] &&
     [ $halt_ok -eq 0 ] &&
     [ $depth_guard_ok -eq 0 ] &&
     [ $doc_check -eq 0 ] &&
     [ $fields_check -eq 0 ] && [ $prose_check -eq 0 ] && [ $tool_check -eq 0 ] &&
     [ $verdict_check -eq 0 ]; then
    echo "$label OK"
    return 0
  fi
  echo "$label MISMATCH"
  [ $rec_ok -eq 0 ] || echo "  recorder: replaying the recording did not reproduce the run"
  [ $thr_ok -eq 0 ] || echo "  threads: the threaded run did not match the direct run, or its hand-off count did not"
  [ $broker_ok -eq 0 ] || echo "  broker: the brokered run did not match the direct one, or a lost report was not reported"
  [ $multi_ok -eq 0 ] ||
    echo "  multi: the two-book run did not report two books, two legs, or the sum of its own event counts"
  [ $session_ok -eq 0 ] ||
    echo "  session: the resumed run did not report the account the first run left, or an order went unresolved"
  [ $close_ok -eq 0 ] ||
    echo "  close: the end-of-session line is missing, or what it closed and left does not add up to what it carried"
  [ $halt_ok -eq 0 ] ||
    echo "  halt: the kill switch fired and left the account holding something, or a limit it never reaches changed the run"
  [ $depth_guard_ok -eq 0 ] ||
    echo "  depth: an empty depth file or a crossed book was not reported, or the recorded window acquired a crossed one"
  [ $fields_check -eq 0 ] || sed 's/^/  fields: /' "$FIELD_CHECK_LOG"
  [ $prose_check -eq 0 ] || sed 's/^/  prose: /' "$PROSE_CHECK_LOG"
  [ $tool_check -eq 0 ] || sed 's/^/  tool: /' "$TOOL_CHECK_LOG"
  [ $verdict_check -eq 0 ] || grep -v '^     also:' "$VERDICT_CHECK_LOG" | sed 's/^/  verdict: /'
  [ $risk_check -eq 0 ] || sed 's/^/  risk: /' "$RISK_CHECK_LOG"
  [ "$l2" = "$L2_WANT" ] || { echo "  L2 want: $L2_WANT"; echo "  L2 got : $l2"; }
  [ "$l1" = "$L1_WANT" ] || { echo "  L1 want: $L1_WANT"; echo "  L1 got : $l1"; }
  [ $doc_check -eq 0 ] || sed 's/^/  doc: /' "$DOC_CHECK_LOG"
  return 1
}

full_hash() {
  # The mode line carries absolute input paths, so it would make the reference a fact about one checkout rather than
  # about the code: dropping it is what lets the hash live in the tree and mean something on any machine. Nothing
  # else is filtered, because the point of this tier is that every number in the run is identical.
  "$BIN" l2_replay "$TRADES" "$FULL_DEPTH" BTC-PERPETUAL 0 active_l2 1 conservative \
    --fast-validation 2>&1 | grep -v '^\[Mode:' | sha256sum | cut -d' ' -f1
}

tier2() {
  if [ ! -f "$REF_HASH" ]; then
    echo "tier2 SKIP (no reference in the tree yet; run: tools/gate/verify_l2.sh ref)"
    return 0
  fi
  local now
  now=$(full_hash)
  if [ "$now" = "$(cat "$REF_HASH")" ]; then
    echo "tier2 OK"
    return 0
  fi
  echo "tier2 MISMATCH"
  echo "  reference: $(cat "$REF_HASH")"
  echo "  current  : $now"
  return 1
}

# Tier 3 sanitizes the code that the other tiers only measure. A leak in the block reader, an out-of-range
# slot in the ladder or a signed overflow in the parser is invisible in a normal build. The build is
# incremental, so the cost is a rebuild of whatever changed plus a few seconds of running, and
# -fno-sanitize-recover=all makes the first undefined behaviour abort instead of warning, so a finding cannot
# be missed by reading the log too quickly. If the toolchain has no sanitizers the tier skips rather than
# failing: a missing compiler feature is not a regression in this code.
tier3() {
  local asan_build_dir=out/build/linux-asan
  local log
  log=$(mktemp)
  if ! cmake --preset linux-asan > "$log" 2>&1; then
    echo "tier3 SKIP (the sanitizer preset did not configure on this toolchain)"
    return 0
  fi
  if ! cmake --build "$asan_build_dir" >> "$log" 2>&1; then
    echo "tier3 MISMATCH (sanitizer build failed)"
    tail -20 "$log" | sed 's/^/  /'
    return 1
  fi
  local rc=0
  ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" UBSAN_OPTIONS="print_stacktrace=1" \
    ctest --test-dir "$asan_build_dir" >> "$log" 2>&1 || rc=1
  ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" UBSAN_OPTIONS="print_stacktrace=1" \
    "$asan_build_dir/toy_quant" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 prorata \
    --fast-validation --no-output >> "$log" 2>&1 || rc=1
  local finding
  finding=$(grep -m3 -E 'ERROR: AddressSanitizer|runtime error:|LeakSanitizer' "$log")
  if [ $rc -eq 0 ] && [ -z "$finding" ]; then
    echo "tier3 OK"
    return 0
  fi
  echo "tier3 MISMATCH"
  [ -z "$finding" ] || echo "$finding" | sed 's/^/  /'
  [ $rc -eq 0 ] || tail -5 "$log" | sed 's/^/  /'
  return 1
}

# The smallest real assertion in the repository: two replays and the six fields they are checked on, a third of a
# second with the slices cut. It exists because the gap between "it compiles" and "it is worth twelve seconds of
# invariants" is where most of a change's mistakes are, and the six fields below are the cheapest thing that can see
# them: an event count, the order count, the fill count, the queue consumed, the PnL and the final equity.
smoke() {
  ensure_slices
  local l2 l1 rc=0
  l2=$("$BIN" l2_replay "$TRADES" "$SLICE_200K" BTC-PERPETUAL 0 active_l2 1 conservative \
        --fast-validation 2>&1 | grep -oE 'incremental_batches=117288|submitted_orders=406|trade_reports=1|queue_ahead_consumed=55460|realized_pnl=-0.002|equity=1000' | tokens)
  l1=$("$BIN" replay "$L1_TRADES" "$L1_BBO" BTCUSDT 0 optimized 1000000 2>&1 |
        grep -oE 'submitted_orders=13734|trade_reports=2277|fill_rate=0.175872|realized_pnl=-33.7861|equity=966.196' | tokens)
  [ "$l2" = "$L2_WANT" ] || { echo "smoke MISMATCH"; echo "  L2 want: $L2_WANT"; echo "  L2 got : $l2"; rc=1; }
  [ "$l1" = "$L1_WANT" ] || { [ $rc -eq 0 ] && echo "smoke MISMATCH"; echo "  L1 want: $L1_WANT"; echo "  L1 got : $l1"; rc=1; }
  [ $rc -eq 0 ] && echo "smoke OK"
  return $rc
}

case "${1:-fast}" in
  smoke) smoke ;;
  code) ensure_slices; rc=0; tier0 || rc=1; tier1 code || rc=1; exit $rc ;;
  docs) shift; ensure_slices; BIN="${GATE_BIN:-$DEFAULT_BIN}"; tier1 docs "$@" ;;
  fast) ensure_slices; rc=0; tier0 || rc=1; tier1 || rc=1; exit $rc ;;
  ref)  ensure_slices; full_hash > "$REF_HASH"; echo "reference stored: $(cat "$REF_HASH")" ;;
  full) ensure_slices; tier2 ;;
  asan) ensure_slices; tier3 ;;
  all)  ensure_slices; rc=0; tier0 || rc=1; tier1 || rc=1; tier2 || rc=1; tier3 || rc=1; exit $rc ;;
  *)    echo "usage: tools/gate/verify_l2.sh smoke|code|docs [paths]|fast|ref|full|asan|all [binary]" >&2; exit 2 ;;
esac
