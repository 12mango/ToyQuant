#include "app/app_config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <system_error>

#include "utils/stage_profiler.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

namespace
{
// The accepted strategy names, in one place. The usage text prints this list and the validation below
// searches it, so a name cannot be accepted without being documented: the two were written out
// separately, and four names (`l2_baseline`, `l2_depth`, `l2_micro`, `l2_flow`) were accepted while the
// usage text stopped short of them. The order is the order the usage text prints, grouped by family.
constexpr std::array<std::string_view, 19> kStrategyNames = {
    // The touch-only baselines. Neither implements the L2 entry point, so neither sees depth.
    "optimized", "naive",
    // The frozen L1 line, kept so recorded results stay reproducible rather than re-measured.
    "l1", "passive_l1", "inventory_aware_l1", "flow_aware_l1", "active_l1",
    // The active L2 line. These are one class under several signal configs, which is why the names
    // outnumber the classes they select.
    "l2", "passive_l2", "l2_baseline", "l2_depth", "l2_micro", "l2_flow", "flow_aware_l2",
    "inventory_aware_l2", "active_l2", "adaptive_l2",
    // The non-market-making line: a signal sets a target position instead of a quote.
    "trend_l2",
    // The execution line: a parent order from the configuration and a schedule, with no direction of its own.
    "execution_l2",
};
constexpr std::string_view kDefaultStrategyName = "optimized";

constexpr std::string_view kProfileStagesPrefix = "--profile-stages=";
constexpr std::string_view kLivePacingPrefix = "--live-pacing=";
constexpr std::string_view kRecordPrefix = "--record=";
constexpr std::string_view kFeedThreadFlag = "--feed-thread";
constexpr std::string_view kRingCapacityPrefix = "--ring-capacity=";
constexpr std::string_view kWebSocketTargetPrefix = "--ws-target=";
constexpr std::string_view kWebSocketSecondsPrefix = "--ws-seconds=";
constexpr std::string_view kWebSocketRingPrefix = "--ws-ring-capacity=";
constexpr std::string_view kIdleWaitPrefix = "--idle-wait-us=";
constexpr std::string_view kPaperBrokerFlag = "--paper-broker";
constexpr std::string_view kBrokerRejectPrefix = "--broker-reject-every=";
constexpr std::string_view kBrokerPartialPrefix = "--broker-partial-every=";
constexpr std::string_view kBrokerDropPrefix = "--broker-drop-every=";
constexpr std::string_view kBrokerOutageAfterPrefix = "--broker-outage-after=";
constexpr std::string_view kBrokerOutageHoldsPrefix = "--broker-outage-holds=";
constexpr std::string_view kOrderGatewayFlag = "--order-gateway";
constexpr std::string_view kGatewayAckTimeoutPrefix = "--gateway-ack-timeout-us=";
constexpr std::string_view kGatewayRetryTimeoutFlag = "--gateway-retry-timeouts";
constexpr std::string_view kOrderSizePrefix = "--order-size=";
constexpr std::string_view kInventoryLimitPrefix = "--inventory-limit=";
constexpr std::string_view kMaxPositionPrefix = "--max-position=";
constexpr std::string_view kMaxExposurePrefix = "--max-exposure-usd=";
constexpr std::string_view kKillSwitchPrefix = "--kill-switch-loss-usd=";
constexpr std::string_view kFatFingerPrefix = "--fat-finger-bps=";
constexpr std::string_view kMaxOrdersPerSecondPrefix = "--max-orders-per-second=";
constexpr std::string_view kOrderBurstPrefix = "--order-burst=";
constexpr std::string_view kParticipationLimitPrefix = "--participation-limit=";
constexpr std::string_view kTimeInForcePrefix = "--time-in-force=";
// Add-liquidity-only, which the venue enforces rather than the strategy: an order that would trade on arrival is
// cancelled instead, so a quote that arrived after the market went through it pays nothing rather than the taker
// rate. It has no value, because there is nothing to say beyond "on".
constexpr std::string_view kPostOnlyFlag = "--post-only";
// Close the position at the end of the input, at the touch, and report what the exit cost. Nothing sets it by
// default: a run that stops with a position has marked it rather than closed it, and that is what the recorded
// numbers describe.
constexpr std::string_view kCloseOutFlag = "--close-out";
constexpr std::string_view kTrendParamsPrefix = "--trend-params=";
constexpr std::string_view kBaseSpreadPrefix = "--base-spread-ticks=";
constexpr std::string_view kInventoryShiftPrefix = "--inventory-shift-ticks=";
constexpr std::string_view kRefreshPrefix = "--refresh-price-ticks=";
constexpr std::string_view kEdgeCoverPrefix = "--edge-cover-ticks=";
constexpr std::string_view kMinSpreadPrefix = "--min-spread-ticks=";
constexpr std::string_view kQueueChunksPrefix = "--queue-chunks=";
constexpr std::string_view kQueueSeedPrefix = "--queue-seed=";
constexpr std::string_view kArrivalSharePrefix = "--arrival-share=";
constexpr std::string_view kOrderLatencyPrefix = "--order-latency-us=";
constexpr std::string_view kCancelLatencyPrefix = "--cancel-latency-us=";
constexpr std::string_view kTraceOrderPrefix = "--trace-order=";
constexpr std::string_view kFundingRatesPrefix = "--funding-rates=";
constexpr std::string_view kSymbolAPrefix = "--symbol-a=";
constexpr std::string_view kSymbolBPrefix = "--symbol-b=";
// The second book of an l2_replay run. `--symbol-b` is shared with the carry mode, which is why it is declared
// above with the carry flags rather than here: one name, one symbol, one meaning.
constexpr std::string_view kTradesBPrefix = "--trades-b=";
constexpr std::string_view kDepthBPrefix = "--depth-b=";
// The two halves of a session that another process can take over: where the state is written, and where a run
// reads one to resume from.
constexpr std::string_view kSnapshotOutPrefix = "--snapshot-out=";
constexpr std::string_view kSnapshotInPrefix = "--snapshot-in=";
// Where a run writes the order leg -- what went out and what came back -- as <prefix>_orders.csv.
constexpr std::string_view kRecordOrdersPrefix = "--record-orders=";
constexpr std::string_view kReplayOrdersPrefix = "--replay-orders=";
// The live order leg: ADR 0005 decision 3's second opt-in, the socket it reaches, and the adapter's own cap. The
// cap is a required companion rather than a default, because decision 4 puts a bound below the strategy's controls
// and a live leg with no bound is the case that decision forbids.
constexpr std::string_view kLiveOrdersFlag = "--live-orders";
constexpr std::string_view kVenueEndpointPrefix = "--venue-endpoint=";
constexpr std::string_view kVenueMaxOrdersPrefix = "--venue-max-orders=";
constexpr std::string_view kVenueMaxNotionalPrefix = "--venue-max-notional=";
constexpr std::string_view kNotionalPrefix = "--notional-usd=";
constexpr std::string_view kLongPrefix = "--long=";
constexpr std::string_view kMakerFeePrefix = "--maker-fee-rate=";
constexpr std::string_view kFeeSchedulePrefix = "--fee-schedule=";
constexpr std::string_view kMarkoutCoverFlag = "--markout-cover";
constexpr std::string_view kMetricsFlag = "--metrics";
constexpr std::string_view kMarkoutHorizonPrefix = "--markout-horizon=";
constexpr std::string_view kParentPrefix = "--parent=";
constexpr std::string_view kAlgoPrefix = "--algo=";
constexpr std::string_view kAlgoEntryPrefix = "--algo-entry=";
constexpr std::string_view kFlattenBandPrefix = "--flatten-band=";
}  // namespace

std::string to_abs_path(const std::string& input_path)
{
    namespace fs = std::filesystem;
    fs::path p(input_path);
    if (p.is_absolute()) return p.string();
    return (fs::path(PROJECT_ROOT_DIR) / p).string();
}

bool parse_integer(const std::string& value, int& result)
{
    if (value.empty()) return false;

    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_unsigned(const std::string& value, uint64_t& result)
{
    if (value.empty()) return false;

    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_time_in_force(const std::string& value, TimeInForce& result)
{
    if (value == "gtc") result = TimeInForce::Gtc;
    else if (value == "ioc") result = TimeInForce::Ioc;
    else if (value == "fok") result = TimeInForce::Fok;
    else return false;
    return true;
}

bool parse_queue_model(const std::string& value, QueueModel& result)
{
    if (value == "conservative") result = QueueModel::Conservative;
    else if (value == "prorata") result = QueueModel::ProRata;
    else if (value == "lumpy") result = QueueModel::Lumpy;
    else if (value == "optimistic") result = QueueModel::Optimistic;
    else return false;
    return true;
}

void print_usage(const char* executable)
{
    std::cerr << "Usage: " << executable << " csv [path_to_csv] [ms_delay] [strategy]\n";
    std::cerr << "   or: " << executable << " udp <port> [strategy]\n";
        std::cerr << "   or: " << executable
              << " replay <agg_trades_csv> <bbo_csv> <symbol> [ms_delay] [strategy] "
                  "[quantity_scale] [queue_model] [--no-output] [--fast-validation]\n";
        std::cerr << "   or: " << executable
                      << " l2_replay <trades_csv[.gz]> <depth_or_incremental_csv> <symbol> "
                         "[ms_delay] [strategy] [quantity_scale] [queue_model] [--no-output] "
                         "[--fast-validation] [--profile-stages[=interval]] [--live-pacing[=speed]]\n";
        std::cerr << "   or: " << executable
                      << " ws <host:port> <symbol> [strategy] [quantity_scale] [queue_model] "
                         "[--ws-target=/path] [--ws-seconds=N] [--ws-ring-capacity=N] "
                         "[--idle-wait-us=N] [--no-output] [--fast-validation] "
                         "[--record=PREFIX] [--feed-thread] [--paper-broker]; the live twin of "
                         "l2_replay, reading the same replay shapes over a websocket instead of from "
                         "files\n";
        std::cerr << "   or: " << executable
                      << " carry <leg_a_funding_csv> <leg_b_funding_csv> --symbol-a=SYM --symbol-b=SYM "
                         "[--notional-usd=N] [--long=b]\n";
        std::cerr << "   second book: --symbol-b=SYM --trades-b=FILE --depth-b=FILE (l2_replay only) "
                     "replays two instruments into one account -- a book, a decision layer and a venue per "
                     "symbol, interleaved by timestamp -- and the [EXPOSURE] line then reports the two legs' "
                     "gross exposure against their net\n";
        std::cerr << "   session: --snapshot-out=FILE writes the session -- account, positions, marks, working "
                     "orders, clock, next order id -- when the run ends, and --snapshot-in=FILE takes one over: the "
                     "account is restored, every order the snapshot listed is asked about at the venue (a live "
                     "one is cancelled, a forgotten one is counted), and the run replays its file from the first "
                     "line while holding its own orders until the snapshot's clock\n";
        std::cerr << "   end of session: --close-out flattens whatever the run is holding at the end of the input, "
                     "at the touch, and prints [WIND-DOWN] with the half spread and the taker fee the exit cost in "
                     "basis points. Without it a run that stops with a position has marked it at the last midpoint "
                     "rather than closed it, and its [PNL] is that mark\n";
        std::cerr << "   order leg: --record-orders=PREFIX writes <prefix>_orders.csv -- every submission, cancel and "
                     "execution report of the session, in order -- which `RecordedVenue` replays as the venue; "
                     "without it nothing is written and no number moves\n";
        std::cerr << "   order replay: --replay-orders=PREFIX makes <prefix>_orders.csv the run's venue and releases "
                     "each recorded report on the clock it was recorded with, so a session is replayed rather than "
                     "believed; it is l2_replay and ws only, and refused with --live-orders and --record-orders\n";
        std::cerr << "   live order leg: --live-orders --venue-endpoint=HOST:PORT --venue-max-orders=N makes the venue "
                     "the relay's order channel instead of the in-process matcher (l2_replay and ws only); the three "
                     "are required together, and the cap is the adapter's own, below the strategy's limits. "
                     "--venue-max-notional=USD adds the cap a reader can price: one order's notional, computed with "
                     "the accounting's own helper\n";
        std::cerr << "   optional flags: --no-output | --fast-validation (l2_replay only) | "
                     "--profile-stages[=N] prints sampled per-stage timings (l2_replay only, default interval 64) | "
                     "--order-size=N, --inventory-limit=N, and --max-position=N override the "
                     "values derived from the instrument quantity scale | --base-spread-ticks=N "
                     "sets the quote half-spread, 1 being the venue touch\n";
        std::cerr << "   overrides: --inventory-shift-ticks=N shifts the inventory-aware quotes by N ticks "
                     "at a full position, --refresh-price-ticks=N is how far the strategy's own fair price "
                     "has to move before its resting quotes are pulled and replaced, --edge-cover-ticks=N "
                     "requires a quote to cover N ticks on top of the maker fee before it is placed, and "
                     "--min-spread-ticks=N quotes only where the touch is at least N ticks wide; 0 leaves "
                     "each of them at the strategy's own setting\n";
        std::cerr << "   limits: --max-exposure-usd=N is the exposure ceiling the summary measures the run "
                     "against, enforced rather than reported once it is named, and --kill-switch-loss-usd=N "
                     "latches when the loss reaches N dollars, closes the position it was holding at the "
                     "touch and refuses every order after that\n";
        std::cerr << "   risk: --fat-finger-bps=X refuses an order priced more than X basis points from the "
                     "current midpoint, and --max-orders-per-second=X with --order-burst=N caps submissions "
                     "with a token bucket; 0 leaves each of them off\n";
        std::cerr << "   capacity: --participation-limit=X caps one aggressive external-BBO fill at X of the "
                     "displayed best size; 1.0 is off, and the execution price remains the touch\n";
        std::cerr << "   time in force: --time-in-force=gtc|ioc|fok applies to every strategy order; gtc is "
                     "the default, ioc cancels any immediate remainder, and fok cancels without trading "
                     "unless the whole quantity is available; --post-only is the venue's side of the same "
                     "question: an order that would trade on arrival is cancelled instead, so a quote that "
                     "arrived after the market went through it pays nothing rather than the taker rate\n";
        std::cerr << "   trend: --trend-params=FAST,SLOW,ENTRY_TICKS,EXIT_TICKS,STOP_TICKS[,COOLDOWN_VIEWS"
                     "[,HOLDING_VIEWS]] sets the non-market-making signal; the view counts are decisions and "
                     "the bands are ticks\n";
        std::cerr << "   strategy: ";
        for (const std::string_view name : kStrategyNames)
        {
            if (name != kStrategyNames.front()) std::cerr << " | ";
            std::cerr << name;
            if (name == kDefaultStrategyName) std::cerr << " (default)";
        }
        std::cerr << "\n";
        std::cerr << "   queue_model: prorata (default, calibrated) | lumpy (same mean, real variance) | "
                     "conservative (lower bound) | optimistic (upper bound)\n";
        std::cerr << "   queue: --queue-chunks=N is how many chunks a level decrease is split into when the "
                     "lumpy model draws from it, and --queue-seed=N is that generator's seed, fixed by "
                     "default so a lumpy run is reproducible\n";
        std::cerr << "   arrival: --arrival-share=X sets the share of the displayed size a new quote "
                     "starts behind; 1.0 is FIFO\n";
        std::cerr << "   funding: --funding-rates=FILE applies the instrument's funding history as the "
                     "replay passes each settlement, reported as a [FUNDING] line\n";
        std::cerr << "   schedule: --fee-schedule=KEY=VALUE[,KEY=VALUE] (l2_replay) prices a run under a "
                     "broker's own schedule, where the keys are min (a floor per order, in USD), regulatory "
                     "(a fraction of the proceeds of a sale), clearing and audit (USD a share on every "
                     "execution), rebate (USD a share of credit for adding liquidity) and commission (USD a "
                     "share instead of the instrument's own, for a broker's monthly volume tier); every one of "
                     "them is zero unless named, and the [Mode] line reports the terms in force";
        std::cerr << "\n";
        std::cerr << "   fees: --maker-fee-rate=R replaces the venue's maker rate for a sensitivity "
                     "measurement, and the run reports it in the [Mode] line\n";
        std::cerr << "   gate: --markout-cover (l2_replay, active_l2) quotes only where the capture also "
                     "covers what passive fills at that distance have cost, measured from the venue's "
                     "prints, and reports [GATE]\n";
        std::cerr << "   metrics: --metrics samples the marked equity once per 60-second bucket and reports "
                     "[PERFORMANCE]: drawdown, its duration, annualized volatility, Sharpe, Sortino and "
                     "Calmar\n";
        std::cerr << "   horizon: --markout-horizon=N resolves the fill markouts and the implementation "
                     "shortfall at N quote cycles instead of the default 5\n";
        std::cerr << "   execution: --parent=SIDE:QUANTITY:SECONDS (l2_replay, execution_l2) works one parent "
                     "order instead of quoting; --algo=market|twap:N|pov:X picks the schedule, "
                     "--algo-entry=cross|post:SECONDS picks how a slice is sent, and the run reports "
                     "[EXECUTION_ALGO]\n";
        std::cerr << "   band: --flatten-band=N (l2_replay) reduces a position above N units with a taker "
                     "order at the touch instead of carrying it into the final mark\n";
        std::cerr << "   record: --record=PREFIX (l2_replay) writes PREFIX_trades.csv and "
                     "PREFIX_depth.csv in the replay shapes, so the stream this run was fed can be "
                     "replayed again\n";
        std::cerr << "   trace: --trace-order=N reports the pipeline's per-order hints for order N, which "
                     "is how one order's decisions are read back\n";
        std::cerr << "   threads: --feed-thread (l2_replay) reads on its own thread and hands each "
                     "event to the strategy thread over a ring, reporting the hand-off as a "
                     "[LATENCY] line; --ring-capacity=N sizes that ring (default 4096)\n";
        std::cerr << "   latency: --order-latency-us=N and --cancel-latency-us=N hold an order or a cancel "
                     "for N microseconds of the market's own clock before the venue sees it, which is what "
                     "being slow costs and the one part of the order lifecycle a replay can model "
                     "directly\n";
        std::cerr << "   broker: --paper-broker (l2_replay) puts a broker between the strategy and the "
                     "venue that keeps its own statement and reports [BROKER] and [RECON]; "
                     "--broker-reject-every=N refuses every Nth order, --broker-partial-every=N splits "
                     "every Nth fill, --broker-drop-every=N withholds every Nth fill from the strategy, "
                     "and --broker-outage-after=N with --broker-outage-holds=M holds the next M orders "
                     "while the venue is unreachable\n";
        std::cerr << "   gateway: --order-gateway (l2_replay) puts the client side of the order path "
                     "outside the broker, reports [GATEWAY], and keeps the run's own record of every "
                     "order from submission to answer; --gateway-ack-timeout-us=N counts an order the "
                     "venue has not answered within N microseconds of market time, and "
                     "--gateway-retry-timeouts sends a timed-out order again under the same client id, "
                     "which the venue refuses as a duplicate\n";
        std::cerr << "   --help prints this text\n";
}

bool parse_config(int argc, char** argv, AppConfig& cfg, std::string& error)
{
    if (argc == 2 && std::string(argv[1]) == "--help") return false;

    const std::string mode = argc >= 2 ? argv[1] : "csv";
    if (mode == "csv")
        cfg.mode = AppMode::LegacyCsv;
    else if (mode == "udp")
        cfg.mode = AppMode::LegacyUdp;
    else if (mode == "replay")
        cfg.mode = AppMode::Replay;
    else if (mode == "l2_replay")
        cfg.mode = AppMode::L2Replay;
    else if (mode == "carry")
        cfg.mode = AppMode::Carry;
    else if (mode == "ws")
        cfg.mode = AppMode::WebSocket;
    else
    {
        error = "mode must be 'csv', 'udp', 'replay', 'l2_replay', 'ws', or 'carry'";
        return false;
    }

    // The two L2 delivery modes. They share every optional flag, the book, the engine, the recorder
    // and the paper broker; they differ only in whether the readers come from files or from a socket,
    // which is why the flag block below treats them as one family rather than naming each flag twice.
    const bool is_l2_family =
        cfg.mode == AppMode::L2Replay || cfg.mode == AppMode::WebSocket;

    if (cfg.mode == AppMode::Replay || is_l2_family)
    {
        // The optional positional arguments differ between the two sources -- a replay takes two files
        // and a symbol, a live session a host:port and a symbol -- so they are parsed here, and every
        // optional flag after them is parsed once, below, for both.
        int first_optional_flag = 0;
        if (cfg.mode == AppMode::WebSocket)
        {
            // ws <host:port> <symbol> [strategy] [quantity_scale] [queue_model]
            if (argc < 4)
            {
                error = "ws requires a host:port endpoint and a symbol";
                return false;
            }
            const std::string endpoint = argv[2];
            const std::size_t colon = endpoint.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size())
            {
                error = "ws endpoint must be host:port";
                return false;
            }
            cfg.ws_host = endpoint.substr(0, colon);
            int port = 0;
            if (!parse_integer(endpoint.substr(colon + 1), port) || port < 1 || port > 65535)
            {
                error = "ws port must be an integer from 1 to 65535";
                return false;
            }
            cfg.ws_port = static_cast<uint16_t>(port);
            cfg.path_or_port = endpoint;  // kept for the runtime header and the [WS] line
            cfg.symbol = argv[3];
            if (cfg.symbol.empty())
            {
                error = "ws symbol cannot be empty";
                return false;
            }
            first_optional_flag = 4;
            if (argc >= 5 && argv[4][0] != '-')
            {
                cfg.strategy_name = argv[4];
                first_optional_flag = 5;
            }
            if (argc >= 6 && argv[5][0] != '-')
            {
                if (!parse_unsigned(argv[5], cfg.quantity_scale) || cfg.quantity_scale == 0)
                {
                    error = "quantity scale must be a positive integer";
                    return false;
                }
                first_optional_flag = 6;
            }
            if (argc >= 7 && argv[6][0] != '-')
            {
                if (!parse_queue_model(argv[6], cfg.queue_model))
                {
                    error = "queue model must be conservative, prorata, lumpy, or optimistic";
                    return false;
                }
                first_optional_flag = 7;
            }
        }
        else
        {
            // No upper bound on the argument count: every optional flag is validated as it is parsed,
            // so capping the total only rejected valid commands with a misleading message once the
            // flags multiplied, which is exactly what happened when two were added at once.
            if (argc < 5)
            {
                error = "replay requires trade file, market-state file, and symbol";
                return false;
            }
            cfg.path_or_port = argv[2];
            cfg.quotes_path = argv[3];
            cfg.symbol = argv[4];
            if (!std::filesystem::is_regular_file(to_abs_path(cfg.path_or_port)) ||
                !std::filesystem::is_regular_file(to_abs_path(cfg.quotes_path)))
            {
                error = "replay input file does not exist";
                return false;
            }
            if (argc >= 6 && (!parse_integer(argv[5], cfg.delay) || cfg.delay < 0))
            {
                error = "delay must be a non-negative integer";
                return false;
            }
            if (argc >= 7) cfg.strategy_name = argv[6];
            if (argc >= 8 &&
                (!parse_unsigned(argv[7], cfg.quantity_scale) || cfg.quantity_scale == 0))
            {
                error = "quantity scale must be a positive integer";
                return false;
            }
            if (argc >= 8) cfg.has_quantity_scale = true;
            // The queue model is optional, so an argument starting with -- is a flag rather than a
            // model name. Without this, `... active_l2 1 --fast-validation` was read as the model
            // being \"--fast-validation\" and failed, which made the optional positional impossible to
            // omit.
            first_optional_flag = 8;
            if (argc >= 9 && argv[8][0] != '-')
            {
                if (!parse_queue_model(argv[8], cfg.queue_model))
                {
                    error = "queue model must be conservative, prorata, lumpy, or optimistic";
                    return false;
                }
                first_optional_flag = 9;
            }
        }
        // The broker's faults are all "every Nth order or fill" counts, so one parser serves them. Each
        // needs the broker itself: without it the number would be silently ignored, which is worse than
        // being refused.
        const auto parse_fault_count = [&cfg, &error, is_l2_family](const std::string& flag,
                                                                    std::string_view prefix,
                                                                    const char* name,
                                                                    uint64_t& target) -> bool
        {
            if (!is_l2_family)
            {
                error = std::string(name) + " is an l2_replay or ws flag, beside --paper-broker";
                return false;
            }
            uint64_t count = 0;
            if (!parse_unsigned(flag.substr(prefix.size()), count) || count == 0)
            {
                error = std::string(name) + " must be a positive integer";
                return false;
            }
            target = count;
            return true;
        };
        for (int argument = first_optional_flag; argument < argc; ++argument)
        {
            const std::string flag = argv[argument];
            if (flag == "--no-output")
                cfg.discard_output = true;
            else if (flag.compare(0, kSymbolBPrefix.size(), kSymbolBPrefix) == 0)
                cfg.symbol_b = flag.substr(kSymbolBPrefix.size());
            else if (flag.compare(0, kTradesBPrefix.size(), kTradesBPrefix) == 0)
                cfg.trades_b_path = flag.substr(kTradesBPrefix.size());
            else if (flag.compare(0, kDepthBPrefix.size(), kDepthBPrefix) == 0)
                cfg.depth_b_path = flag.substr(kDepthBPrefix.size());
            else if (flag.compare(0, kSnapshotOutPrefix.size(), kSnapshotOutPrefix) == 0)
                cfg.snapshot_out_path = flag.substr(kSnapshotOutPrefix.size());
            else if (flag.compare(0, kSnapshotInPrefix.size(), kSnapshotInPrefix) == 0)
                cfg.snapshot_in_path = flag.substr(kSnapshotInPrefix.size());
            else if (flag.compare(0, kRecordOrdersPrefix.size(), kRecordOrdersPrefix) == 0)
                cfg.order_log_path = flag.substr(kRecordOrdersPrefix.size());
            else if (flag.compare(0, kReplayOrdersPrefix.size(), kReplayOrdersPrefix) == 0)
                cfg.order_replay_path = flag.substr(kReplayOrdersPrefix.size());
            else if (flag == kLiveOrdersFlag)
                cfg.live_orders = true;
            else if (flag.compare(0, kVenueEndpointPrefix.size(), kVenueEndpointPrefix) == 0)
                cfg.venue_endpoint = flag.substr(kVenueEndpointPrefix.size());
            else if (flag.compare(0, kVenueMaxOrdersPrefix.size(), kVenueMaxOrdersPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kVenueMaxOrdersPrefix.size()), cfg.venue_max_orders) ||
                    cfg.venue_max_orders == 0)
                {
                    error = "--venue-max-orders must be a positive number of orders";
                    return false;
                }
            }
            else if (flag.compare(0, kVenueMaxNotionalPrefix.size(), kVenueMaxNotionalPrefix) == 0)
            {
                std::istringstream stream(flag.substr(kVenueMaxNotionalPrefix.size()));
                double usd = 0.0;
                if (!(stream >> usd) || !(usd > 0.0))
                {
                    error = "--venue-max-notional must be a positive number of USD";
                    return false;
                }
                cfg.venue_max_notional_usd = usd;
            }
            else if (flag == kMarkoutCoverFlag)
                cfg.markout_cover = true;
            else if (flag == kMetricsFlag)
                cfg.metrics = true;
            else if (flag.compare(0, kMarkoutHorizonPrefix.size(), kMarkoutHorizonPrefix) == 0)
            {
                uint64_t cycles = 0;
                if (!parse_unsigned(flag.substr(kMarkoutHorizonPrefix.size()), cycles) || cycles == 0)
                {
                    error = "--markout-horizon must be a positive integer";
                    return false;
                }
                cfg.markout_horizon = cycles;
            }
            else if (flag.compare(0, kParentPrefix.size(), kParentPrefix) == 0)
            {
                // SIDE:QUANTITY:SECONDS in one token, because the three numbers are one decision: a side
                // without a size or a size without a deadline is not a parent order.
                const std::string value = flag.substr(kParentPrefix.size());
                const std::size_t first = value.find(':');
                const std::size_t second =
                    first == std::string::npos ? std::string::npos : value.find(':', first + 1);
                const std::string side_text =
                    first == std::string::npos ? value : value.substr(0, first);
                const std::string quantity_text = second == std::string::npos
                                                      ? std::string()
                                                      : value.substr(first + 1, second - first - 1);
                const std::string seconds_text =
                    second == std::string::npos ? std::string() : value.substr(second + 1);
                uint64_t quantity = 0;
                uint64_t seconds = 0;
                Side side = Side::Unknown;
                if (side_text == "buy")
                    side = Side::Buy;
                else if (side_text == "sell")
                    side = Side::Sell;
                if (side == Side::Unknown || !parse_unsigned(quantity_text, quantity) || quantity == 0 ||
                    !parse_unsigned(seconds_text, seconds) || seconds == 0)
                {
                    error = "--parent wants SIDE:QUANTITY:SECONDS, with a side of buy or sell and two positive "
                            "integers";
                    return false;
                }
                cfg.has_parent = true;
                cfg.parent_side = side;
                cfg.parent_quantity = quantity;
                cfg.parent_horizon_us = seconds * 1'000'000;
            }
            else if (flag.compare(0, kAlgoPrefix.size(), kAlgoPrefix) == 0)
            {
                // The schedule and its one parameter arrive in the same token, so a run cannot name a schedule
                // without the number it needs or pass a number to a schedule that has no use for it.
                const std::string value = flag.substr(kAlgoPrefix.size());
                const std::size_t colon = value.find(':');
                const std::string name = colon == std::string::npos ? value : value.substr(0, colon);
                const std::string param =
                    colon == std::string::npos ? std::string() : value.substr(colon + 1);
                if (name == "market")
                {
                    cfg.algo = "market";
                }
                else if (name == "twap")
                {
                    uint64_t slices = 0;
                    if (!parse_unsigned(param, slices) || slices == 0)
                    {
                        error = "--algo=twap wants a positive slice count, for example --algo=twap:10";
                        return false;
                    }
                    cfg.algo = "twap";
                    cfg.algo_slices = slices;
                }
                else if (name == "pov")
                {
                    double participation = 0.0;
                    std::istringstream stream(param);
                    if (!(stream >> participation) || participation <= 0.0 || participation > 1.0)
                    {
                        error = "--algo=pov wants a share of traded volume in (0, 1], for example --algo=pov:0.1";
                        return false;
                    }
                    cfg.algo = "pov";
                    cfg.algo_participation = participation;
                }
                else
                {
                    error = "--algo must be market, twap:N or pov:X";
                    return false;
                }
            }
            else if (flag.compare(0, kAlgoEntryPrefix.size(), kAlgoEntryPrefix) == 0)
            {
                // The entry and its one parameter, in the same token as the schedule: `cross` has none, and
                // `post` needs the timeout that decides when to give up waiting for the queue.
                const std::string value = flag.substr(kAlgoEntryPrefix.size());
                const std::size_t colon = value.find(':');
                const std::string name = colon == std::string::npos ? value : value.substr(0, colon);
                const std::string param =
                    colon == std::string::npos ? std::string() : value.substr(colon + 1);
                if (name == "cross")
                {
                    // Crossing has no parameter, and a number here would be silently ignored: refuse it the way
                    // every other flag refuses a shape it does not use.
                    if (!param.empty())
                    {
                        error = "--algo-entry=cross takes no parameter";
                        return false;
                    }
                    cfg.algo_entry = "cross";
                }
                else if (name == "post")
                {
                    uint64_t seconds = 0;
                    if (!parse_unsigned(param, seconds) || seconds == 0)
                    {
                        error = "--algo-entry=post wants a positive timeout in seconds, for example "
                                "--algo-entry=post:5";
                        return false;
                    }
                    cfg.algo_entry = "post";
                    cfg.algo_post_timeout_seconds = seconds;
                }
                else
                {
                    error = "--algo-entry must be cross or post:SECONDS";
                    return false;
                }
            }
            else if (flag.compare(0, kFlattenBandPrefix.size(), kFlattenBandPrefix) == 0)
            {
                uint64_t band = 0;
                if (!parse_unsigned(flag.substr(kFlattenBandPrefix.size()), band) || band == 0)
                {
                    error = "--flatten-band must be a positive integer";
                    return false;
                }
                cfg.flatten_band = static_cast<int64_t>(band);
            }
            else if (flag == "--fast-validation")
                cfg.fast_validation = true;
            else if (flag == "--profile-stages")
                cfg.profile_sample_interval = StageProfiler::kDefaultSampleInterval;
            else if (flag == "--live-pacing")
            {
                cfg.live_pacing = true;
                cfg.live_pacing_speed = 1.0;
            }
            else if (flag.compare(0, kLivePacingPrefix.size(), kLivePacingPrefix) == 0)
            {
                const std::string value = flag.substr(kLivePacingPrefix.size());
                try
                {
                    cfg.live_pacing_speed = std::stod(value);
                }
                catch (const std::exception&)
                {
                    error = "--live-pacing speed must be a number";
                    return false;
                }
                if (!(cfg.live_pacing_speed > 0.0))
                {
                    error = "--live-pacing speed must be positive";
                    return false;
                }
                cfg.live_pacing = true;
            }
            else if (flag.compare(0, kRecordPrefix.size(), kRecordPrefix) == 0)
            {
                // The recorder writes the two streams the L2 replay reads: a trade file and a depth
                // file. The L1 replay's second stream is a BBO quote, and no replay CSV in this
                // project holds one as rows, so asking to record there is refused rather than
                // silently ignored.
                if (!is_l2_family)
                {
                    error = "--record is an l2_replay or ws flag: the L1 replay's second stream is a "
                            "BBO quote, which the replay CSV formats cannot carry as rows";
                    return false;
                }
                if (flag.size() == kRecordPrefix.size())
                {
                    error = "--record needs a file prefix";
                    return false;
                }
                cfg.record_prefix = flag.substr(kRecordPrefix.size());
            }
            else if (flag == kFeedThreadFlag)
            {
                if (!is_l2_family)
                {
                    error = "--feed-thread is an l2_replay or ws flag: the L1 replay's second stream "
                            "is a BBO quote, and a quote is not what this hand-off carries";
                    return false;
                }
                cfg.feed_thread = true;
            }
            else if (flag.compare(0, kWebSocketTargetPrefix.size(), kWebSocketTargetPrefix) == 0)
            {
                if (cfg.mode != AppMode::WebSocket)
                {
                    error = "--ws-target is a ws flag: it is the HTTP path the live handshake asks for";
                    return false;
                }
                const std::string target = flag.substr(kWebSocketTargetPrefix.size());
                if (target.empty() || target.front() != '/')
                {
                    error = "--ws-target must be a path beginning with /";
                    return false;
                }
                cfg.ws_target = target;
            }
            else if (flag.compare(0, kWebSocketSecondsPrefix.size(), kWebSocketSecondsPrefix) == 0)
            {
                if (cfg.mode != AppMode::WebSocket)
                {
                    error = "--ws-seconds is a ws flag: it bounds a live session, which a file replay "
                            "does not need";
                    return false;
                }
                std::istringstream stream(flag.substr(kWebSocketSecondsPrefix.size()));
                if (!(stream >> cfg.ws_seconds) || !(cfg.ws_seconds > 0.0))
                {
                    error = "--ws-seconds must be a positive number of seconds";
                    return false;
                }
            }
            else if (flag.compare(0, kWebSocketRingPrefix.size(), kWebSocketRingPrefix) == 0)
            {
                if (cfg.mode != AppMode::WebSocket)
                {
                    error = "--ws-ring-capacity is a ws flag: it sizes the live feed's input ring";
                    return false;
                }
                uint64_t capacity = 0;
                if (!parse_unsigned(flag.substr(kWebSocketRingPrefix.size()), capacity) || capacity == 0)
                {
                    error = "--ws-ring-capacity must be a positive integer";
                    return false;
                }
                cfg.ws_ring_capacity = static_cast<std::size_t>(capacity);
            }
            else if (flag.compare(0, kIdleWaitPrefix.size(), kIdleWaitPrefix) == 0)
            {
                if (cfg.mode != AppMode::WebSocket)
                {
                    error = "--idle-wait-us is a ws flag: it is how a live reader waits on an empty stream";
                    return false;
                }
                uint64_t wait = 0;
                if (!parse_unsigned(flag.substr(kIdleWaitPrefix.size()), wait) || wait == 0)
                {
                    error = "--idle-wait-us must be a positive integer";
                    return false;
                }
                cfg.idle_wait_us = static_cast<int64_t>(wait);
            }
            else if (flag.compare(0, kRingCapacityPrefix.size(), kRingCapacityPrefix) == 0)
            {
                if (!is_l2_family)
                {
                    error = "--ring-capacity is an l2_replay or ws flag, beside --feed-thread";
                    return false;
                }
                uint64_t capacity = 0;
                if (!parse_unsigned(flag.substr(kRingCapacityPrefix.size()), capacity) || capacity == 0)
                {
                    error = "--ring-capacity must be a positive integer";
                    return false;
                }
                cfg.ring_capacity = static_cast<std::size_t>(capacity);
            }
            else if (flag == kPaperBrokerFlag)
            {
                if (!is_l2_family)
                {
                    error = "--paper-broker is an l2_replay or ws flag: it wraps that mode's engine";
                    return false;
                }
                cfg.paper_broker = true;
            }
            else if (flag.compare(0, kBrokerRejectPrefix.size(), kBrokerRejectPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerRejectPrefix, "--broker-reject-every",
                                       cfg.broker_reject_every))
                    return false;
            }
            else if (flag.compare(0, kBrokerPartialPrefix.size(), kBrokerPartialPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerPartialPrefix, "--broker-partial-every",
                                       cfg.broker_partial_every))
                    return false;
            }
            else if (flag.compare(0, kBrokerDropPrefix.size(), kBrokerDropPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerDropPrefix, "--broker-drop-every",
                                       cfg.broker_drop_every))
                    return false;
            }
            else if (flag.compare(0, kBrokerOutageAfterPrefix.size(), kBrokerOutageAfterPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerOutageAfterPrefix, "--broker-outage-after",
                                       cfg.broker_outage_after))
                    return false;
            }
            else if (flag.compare(0, kBrokerOutageHoldsPrefix.size(), kBrokerOutageHoldsPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerOutageHoldsPrefix, "--broker-outage-holds",
                                       cfg.broker_outage_holds))
                    return false;
            }
            else if (flag == kOrderGatewayFlag)
            {
                if (!is_l2_family)
                {
                    error = "--order-gateway is an l2_replay or ws flag: it wraps that mode's venue";
                    return false;
                }
                cfg.order_gateway = true;
            }
            else if (flag.compare(0, kGatewayAckTimeoutPrefix.size(), kGatewayAckTimeoutPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kGatewayAckTimeoutPrefix.size()),
                                    cfg.gateway_ack_timeout_us) ||
                    cfg.gateway_ack_timeout_us == 0)
                {
                    error = "--gateway-ack-timeout-us must be a positive integer";
                    return false;
                }
            }
            else if (flag == kGatewayRetryTimeoutFlag)
            {
                if (!is_l2_family)
                {
                    error = "--gateway-retry-timeouts is an l2_replay or ws flag, beside --order-gateway";
                    return false;
                }
                cfg.gateway_retry_on_timeout = true;
            }
            else if (flag.compare(0, kProfileStagesPrefix.size(), kProfileStagesPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kProfileStagesPrefix.size()),
                                    cfg.profile_sample_interval) ||
                    cfg.profile_sample_interval == 0)
                {
                    error = "--profile-stages interval must be a positive integer";
                    return false;
                }
            }
            else if (flag.compare(0, kOrderSizePrefix.size(), kOrderSizePrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kOrderSizePrefix.size()), cfg.order_size_override) ||
                    cfg.order_size_override == 0)
                {
                    error = "--order-size must be a positive integer";
                    return false;
                }
            }
            else if (flag.compare(0, kInventoryLimitPrefix.size(), kInventoryLimitPrefix) == 0)
            {
                uint64_t limit = 0;
                if (!parse_unsigned(flag.substr(kInventoryLimitPrefix.size()), limit) || limit == 0)
                {
                    error = "--inventory-limit must be a positive integer";
                    return false;
                }
                cfg.inventory_limit_override = static_cast<int64_t>(limit);
            }
            else if (flag.compare(0, kMaxPositionPrefix.size(), kMaxPositionPrefix) == 0)
            {
                uint64_t limit = 0;
                if (!parse_unsigned(flag.substr(kMaxPositionPrefix.size()), limit) || limit == 0)
                {
                    error = "--max-position must be a positive integer";
                    return false;
                }
                cfg.position_limit = static_cast<int64_t>(limit);
            }
            else if (flag.compare(0, kMaxExposurePrefix.size(), kMaxExposurePrefix) == 0)
            {
                double exposure = 0.0;
                std::istringstream stream(flag.substr(kMaxExposurePrefix.size()));
                if (!(stream >> exposure) || !(exposure > 0.0))
                {
                    error = "--max-exposure-usd must be a positive number of US dollars";
                    return false;
                }
                cfg.max_exposure_usd = exposure;
            }
            else if (flag.compare(0, kKillSwitchPrefix.size(), kKillSwitchPrefix) == 0)
            {
                double loss = 0.0;
                std::istringstream stream(flag.substr(kKillSwitchPrefix.size()));
                if (!(stream >> loss) || !(loss > 0.0))
                {
                    error = "--kill-switch-loss-usd must be a positive number of US dollars";
                    return false;
                }
                cfg.kill_switch_loss_usd = loss;
            }
            else if (flag.compare(0, kFatFingerPrefix.size(), kFatFingerPrefix) == 0)
            {
                double bps = 0.0;
                std::istringstream stream(flag.substr(kFatFingerPrefix.size()));
                if (!(stream >> bps) || !(bps > 0.0))
                {
                    error = "--fat-finger-bps must be a positive number of basis points";
                    return false;
                }
                cfg.fat_finger_bps = bps;
            }
            else if (flag.compare(0, kMaxOrdersPerSecondPrefix.size(), kMaxOrdersPerSecondPrefix) == 0)
            {
                double orders = 0.0;
                std::istringstream stream(flag.substr(kMaxOrdersPerSecondPrefix.size()));
                if (!(stream >> orders) || !(orders > 0.0))
                {
                    error = "--max-orders-per-second must be a positive number";
                    return false;
                }
                cfg.max_orders_per_second = orders;
            }
            else if (flag.compare(0, kOrderBurstPrefix.size(), kOrderBurstPrefix) == 0)
            {
                uint64_t burst = 0;
                if (!parse_unsigned(flag.substr(kOrderBurstPrefix.size()), burst) || burst == 0)
                {
                    error = "--order-burst must be a positive integer";
                    return false;
                }
                cfg.order_burst = burst;
            }
            else if (flag.compare(0, kParticipationLimitPrefix.size(), kParticipationLimitPrefix) == 0)
            {
                double limit = 0.0;
                std::istringstream stream(flag.substr(kParticipationLimitPrefix.size()));
                if (!(stream >> limit) || !(limit > 0.0) || limit > 1.0)
                {
                    error = "--participation-limit must be greater than 0 and at most 1";
                    return false;
                }
                cfg.participation_limit = limit;
            }
            else if (flag.compare(0, kTimeInForcePrefix.size(), kTimeInForcePrefix) == 0)
            {
                if (!parse_time_in_force(flag.substr(kTimeInForcePrefix.size()), cfg.time_in_force))
                {
                    error = "--time-in-force must be gtc, ioc, or fok";
                    return false;
                }
                cfg.time_in_force_set = true;
            }
            else if (flag == kPostOnlyFlag)
                cfg.post_only = true;
            else if (flag == kCloseOutFlag)
                cfg.close_out = true;
            else if (flag.compare(0, kTrendParamsPrefix.size(), kTrendParamsPrefix) == 0)
            {
                const std::string text = flag.substr(kTrendParamsPrefix.size());
                std::vector<double> values;
                std::istringstream items(text);
                std::string piece;
                while (std::getline(items, piece, ','))
                {
                    std::istringstream item(piece);
                    double value = 0.0;
                    item >> value;
                    // A number extracted at the end of the piece already sets eofbit; asking std::ws for
                    // more would set failbit on that eof and reject every well-formed value.
                    if (!item.eof()) item >> std::ws;
                    if (item.fail() || !item.eof())
                    {
                        error = "--trend-params must be FAST,SLOW,ENTRY_TICKS,EXIT_TICKS,STOP_TICKS[,"
                                "COOLDOWN_VIEWS[,HOLDING_VIEWS]]";
                        return false;
                    }
                    values.push_back(value);
                }
                if (values.size() < 5 || values.size() > 7)
                {
                    error = "--trend-params must be FAST,SLOW,ENTRY_TICKS,EXIT_TICKS,STOP_TICKS[,"
                            "COOLDOWN_VIEWS[,HOLDING_VIEWS]]";
                    return false;
                }
                const auto positive_integer = [](double value, double maximum)
                {
                    return value >= 1.0 && value <= maximum &&
                           value == static_cast<double>(static_cast<uint64_t>(value));
                };
                const auto non_negative_integer = [](double value, double maximum)
                {
                    return value >= 0.0 && value <= maximum &&
                           value == static_cast<double>(static_cast<uint64_t>(value));
                };
                if (!positive_integer(values[0], 1000000.0) ||
                    !positive_integer(values[1], 1000000.0) || !(values[2] > 0.0) ||
                    values[3] < 0.0 || values[4] < 0.0 ||
                    (values.size() > 5 && !non_negative_integer(values[5], 1000000.0)) ||
                    (values.size() > 6 && !positive_integer(values[6], 1000000000.0)))
                {
                    error = "--trend-params needs positive view counts, a positive entry band, and "
                            "non-negative exit and stop bands";
                    return false;
                }
                cfg.trend.fast_views = static_cast<uint64_t>(values[0]);
                cfg.trend.slow_views = static_cast<uint64_t>(values[1]);
                cfg.trend.entry_ticks = values[2];
                cfg.trend.exit_ticks = values[3];
                cfg.trend.stop_ticks = values[4];
                if (values.size() > 5)
                    cfg.trend.exit_cooldown_views = static_cast<uint64_t>(values[5]);
                if (values.size() > 6)
                    cfg.trend.max_holding_views = static_cast<uint64_t>(values[6]);
                cfg.trend.set = true;
            }
            else if (flag.compare(0, kBaseSpreadPrefix.size(), kBaseSpreadPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kBaseSpreadPrefix.size()));
                if (!(stream >> ticks) || !(ticks > 0.0))
                {
                    error = "--base-spread-ticks must be a positive number of ticks";
                    return false;
                }
                cfg.base_spread_ticks = ticks;
            }
            else if (flag.compare(0, kInventoryShiftPrefix.size(), kInventoryShiftPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kInventoryShiftPrefix.size()));
                if (!(stream >> ticks) || !(ticks > 0.0))
                {
                    error = "--inventory-shift-ticks must be a positive number of ticks";
                    return false;
                }
                cfg.inventory_shift_ticks = ticks;
            }
            else if (flag.compare(0, kRefreshPrefix.size(), kRefreshPrefix) == 0)
            {
                uint64_t ticks = 0;
                if (!parse_unsigned(flag.substr(kRefreshPrefix.size()), ticks) || ticks == 0)
                {
                    error = "--refresh-price-ticks must be a positive integer";
                    return false;
                }
                cfg.refresh_price_ticks_override = ticks;
            }
            else if (flag.compare(0, kEdgeCoverPrefix.size(), kEdgeCoverPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kEdgeCoverPrefix.size()));
                if (!(stream >> ticks) || ticks < 0.0)
                {
                    error = "--edge-cover-ticks must not be negative";
                    return false;
                }
                cfg.edge_cover_ticks = ticks;
            }
            else if (flag.compare(0, kMinSpreadPrefix.size(), kMinSpreadPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kMinSpreadPrefix.size()));
                if (!(stream >> ticks) || ticks < 0.0)
                {
                    error = "--min-spread-ticks must not be negative";
                    return false;
                }
                cfg.min_spread_ticks = ticks;
            }
            else if (flag.compare(0, kQueueChunksPrefix.size(), kQueueChunksPrefix) == 0)
            {
                uint64_t chunks = 0;
                if (!parse_unsigned(flag.substr(kQueueChunksPrefix.size()), chunks) ||
                    chunks == 0)
                {
                    error = "--queue-chunks must be a positive integer";
                    return false;
                }
                cfg.queue_lump_chunks = chunks;
            }
            else if (flag.compare(0, kQueueSeedPrefix.size(), kQueueSeedPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kQueueSeedPrefix.size()), cfg.queue_seed))
                {
                    error = "--queue-seed must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kArrivalSharePrefix.size(), kArrivalSharePrefix) == 0)
            {
                double share = 0.0;
                std::istringstream stream(flag.substr(kArrivalSharePrefix.size()));
                if (!(stream >> share) || share < 0.0 || share > 1.0)
                {
                    error = "--arrival-share must be a number between 0 and 1";
                    return false;
                }
                cfg.arrival_share = share;
            }
            else if (flag.compare(0, kOrderLatencyPrefix.size(), kOrderLatencyPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kOrderLatencyPrefix.size()), cfg.order_latency_us))
                {
                    error = "--order-latency-us must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kCancelLatencyPrefix.size(), kCancelLatencyPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kCancelLatencyPrefix.size()), cfg.cancel_latency_us))
                {
                    error = "--cancel-latency-us must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kTraceOrderPrefix.size(), kTraceOrderPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kTraceOrderPrefix.size()), cfg.trace_order))
                {
                    error = "--trace-order must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kFundingRatesPrefix.size(), kFundingRatesPrefix) == 0)
            {
                cfg.funding_rates_path = flag.substr(kFundingRatesPrefix.size());
                if (cfg.funding_rates_path.empty())
                {
                    error = "--funding-rates needs a file path";
                    return false;
                }
            }
            else if (flag.compare(0, kMakerFeePrefix.size(), kMakerFeePrefix) == 0)
            {
                std::istringstream stream(flag.substr(kMakerFeePrefix.size()));
                if (!(stream >> cfg.maker_fee_rate))
                {
                    error = "--maker-fee-rate must be a number, and may be negative for a rebate";
                    return false;
                }
                cfg.has_maker_fee_rate = true;
            }
            else if (flag.compare(0, kFeeSchedulePrefix.size(), kFeeSchedulePrefix) == 0)
            {
                // Comma separated key=value pairs, because the terms arrive together: one flag that names all
                // five keeps a sensitivity run to one command line, and a run that names a subset is priced
                // under that subset with the rest at zero rather than under a half remembered default.
                std::stringstream items(flag.substr(kFeeSchedulePrefix.size()));
                std::string item;
                while (std::getline(items, item, ','))
                {
                    const std::size_t equals = item.find('=');
                    if (equals == std::string::npos)
                    {
                        error = "--fee-schedule wants key=value pairs separated by commas";
                        return false;
                    }
                    const std::string key = item.substr(0, equals);
                    double value = 0.0;
                    std::istringstream value_stream(item.substr(equals + 1));
                    if (!(value_stream >> value))
                    {
                        error = "--fee-schedule value for " + key + " must be a number";
                        return false;
                    }
                    if (key == "min")
                        cfg.fee_schedule.min_commission_per_order = value;
                    else if (key == "regulatory")
                        cfg.fee_schedule.regulatory_fee_rate = value;
                    else if (key == "clearing")
                        cfg.fee_schedule.clearing_fee_per_share = value;
                    else if (key == "audit")
                        cfg.fee_schedule.audit_fee_per_share = value;
                    else if (key == "rebate")
                        cfg.fee_schedule.maker_rebate_per_share = value;
                    else if (key == "commission")
                    {
                        cfg.fee_schedule.commission_per_share = value;
                        cfg.fee_schedule.has_commission_per_share = true;
                    }
                    else
                    {
                        error = "--fee-schedule keys are min, regulatory, clearing, audit, rebate and commission";
                        return false;
                    }
                }
                cfg.has_fee_schedule = true;
            }
            else
            {
                error = "optional replay flags must be --no-output, --fast-validation, "
                        "--profile-stages[=interval], --live-pacing[=speed], --order-size=N, --inventory-limit=N, "
                        "--max-position=N, --base-spread-ticks=N, --refresh-price-ticks=N, "
                        "--queue-chunks=N, --queue-seed=N, --arrival-share=X, --order-latency-us=N, "
                        "--cancel-latency-us=N, --trace-order=N, --funding-rates=FILE, --maker-fee-rate=R, "
                        "--fee-schedule=KEY=VALUE[,KEY=VALUE], "
                        "--markout-cover, --metrics, --markout-horizon=N, --parent=SIDE:QUANTITY:SECONDS, "
                        "--algo=MARKET|TWAP:N|POV:X, --algo-entry=CROSS|POST:SECONDS, --flatten-band=N, "
                        "--symbol-b=SYM with --trades-b=FILE and --depth-b=FILE, "
                        "--snapshot-out=FILE or --snapshot-in=FILE, --post-only, --close-out, or "
                        "--paper-broker with one of its faults";
                return false;
            }
        }
        // The second book. Three flags that only mean anything together: a symbol with no files to replay, or
        // files with no symbol, is half a book, and reporting it as a run that traded one instrument would hide
        // the mistake rather than make it.
        const bool second_book_named =
            !cfg.symbol_b.empty() || !cfg.trades_b_path.empty() || !cfg.depth_b_path.empty();
        if (second_book_named)
        {
            if (cfg.mode != AppMode::L2Replay)
            {
                error = "a second book is an l2_replay run: a live session reads one socket, and the "
                        "single-instrument replay modes take one symbol";
                return false;
            }
            if (cfg.symbol_b.empty() || cfg.trades_b_path.empty() || cfg.depth_b_path.empty())
            {
                error = "a second book needs --symbol-b, --trades-b and --depth-b: a symbol without its files, or "
                        "files without a symbol, is not a book";
                return false;
            }
            if (cfg.symbol_b == cfg.symbol)
            {
                error = "a second book needs a different --symbol-b: the same symbol twice would be one position "
                        "with two owners rather than two legs";
                return false;
            }
            for (const std::string& path : {cfg.trades_b_path, cfg.depth_b_path})
            {
                if (!std::filesystem::is_regular_file(to_abs_path(path)))
                {
                    error = "second book input file does not exist: " + to_abs_path(path);
                    return false;
                }
            }
            // The four things a second book cannot carry, each refused for its own reason rather than ignored:
            // a knob that configures a layer the run does not build makes a two-book run and a one-book run
            // with the same numbers look alike.
            if (cfg.paper_broker || cfg.order_gateway)
            {
                error = "the paper broker and the order gateway wrap one venue; a two-symbol run builds one venue "
                        "per symbol, and the wrapper is not per leg yet";
                return false;
            }
            if (cfg.metrics)
            {
                error = "--metrics samples one account's equity series; with two books the sampler would run "
                        "twice per event, and [PERFORMANCE] would describe a book that does not exist";
                return false;
            }
            if (cfg.has_parent)
            {
                error = "--parent works one order on one book: two books have two arrival prices and the "
                        "[EXECUTION_ALGO] line has room for one of them";
                return false;
            }
            if (!cfg.record_prefix.empty())
            {
                error = "--record writes one stream in the replay shapes, and replaying a two-symbol recording "
                        "would read both instruments into the single book a replay builds";
                return false;
            }
            if (!cfg.funding_rates_path.empty())
            {
                error = "--funding-rates names one history for one instrument; a two-symbol run needs one "
                        "history per leg, which is a shape the flag does not have yet";
                return false;
            }
        }
        if (cfg.fast_validation && !is_l2_family)
        {
            error = "--fast-validation is only supported by l2_replay or ws";
            return false;
        }
        // A session snapshot belongs to one account and one record per book: a two-symbol run writes and resumes two
        // of them, each with its own clock, id sequence and working orders. A live session has no replay clock to
        // resume against, which is why the mode is refused rather than the second book.
        if (!cfg.snapshot_out_path.empty() || !cfg.snapshot_in_path.empty())
        {
            if (cfg.mode != AppMode::L2Replay)
            {
                error = "--snapshot-out and --snapshot-in are l2_replay flags: a session is written and resumed "
                        "against a replay's own clock";
                return false;
            }
            if (!cfg.snapshot_in_path.empty() && cfg.order_gateway)
            {
                error = "--snapshot-in cannot be combined with --order-gateway: the gateway's own table is client "
                        "state that a resumed process does not have, so its cancel of an inherited order would "
                        "never reach the venue";
                return false;
            }
            if (!cfg.snapshot_in_path.empty() &&
                !std::filesystem::is_regular_file(to_abs_path(cfg.snapshot_in_path)))
            {
                error = "session snapshot does not exist: " + to_abs_path(cfg.snapshot_in_path);
                return false;
            }
        }
        // The order leg is one file per venue, so it belongs to a run that has exactly one: a two-book run has two
        // engines and would need two recordings, which is a shape the flag does not have yet. Every other mode has
        // no venue to record at all.
        // The live order leg. The endpoint, the cap and the opt-in are one decision, so a run that names one of them
        // without the others has asked for something that cannot be what it meant: an endpoint that reaches no venue,
        // or a live leg with no cap of its own, which is the case ADR 0005 decision 4 exists to forbid.
        if (cfg.live_orders)
        {
            if (!is_l2_family)
            {
                error = "--live-orders is l2_replay and ws only: it replaces the run's venue with a socket";
                return false;
            }
            if (cfg.venue_endpoint.empty())
            {
                error = "--live-orders needs --venue-endpoint=HOST:PORT: the relay's order channel is named, not guessed";
                return false;
            }
            if (cfg.venue_max_orders == 0)
            {
                error = "--live-orders needs --venue-max-orders=N, the adapter's own cap: the pipeline's limits "
                        "protect against a strategy, and this one protects against the adapter and the strategy "
                        "disagreeing about what was sent";
                return false;
            }
            if (!cfg.trades_b_path.empty() || !cfg.depth_b_path.empty())
            {
                error = "--live-orders names one venue, and a two-book run has two engines";
                return false;
            }
        }
        else if (!cfg.venue_endpoint.empty() || cfg.venue_max_orders != 0 || cfg.venue_max_notional_usd != 0.0)
        {
            error = "--venue-endpoint, --venue-max-orders and --venue-max-notional are part of --live-orders: without "
                    "it the venue is the in-process matcher, and an endpoint that reaches nothing is worse than no flag";
            return false;
        }
        if (!cfg.order_log_path.empty())
        {
            if (!is_l2_family)
            {
                error = "--record-orders is l2_replay and ws only: it writes the order leg of a session";
                return false;
            }
            if (!cfg.trades_b_path.empty() || !cfg.depth_b_path.empty())
            {
                error = "--record-orders names one prefix for one venue, and a two-book run has two engines";
                return false;
            }
        }
        // A recorded order leg replayed as the venue. It is the other half of the same opt-in: the file is what
        // `--record-orders` wrote, and the run is the session that wrote it. Two refusals follow from that -- a live
        // venue would be a second venue, and recording the replay would write a second file of the same session --
        // and a missing file is an error naming the path rather than a run that quotes nothing at all.
        if (!cfg.order_replay_path.empty())
        {
            if (!is_l2_family)
            {
                error = "--replay-orders is l2_replay and ws only: it replaces the run's venue with a recorded one";
                return false;
            }
            if (cfg.live_orders)
            {
                error = "--replay-orders and --live-orders are two venues: a replay answers from a file and a live "
                        "leg from a socket";
                return false;
            }
            if (!cfg.order_log_path.empty())
            {
                error = "--replay-orders and --record-orders would write the session being replayed, which is a "
                        "second file of the same answers";
                return false;
            }
            if (!cfg.trades_b_path.empty() || !cfg.depth_b_path.empty())
            {
                error = "--replay-orders names one recording for one venue, and a two-book run has two engines";
                return false;
            }
            const std::string recording = to_abs_path(cfg.order_replay_path) + "_orders.csv";
            if (!std::filesystem::is_regular_file(recording))
            {
                error = "order recording does not exist: " + recording;
                return false;
            }
        }
        if (cfg.profile_sample_interval > 0 && !is_l2_family)
        {
            error = "--profile-stages is only supported by l2_replay or ws";
            return false;
        }
        if (cfg.live_pacing && cfg.mode != AppMode::L2Replay)
        {
            // A live socket is paced by the venue, not by a file's clock, so pacing has no meaning
            // there and is refused rather than silently ignored.
            error = "--live-pacing is only supported by l2_replay";
            return false;
        }
        if (cfg.markout_cover && !is_l2_family)
        {
            error = "--markout-cover is only supported by l2_replay or ws";
            return false;
        }
        if (cfg.has_parent && !is_l2_family && cfg.mode != AppMode::Replay)
        {
            error = "--parent is supported by replay, l2_replay and ws: each has a touch to cross and an event "
                    "clock to measure against";
            return false;
        }
        if (cfg.has_parent && cfg.strategy_name != "execution_l2")
        {
            error = "--parent needs the execution_l2 strategy: the schedule is what works the order";
            return false;
        }
        if (!cfg.has_parent && cfg.strategy_name == "execution_l2")
        {
            error = "execution_l2 needs --parent=SIDE:QUANTITY:SECONDS: there is nothing to execute";
            return false;
        }
        if (cfg.has_parent && cfg.mode == AppMode::Replay && cfg.algo != "market")
        {
            // The L1 touch callback carries no timestamp, so a schedule that spaces slices by market time has
            // no clock to space them on and would silently emit one child. Refused rather than half-run.
            error = "a time-sliced --algo needs the L2 path: the replay mode's touch callback carries no clock, "
                    "so only --algo=market can run there";
            return false;
        }
        if (!cfg.has_parent && cfg.algo_entry != "cross")
        {
            error = "--algo-entry configures how a parent is worked and needs --parent";
            return false;
        }
        if (cfg.flatten_band > 0 && !is_l2_family)
        {
            error = "--flatten-band is only supported by l2_replay or ws, whose decision layer carries "
                    "the band";
            return false;
        }
        if (!cfg.paper_broker &&
            (cfg.broker_reject_every > 0 || cfg.broker_partial_every > 0 || cfg.broker_drop_every > 0 ||
             cfg.broker_outage_after > 0 || cfg.broker_outage_holds > 0))
        {
            // Silently ignoring a fault would make a faulted run and a clean run indistinguishable in
            // the summary, which is the one thing a fault injection must never do.
            error = "a --broker-* fault needs --paper-broker, the broker it is a fault of";
            return false;
        }
        if (cfg.broker_outage_holds > 0 && cfg.broker_outage_after == 0)
        {
            error = "--broker-outage-holds needs --broker-outage-after: nothing says when the venue goes";
            return false;
        }
        if (!cfg.order_gateway &&
            (cfg.gateway_ack_timeout_us > 0 || cfg.gateway_retry_on_timeout))
        {
            // The same reason as the broker faults: a knob that configures a layer the run does not
            // build would leave a timed-out run and a clean one indistinguishable in the summary.
            error = "a --gateway-* flag needs --order-gateway, the gateway it configures";
            return false;
        }
        if (cfg.gateway_retry_on_timeout && cfg.gateway_ack_timeout_us == 0)
        {
            error = "--gateway-retry-timeouts needs --gateway-ack-timeout-us: nothing says when to retry";
            return false;
        }
        if (cfg.markout_cover && cfg.strategy_name != "active_l2" &&
            cfg.strategy_name != "adaptive_l2")
        {
            error = "--markout-cover needs a strategy whose decision layer carries the gate: active_l2 or "
                    "its adaptive_l2 alias";
            return false;
        }
    }
    else if (cfg.mode == AppMode::Carry)
    {
        // Two funding histories and the position they are measured with. There is no book file and no
        // strategy, so every other input is a flag and this branch owns all of them.
        if (argc < 4)
        {
            error = "carry requires two funding rate files";
            return false;
        }
        cfg.path_or_port = argv[2];
        cfg.funding_b_path = argv[3];
        for (int argument = 4; argument < argc; ++argument)
        {
            const std::string flag = argv[argument];
            if (flag.compare(0, kSymbolAPrefix.size(), kSymbolAPrefix) == 0)
                cfg.symbol = flag.substr(kSymbolAPrefix.size());
            else if (flag.compare(0, kSymbolBPrefix.size(), kSymbolBPrefix) == 0)
                cfg.symbol_b = flag.substr(kSymbolBPrefix.size());
            else if (flag.compare(0, kNotionalPrefix.size(), kNotionalPrefix) == 0)
            {
                std::istringstream stream(flag.substr(kNotionalPrefix.size()));
                if (!(stream >> cfg.carry_notional_usd) || !(cfg.carry_notional_usd > 0.0))
                {
                    error = "--notional-usd must be a positive number";
                    return false;
                }
            }
            else if (flag.compare(0, kLongPrefix.size(), kLongPrefix) == 0)
            {
                const std::string leg = flag.substr(kLongPrefix.size());
                if (leg == "a")
                    cfg.carry_leg_a_long = true;
                else if (leg == "b")
                    cfg.carry_leg_a_long = false;
                else
                {
                    error = "--long must be a or b";
                    return false;
                }
            }
            else
            {
                error = "carry flags must be --symbol-a=SYM, --symbol-b=SYM, --notional-usd=N, or "
                        "--long=a|b";
                return false;
            }
        }
        if (cfg.symbol.empty() || cfg.symbol_b.empty())
        {
            error = "carry needs --symbol-a and --symbol-b, because the venue's fee schedule is looked up "
                    "by symbol and not passed on the command line";
            return false;
        }
        if (cfg.symbol == cfg.symbol_b)
        {
            error = "carry needs two different symbols: the same one on both legs would net into a single "
                    "position rather than a hedge";
            return false;
        }
        for (const std::string& path : {cfg.path_or_port, cfg.funding_b_path})
        {
            if (!std::filesystem::is_regular_file(to_abs_path(path)))
            {
                error = "carry funding file does not exist: " + to_abs_path(path);
                return false;
            }
        }
        return true;
    }
    else if (argc > 5 || (cfg.mode == AppMode::LegacyUdp && argc > 4))
    {
        error = "too many arguments";
        return false;
    }

    const bool is_replay_mode =
        cfg.mode == AppMode::Replay || cfg.mode == AppMode::L2Replay ||
        cfg.mode == AppMode::WebSocket;
    if (!is_replay_mode && argc >= 3) cfg.path_or_port = argv[2];
    if (cfg.mode == AppMode::LegacyCsv && cfg.path_or_port.empty())
    {
        error = "CSV path cannot be empty";
        return false;
    }
    if (cfg.mode == AppMode::LegacyCsv &&
        !std::filesystem::is_regular_file(to_abs_path(cfg.path_or_port)))
    {
        error = "CSV file does not exist: " + to_abs_path(cfg.path_or_port);
        return false;
    }

    if (!is_replay_mode && argc >= 4)
    {
        if (cfg.mode == AppMode::LegacyUdp)
        {
            cfg.strategy_name = argv[3];
        }
        else if (!parse_integer(argv[3], cfg.delay) || cfg.delay < 0)
        {
            error = "delay must be a non-negative integer";
            return false;
        }
    }
    if (!is_replay_mode && argc >= 5) cfg.strategy_name = argv[4];

    // One list, searched rather than spelled out: the names this accepts and the names the usage text
    // prints are the same array, so they cannot drift apart again.
    const std::string_view requested_strategy = cfg.strategy_name;
    if (std::find(kStrategyNames.begin(), kStrategyNames.end(), requested_strategy) ==
        kStrategyNames.end())
    {
        error = "unsupported strategy name";
        return false;
    }

    if (cfg.mode == AppMode::LegacyUdp)
    {
        int port = 0;
        if (!parse_integer(cfg.path_or_port, port) || port < 1 || port > 65535)
        {
            error = "UDP port must be an integer from 1 to 65535";
            return false;
        }
    }

    return true;
}
