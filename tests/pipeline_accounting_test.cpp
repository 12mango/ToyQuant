#include <cassert>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "exchange/matching_engine.h"
#include "market/funding_rate.h"
#include "orderbook/top_of_book.h"
#include "strategy/l2_market_maker_base.h"
#include "strategy/legacy/baseline_market_makers.h"
#include "support/test_support.h"
#include "utils/logger.h"

namespace
{
// A venue whose cancel answer is the demo's: an order that filled before its cancel is answered with the refusal
// the venue sends for it (`sCode 51400`, a `Rejected` here) and the fill that made it unfillable follows on the same
// channel. The in-process matcher cannot produce that order -- it reports a fill before the report that ends the
// order -- so the strategy's attribution of a late fill is pinned against a venue that answers the way the live one
// did. A reduce-only exit is filled on placement, which is the market order the live close sends.
class LateFillVenue : public IMatchingEngine
{
   public:
    // How many sell cancels are answered in the 51400 shape; the rest cancel like any other venue. A count rather
    // than a side rule, so the sweep the close performs afterwards is an ordinary cancel.
    void fill_next_cancel(uint64_t count) { late_fill_cancels_ = count; }

    // The venue's answer to a reduce-only exit it will not take -- a sell against an account already short is the
    // measured case (2026-11-21) -- as a report rather than as silence.
    void refuse_closes(bool on) { refuse_closes_ = on; }

    // A working quote that fills while the exit is working: the venue books it against the position the exit was sized
    // for, which is the race a cancel cannot close.
    void fill_extra(const std::string& symbol, exchange::Side side, double price, uint64_t quantity)
    {
        emit(ExecutionReport{.order_id = 9000 + extra_fills_++,
                             .side = side,
                             .exec_type = ExecType::Trade,
                             .symbol = symbol,
                             .price = price,
                             .quantity = quantity,
                             .owner = "MarketMaker"});
    }

    void send_order(const exchange::Order& order) override
    {
        sent_[order.id] = order;
        if (order.reduce_only && refuse_closes_)
        {
            emit(ExecutionReport{.order_id = order.id,
                                 .side = order.side,
                                 .exec_type = ExecType::Rejected,
                                 .symbol = order.symbol,
                                 .price = order.price,
                                 .quantity = order.remaining,
                                 .owner = order.owner,
                                 .reject_reason = RejectReason::Venue});
            return;
        }
        if (order.reduce_only)
        {
            emit_trade(order, order.price, order.qty);
            return;
        }
        emit(ExecutionReport{.order_id = order.id,
                             .side = order.side,
                             .exec_type = ExecType::Resting,
                             .symbol = order.symbol,
                             .price = order.price,
                             .quantity = order.remaining,
                             .owner = order.owner});
    }

    void cancel_order(uint64_t order_id) override
    {
        const auto sent = sent_.find(order_id);
        if (sent == sent_.end()) return;
        if (late_fill_cancels_ > 0 && sent->second.side == exchange::Side::Sell)
        {
            --late_fill_cancels_;
            emit(ExecutionReport{.order_id = order_id,
                                 .side = sent->second.side,
                                 .exec_type = ExecType::Rejected,
                                 .symbol = sent->second.symbol,
                                 .price = sent->second.price,
                                 .quantity = sent->second.remaining,
                                 .owner = sent->second.owner,
                                 .reject_reason = RejectReason::Venue});
            emit_trade(sent->second, sent->second.price, sent->second.remaining);
            return;
        }
        emit(ExecutionReport{.order_id = order_id,
                             .side = sent->second.side,
                             .exec_type = ExecType::Cancelled,
                             .symbol = sent->second.symbol,
                             .price = sent->second.price,
                             .quantity = sent->second.remaining,
                             .owner = sent->second.owner,
                             .cancel_reason = CancelReason::User});
    }

    void process_bbo(const BboQuote& quote) override { (void)quote; }
    void process_market_trade(const MarketTrade& trade) override { (void)trade; }
    void set_report_callback(ReportCallback cb) override { callback_ = std::move(cb); }

   private:
    void emit(const ExecutionReport& report)
    {
        if (callback_) callback_(report);
    }

    void emit_trade(const exchange::Order& order, double price, uint64_t quantity)
    {
        emit(ExecutionReport{.order_id = order.id,
                             .side = order.side,
                             .exec_type = ExecType::Trade,
                             .symbol = order.symbol,
                             .price = price,
                             .quantity = quantity,
                             .owner = order.owner});
    }

    std::unordered_map<uint64_t, exchange::Order> sent_;
    ReportCallback callback_;
    uint64_t late_fill_cancels_{0};
    bool refuse_closes_{false};
    uint64_t extra_fills_{0};
};
}  // namespace

int main()
{
    test_support::TempFile orders_file("pipeline_accounting_orders.csv");
    test_support::TempFile trades_file("pipeline_accounting_trades.csv");
    std::ofstream orders = orders_file.open();
    std::ofstream trades = trades_file.open();
    NaiveMarketMaker strategy(10, 0.1, 1.0);
    MatchingEngine engine(1.0,
                          FeeSchedule{.maker_rate = 0.001,
                                      .taker_rate = 0.002,
                                      .quantity_scale = 1000});
    Portfolio portfolio(1000, 1000.0);
    Logger logger;
    Pipeline pipeline(orders, trades, strategy, engine, portfolio);

    pipeline.process_event(BboQuote{1, "TEST", 99.0, 100, 101.0, 100, 1, "binance"});
    pipeline.process_event(MarketTrade{2, "TEST", 100.0, 10, Side::Sell, 2, "binance"});

    const auto* position = portfolio.find_position("TEST");
    const auto metrics = portfolio.metrics();
    assert(position != nullptr);
    assert(position->quantity == 10);
    assert(metrics.maker_trade_count == 1);
    assert(metrics.taker_trade_count == 0);
    assert(std::abs(metrics.fees_paid - 0.001) < 1e-12);
    assert(pipeline.summary().trade_reports == 1);

    Pipeline filtered_pipeline(orders, trades, strategy, engine, portfolio);
    filtered_pipeline.process_event(MarketTrade{3, "TEST", 110.0, 10, Side::Sell, 3, "binance"});
    assert(filtered_pipeline.summary().trade_reports == 0);
    assert(filtered_pipeline.summary().filtered_market_trades == 1);

    Pipeline isolated_pipeline(orders, trades, strategy, engine, portfolio);
    isolated_pipeline.process_event(BboQuote{4, "TEST", 99.0, 100, 101.0, 100, 4, "binance"});
    bool rejected_mixed_exchange = false;
    try
    {
        isolated_pipeline.process_event(MarketDepthSnapshot{
            .ts = 5,
            .symbol = "BTC-PERPETUAL",
            .sequence = 1,
            .bids = {{100.0, 1}},
            .asks = {{101.0, 1}},
            .exchange = "deribit",
        });
    }
    catch (const std::invalid_argument&)
    {
        rejected_mixed_exchange = true;
    }
    assert(rejected_mixed_exchange);

    test_support::TempFile l2_orders_file("pipeline_l2_queue_orders.csv");
    test_support::TempFile l2_trades_file("pipeline_l2_queue_trades.csv");
    std::ofstream l2_orders = l2_orders_file.open();
    std::ofstream l2_trades = l2_trades_file.open();
    L2MarketMakerBase l2_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                   .base_spread = 0.5,
                                                   .inventory_limit = 100,
                                                   .tick_size = 1.0,
                                                   .signal_mode = L2SignalMode::Baseline});
    MatchingEngine l2_engine(1.0);
    Portfolio l2_portfolio;
    Pipeline l2_pipeline(l2_orders, l2_trades, l2_strategy, l2_engine, l2_portfolio);
    l2_pipeline.set_trace_order(1);
    l2_pipeline.process_l2_market_view(L2MarketView{.symbol = "L2",
                                                    .ts = 10,
                                                    .top = TopOfBook{100.0, 100, 101.0, 100},
                                                    .micro_price = 100.5});
    assert(l2_pipeline.summary().submitted_orders == 2);
    l2_pipeline.process_l2_market_trade(
        MarketTrade{11, "L2", 100.0, 50, Side::Sell, 1, "deribit"});
    assert(l2_pipeline.summary().trade_reports == 0);
    l2_pipeline.process_l2_market_trade(
        MarketTrade{12, "L2", 100.0, 60, Side::Sell, 2, "deribit"});
    assert(l2_pipeline.summary().trade_reports == 1);
    assert(l2_pipeline.summary().trade_report_quantity == 10);
    assert(l2_pipeline.summary().queue_ahead_consumed == 100);

    // The trace of order 1, which is the bid the first view submitted. It joins behind the 100 the level
    // displays, the first market trade takes half of that queue, and the second reaches it and fills 10.
    // The accessor behind these rows reports the order's wait, not a property of the price, so the submit
    // row has no queue and the rest row is where the joined queue appears.
    const auto trace = l2_pipeline.summary().queue_trace;
    assert(trace.size() == 5);
    assert(trace[0].event == "submit" && trace[0].side == Side::Buy && trace[0].price == 100.0);
    // The submit row has no queue because the order is not in a level yet, which is the accessor's real
    // semantics rather than a gap in it.
    assert(trace[0].change == 10 && trace[0].queue_ahead == 0);
    assert(trace[1].event == "rest" && trace[1].queue_ahead == 100);
    assert(trace[2].event == "trade" && trace[2].queue_ahead == 50 && trace[2].change == 50);
    // The queue reached zero and the order traded. The second market trade leaves the queue where it is, so
    // it produces no further row: a row means the queue moved.
    assert(trace[3].event == "fill" && trace[3].change == 10 && trace[3].queue_ahead == 0);
    assert(trace[4].event == "fully_filled" && trace[4].queue_ahead == 0);
    assert(l2_pipeline.summary().first_fill_order_id == 1);
    assert(l2_pipeline.summary().trace_requested);

    // The distribution over orders, which is the statistic the trace illustrates. Order 1 (the bid) filled,
    // so its closest approach and its queue at the end are both zero. Order 2 is still resting, so it has no
    // profile yet: the profile is closed when an order dies, not when it is submitted.
    assert(l2_pipeline.summary().queue_profile_orders == 1);
    assert(l2_pipeline.summary().queue_zero_orders == 1);
    assert(l2_pipeline.summary().queue_min_fraction_p50 == 0.0);
    assert(l2_pipeline.summary().queue_end_fraction_p50 == 0.0);

    // A view that moves the mid far enough to requote cancels the ask, whose queue was never touched. This is
    // the case that proves the end-of-life reading is taken before the cancel: the engine erases a level once
    // no order rests in it, so a reading taken after the Cancelled report would report zero here as well and
    // the two percentiles would be indistinguishable.
    l2_pipeline.process_l2_market_view(L2MarketView{.symbol = "L2",
                                                    .ts = 13,
                                                    .top = TopOfBook{101.5, 100, 102.5, 100},
                                                    .micro_price = 102.0});
    assert(l2_pipeline.summary().queue_profile_orders == 2);
    assert(l2_pipeline.summary().queue_zero_orders == 1);
    assert(l2_pipeline.summary().queue_min_fraction_p50 == 1.0);
    assert(l2_pipeline.summary().queue_end_fraction_p50 == 1.0);

    // A normal run never has an order refused: the guard, the lot size and the minimum are all checked on
    // every submit, and a refusal would show up here rather than as a missing quote.
    assert(l2_engine.rejected_orders() == 0);

    MatchingEngine deribit_fee_engine(0.5,
        FeeSchedule{.maker_rate = 0.0002,
                    .taker_rate = 0.0005,
                    .quantity_scale = 1,
                    .unit_notional_usd = 10.0});
    test_support::TempFile fee_orders_file("pipeline_deribit_fee_orders.csv");
    test_support::TempFile fee_trades_file("pipeline_deribit_fee_trades.csv");
    std::ofstream fee_orders = fee_orders_file.open();
    std::ofstream fee_trades = fee_trades_file.open();
    NaiveMarketMaker fee_strategy(1, 0.0, 0.5);
    Portfolio fee_portfolio(1, 1000.0, 10.0);
    Pipeline fee_pipeline(fee_orders, fee_trades, fee_strategy, deribit_fee_engine, fee_portfolio);
    fee_pipeline.process_event(BboQuote{20, "BTC-PERPETUAL", 99.0, 100, 101.0, 100, 1,
                                        "deribit"});
    fee_pipeline.process_event(
        MarketTrade{21, "BTC-PERPETUAL", 100.0, 1, Side::Sell, 2, "deribit"});
    // One Deribit BTC-PERPETUAL contract is worth 10 USD, so the maker fee is 0.02% of 10 USD and
    // not 0.02% of the quoted price.
    assert(std::abs(fee_portfolio.metrics().fees_paid - 0.002) < 1e-12);
    // A pipeline that was never asked to trace an order reports no hints, which is what keeps the summary
    // of an ordinary run exactly as it was.
    assert(!fee_pipeline.summary().trace_requested);

    // Funding as an input to the ledger. The published rate is what longs pay and the accounting takes the
    // rate a long earns, so a settlement is a cost to a long position and the pipeline is what flips the
    // sign. The timing rule is the other half: a settlement is applied at the first event at or after it, on
    // the position the previous event left, at the last price the run has seen.
    {
        FundingRateSeries funding("BTC-PERPETUAL");
        funding.add(0, 0.5);      // reached while the account is flat and no price exists yet
        funding.add(30, 0.0001);  // 0.01%, which the long pays
        test_support::TempFile funding_orders_file("pipeline_funding_orders.csv");
        test_support::TempFile funding_trades_file("pipeline_funding_trades.csv");
        std::ofstream funding_orders = funding_orders_file.open();
        std::ofstream funding_trades = funding_trades_file.open();
        NaiveMarketMaker funding_strategy(1, 0.0, 0.5);
        MatchingEngine funding_engine(0.5, FeeSchedule{.maker_rate = 0.0002,
                                                       .taker_rate = 0.0005,
                                                       .quantity_scale = 1,
                                                       .unit_notional_usd = 10.0});
        Portfolio funding_portfolio(1, 1000.0, 10.0);
        Pipeline funding_pipeline(funding_orders, funding_trades, funding_strategy, funding_engine,
                                  funding_portfolio);
        funding_pipeline.set_funding_rates(&funding);

        funding_pipeline.process_event(BboQuote{20, "BTC-PERPETUAL", 99.0, 100, 101.0, 100, 1,
                                                "deribit"});
        funding_pipeline.process_event(
            MarketTrade{21, "BTC-PERPETUAL", 100.0, 1, Side::Sell, 2, "deribit"});
        // The settlement at ts=0 was reached by the first event, before any price and with a flat account:
        // applied, and worth nothing. The one at ts=30 is still ahead of the clock.
        assert(funding_pipeline.summary().funding.settlements == 1);
        assert(std::abs(funding_pipeline.summary().funding.flow_usd) < 1e-15);

        const double cash_before = funding_portfolio.metrics().cash;
        funding_pipeline.process_event(BboQuote{30, "BTC-PERPETUAL", 99.0, 100, 101.0, 100, 3,
                                                "deribit"});
        // One contract is 10 USD of face value, the mark is the last mid the run saw (100), and the long
        // pays 0.01% of the notional: 0.001 USD out of cash.
        assert(funding_pipeline.summary().funding.settlements == 2);
        assert(std::abs(funding_pipeline.summary().funding.flow_usd + 0.001) < 1e-12);
        assert(std::abs(funding_portfolio.metrics().cash - (cash_before - 0.001)) < 1e-12);
        // The line reports the file as well as the settlements, and exists only because a series was given.
        assert(funding_pipeline.summary().funding_requested);
        assert(funding_pipeline.summary().funding.points == 2);
        assert(std::abs(funding_pipeline.summary().funding.sum_rate - 0.5001) < 1e-15);
        assert(!fee_pipeline.summary().funding_requested);
    }

    // The adverse-selection measurement: the pipeline resolves what a passive fill at each distance from the
    // midpoint would have been worth, from the venue's own prints, and pushes it to the strategy. The strategy
    // cannot measure it, because it never sees the midpoint after a fill, and the [GATE] line is printed only
    // when a run asks for it.
    {
        test_support::TempFile print_orders_file("pipeline_print_orders.csv");
        test_support::TempFile print_trades_file("pipeline_print_trades.csv");
        std::ofstream print_orders = print_orders_file.open();
        std::ofstream print_trades = print_trades_file.open();
        L2MarketMakerBase print_strategy(L2MarketMakerBaseConfig{.order_size = 1,
                                                         .base_spread = 1.0,
                                                         .inventory_limit = 10,
                                                         .tick_size = 1.0});
        MatchingEngine print_engine(1.0);
        Portfolio print_portfolio;
        Pipeline print_pipeline(print_orders, print_trades, print_strategy, print_engine,
                                print_portfolio);
        print_pipeline.set_markout_cover(true);
        print_pipeline.set_gate_diagnostics(true);

        const auto view_at = [](uint64_t ts, double mid) {
            return L2MarketView{.symbol = "L2",
                                .ts = ts,
                                .top = TopOfBook{mid - 0.5, 10, mid + 0.5, 10},
                                .micro_price = mid};
        };
        print_pipeline.process_l2_market_view(view_at(10, 100.0));
        assert(print_pipeline.summary().gate.market_samples == 0);
        // A passive buy at the midpoint, then five cycles with the midpoint a tick higher: the print was worth a
        // tick to whoever was resting there, which is the sign the estimate is built from.
        print_pipeline.process_l2_market_trade(
            MarketTrade{11, "L2", 100.0, 10, Side::Sell, 1, "deribit"});
        for (uint64_t ts = 12; ts <= 17; ++ts) print_pipeline.process_l2_market_view(view_at(ts, 101.0));
        const auto printed = print_pipeline.summary();
        assert(printed.gate_diagnostics_requested);
        assert(printed.gate.market_samples == 1);
        assert(printed.gate.market_buy_near_ticks > 0.0);
    }

    // The round-trip distribution: a win is a round trip whose net PnL is above zero, the profit factor is
    // the winning sum over the absolute losing one, and the holding time runs from the position's opening
    // fill. The third trade below is gross-positive and net-negative, which is the case the fee-inclusive
    // definition exists for.
    {
        const std::vector<ClosedTrade> trades = {
            {.symbol = "X",
             .side = exchange::Side::Buy,
             .entry_price = 100.0,
             .exit_price = 101.0,
             .quantity = 1.0,
             .pnl = 1.0,
             .fee = 0.2,
             .entry_ts = 0,
             .exit_ts = 2'000'000},
            {.symbol = "X",
             .side = exchange::Side::Sell,
             .entry_price = 101.0,
             .exit_price = 102.0,
             .quantity = 1.0,
             .pnl = -1.0,
             .fee = 0.2,
             .entry_ts = 3'000'000,
             .exit_ts = 4'000'000},
            {.symbol = "X",
             .side = exchange::Side::Buy,
             .entry_price = 100.0,
             .exit_price = 100.1,
             .quantity = 1.0,
             .pnl = 0.1,
             .fee = 0.2,
             .entry_ts = 5'000'000,
             .exit_ts = 5'500'000},
        };
        const auto round_trips = RunAccounting::round_trip_summary(trades, 20.0, 30.0, 10.0);
        assert(round_trips.trades == 3);
        assert(std::abs(round_trips.win_rate - 1.0 / 3.0) < 1e-12);
        // The one win is 0.8 net and the two losses are -1.2 and -0.1, so the factor is 0.8 / 1.3.
        assert(std::abs(round_trips.profit_factor - 0.8 / 1.3) < 1e-12);
        assert(std::abs(round_trips.expectancy_usd + 0.5 / 3.0) < 1e-12);
        assert(std::abs(round_trips.average_win_usd - 0.8) < 1e-12);
        assert(std::abs(round_trips.average_loss_usd + 0.65) < 1e-12);
        // Two seconds, one second and half a second over three round trips.
        assert(std::abs(round_trips.average_holding_seconds - 3.5 / 3.0) < 1e-12);
        assert(std::abs(round_trips.taker_notional_usd - 20.0) < 1e-12);
        assert(std::abs(round_trips.total_notional_usd - 30.0) < 1e-12);
        assert(std::abs(round_trips.turnover - 3.0) < 1e-12);
        // The fees each round trip paid, and the pair that decides whether a run can pay at all: 0.6 over three
        // trips is 0.2 a trip against an expectancy of -0.1667. The basis-point figure divides by the notional the
        // run traded, which this fixture sets at 30 arbitrarily, so it is the arithmetic that is asserted here
        // rather than the fixture's realism.
        assert(std::abs(round_trips.fees_usd - 0.6) < 1e-12);
        assert(std::abs(round_trips.fees_per_round_trip_usd - 0.2) < 1e-12);
        assert(std::abs(round_trips.fees_bps_of_turnover - 0.6 / 30.0 * 1e4) < 1e-9);
        assert(round_trips.fees_per_round_trip_usd > std::abs(round_trips.expectancy_usd));

        // The same distribution split by the side the position was opened on: two longs (one win, one loss at
        // -0.1 net) and one short (a loss). The split has to add up to the aggregate, and a side with no
        // round trips has to say zero rather than carry the other side's numbers.
        const auto longs = RunAccounting::side_round_trip_summary(trades, exchange::Side::Buy);
        const auto shorts = RunAccounting::side_round_trip_summary(trades, exchange::Side::Sell);
        assert(longs.trades == 2);
        assert(shorts.trades == 1);
        assert(longs.trades + shorts.trades == round_trips.trades);
        assert(std::abs(longs.win_rate - 0.5) < 1e-12);
        assert(std::abs(longs.profit_factor - 8.0) < 1e-12);
        assert(std::abs(longs.expectancy_usd - 0.35) < 1e-12);
        assert(std::abs(longs.average_loss_usd + 0.1) < 1e-12);
        assert(std::abs(longs.average_holding_seconds - 1.25) < 1e-12);
        assert(shorts.win_rate == 0.0);
        assert(shorts.profit_factor == 0.0);
        assert(std::abs(shorts.expectancy_usd + 1.2) < 1e-12);
        assert(RunAccounting::side_round_trip_summary({}, exchange::Side::Buy).trades == 0);

        // A run that never closed a position reports no distribution and still reports its turnover; a run
        // with no collateral has no turnover to state.
        const auto empty = RunAccounting::round_trip_summary({}, 0.0, 0.0, 0.0);
        assert(empty.trades == 0);
        assert(empty.win_rate == 0.0);
        assert(empty.turnover == 0.0);
    }

    // The taker measurements: the arrival slippage against the decision price, the drift at the three fixed
    // horizons, and the shortfall of an aggressive order whose remainder never filled.
    {
        RunAccounting taker;
        taker.note_taker_fill({.side = Side::Buy,
                               .price = 101.0,
                               .quantity = 10,
                               .slippage_ticks = 1.0,
                               .slippage_bps = 99.0,
                               .slippage_usd = 0.01,
                               .quote_cycle = 100});
        // Only the five-cycle horizon has closed at 105; the other two are still queued.
        taker.drain_taker_measurements(105, 103.0, 1.0, 0.0, 1.0, 1);
        assert(std::abs(taker.taker_summary().edge_5_ticks - 2.0) < 1e-12);
        assert(taker.taker_summary().edge_20_ticks == 0.0);
        taker.drain_taker_measurements(120, 104.0, 1.0, 0.0, 1.0, 1);
        assert(std::abs(taker.taker_summary().edge_20_ticks - 3.0) < 1e-12);
        // By the sixtieth cycle the mid is back below the fill price, which is a negative edge: the drift
        // did not cover what the crossing paid.
        taker.drain_taker_measurements(160, 100.0, 1.0, 0.0, 1.0, 1);
        const auto summary = taker.taker_summary();
        assert(std::abs(summary.edge_60_ticks + 1.0) < 1e-12);
        assert(summary.fills == 1);
        assert(std::abs(summary.arrival_slippage_ticks - 1.0) < 1e-12);
        assert(std::abs(summary.arrival_slippage_bps - 99.0) < 1e-12);
        assert(std::abs(summary.arrival_slippage_usd - 0.01) < 1e-12);
    }

    // An aggressive buy of ten units that filled six a tick above its decision midpoint and never got the
    // other four: the filled part costs 6 USD, the fee 0.6, and the missed four would have earned 8 USD by
    // the horizon, so the shortfall is 14.6 USD against a thousand dollar decision notional.
    {
        RunAccounting shortfall;
        shortfall.note_aggressive_order({.side = Side::Buy,
                                         .decision_mid = 100.0,
                                         .ordered_quantity = 10,
                                         .filled_quantity = 6,
                                         .filled_usd = 6.0,
                                         .fees_usd = 0.6,
                                         .decision_usd = 1000.0,
                                         .start_cycle = 100});
        shortfall.drain_taker_measurements(105, 102.0, 1.0, 0.0, 1.0, 1);
        const auto summary = shortfall.taker_summary();
        assert(summary.aggressive_orders == 1);
        assert(std::abs(summary.shortfall_filled_usd - 6.0) < 1e-12);
        assert(std::abs(summary.shortfall_fees_usd - 0.6) < 1e-12);
        assert(std::abs(summary.shortfall_opportunity_usd - 8.0) < 1e-12);
        assert(std::abs(summary.shortfall_usd - 14.6) < 1e-12);
        assert(std::abs(summary.shortfall_bps - 146.0) < 1e-12);
    }

    // The parent order behind [EXECUTION_ALGO]: ten units decided at 100, six filled at 101 with 0.6 of fees,
    // and the four never filled charged at the deadline's 102. Against a thousand dollar decision notional the
    // three terms are 60, 6 and 80 basis points, and they add up to the total rather than being three stories.
    {
        RunAccounting parent;
        parent.set_parent_order({.side = Side::Buy, .quantity = 10, .horizon_us = 60'000'000});
        parent.start_parent(1'000, 100.0, 0.0, 1.0, 1);
        parent.note_parent_child();
        parent.note_parent_child();
        parent.note_parent_fill(Side::Buy, 101.0, 6, 0.6, LiquidityRole::Maker);
        // Two midpoints inside the window make the time-weighted benchmark 101: thirty seconds at 100 and
        // thirty at 102 give the same average, which is the point of the column -- a schedule is judged
        // against the average price the market traded at, not against where it happened to start.
        parent.observe_parent_mid(31'000, 101.0);
        parent.finish_parent(61'000, 102.0);
        const auto parent_summary = parent.parent_summary();
        assert(parent_summary.requested && parent_summary.complete && parent_summary.buy);
        assert(parent_summary.quantity == 10);
        assert(parent_summary.filled_quantity == 6);
        assert(parent_summary.child_orders == 2);
        assert(std::abs(parent_summary.filled_ratio - 0.6) < 1e-12);
        assert(std::abs(parent_summary.is_filled_bps - 60.0) < 1e-12);
        assert(std::abs(parent_summary.is_fees_bps - 6.0) < 1e-12);
        assert(std::abs(parent_summary.is_opportunity_bps - 80.0) < 1e-12);
        assert(std::abs(parent_summary.is_total_bps - 146.0) < 1e-12);
        // The benchmark: 30 s at 100 and 30 s at 102 give 101, so the six units filled at 101 pay nothing
        // against it. The four it never got cost 4 USD, and the fee 0.6, against the 1,010 USD notional.
        assert(std::abs(parent_summary.twap_mid - 101.0) < 1e-12);
        assert(std::abs(parent_summary.filled_vs_twap_bps) < 1e-12);
        assert(std::abs(parent_summary.opportunity_vs_twap_bps - 39.603960396039604) < 1e-9);
        assert(std::abs(parent_summary.is_vs_twap_bps - 45.544554455445545) < 1e-9);
        // A fill after the deadline belongs to nothing: the measurement closed with the horizon's charge.
        parent.note_parent_fill(Side::Buy, 102.0, 4, 0.4, LiquidityRole::Taker);
        assert(parent.parent_summary().filled_quantity == 6);
    }

    // The execution mix behind [EXECUTION_ALGO]: the same parent worked half by resting and half by crossing. The
    // split matters because every venue prices the two roles differently, and "the fee was 3.2 bps" says nothing
    // about whether the parent took the cheap role or paid for the expensive one.
    {
        RunAccounting mixed;
        mixed.set_parent_order({.side = Side::Buy, .quantity = 10, .horizon_us = 60'000'000});
        mixed.start_parent(1'000, 100.0, 0.0, 1.0, 1);
        mixed.note_parent_child();
        mixed.note_parent_fill(Side::Buy, 100.5, 6, 0.12, LiquidityRole::Maker);
        mixed.note_parent_fill(Side::Buy, 100.5, 4, 0.20, LiquidityRole::Taker);
        mixed.finish_parent(61'000, 100.5);
        const auto summary = mixed.parent_summary();
        assert(std::abs(summary.maker_share - 0.6) < 1e-12);
        // The two roles' fees are the total, which is the invariant that keeps the split from becoming a third
        // story: whatever the venue did not report is charged to neither side and shows as the difference.
        assert(std::abs(summary.maker_fees_bps + summary.taker_fees_bps - summary.is_fees_bps) < 1e-12);
        assert(std::abs(summary.maker_fees_bps - 1.2) < 1e-12);
        assert(std::abs(summary.taker_fees_bps - 2.0) < 1e-12);
    }

    // The end of the session, priced. A run that stops with a position on the book has marked it at the midpoint and
    // called the difference PnL; nobody can close at the midpoint, so `close_out` sends the position out as an IOC at
    // the touch and reports what the exit cost -- the half spread it gave up and the taker fee it paid, in basis
    // points, which is the same unit every other cost in this project is quoted in.
    {
        test_support::TempFile end_orders_file("pipeline_close_out_orders.csv");
        test_support::TempFile end_trades_file("pipeline_close_out_trades.csv");
        std::ofstream end_orders = end_orders_file.open();
        std::ofstream end_trades = end_trades_file.open();
        L2MarketMakerBase end_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                               .base_spread = 0.5,
                                                               .inventory_limit = 100,
                                                               .tick_size = 1.0,
                                                               .signal_mode = L2SignalMode::Baseline});
        MatchingEngine end_engine(1.0, FeeSchedule{.maker_rate = 0.0002,
                                                   .taker_rate = 0.0005,
                                                   .quantity_scale = 1,
                                                   .unit_notional_usd = 10.0});
        Portfolio end_portfolio(1, 1000.0, 10.0);
        Pipeline end_pipeline(end_orders, end_trades, end_strategy, end_engine, end_portfolio, 1.0);
        // The collateral call is what tells the pipeline what a contract is worth, and the close prices its exit in
        // basis points of that notional: a pipeline without it values a contract at its price, which is 100 times the
        // ten dollar face value this instrument has.
        end_pipeline.set_collateral(1000.0, 10.0, 1, 1.0);
        end_pipeline.process_l2_market_view(L2MarketView{.symbol = "L2",
                                                         .ts = 10,
                                                         .top = TopOfBook{100.0, 100, 101.0, 100},
                                                         .micro_price = 100.5});
        // The queue in front of the bid is taken and the order fills: the book ends long ten contracts.
        end_pipeline.process_l2_market_trade(MarketTrade{11, "L2", 100.0, 200, Side::Sell, 1, "deribit"});
        assert(end_pipeline.summary().trade_reports == 1);
        const auto* open = end_portfolio.find_position("L2");
        assert(open != nullptr && open->quantity == 10);

        // The market prints again before the close, which is what a book with liquidity on it looks like: the trade
        // that filled us consumed the displayed bid, so the touch has to be re-shown for the exit to have a price to
        // cross.
        end_pipeline.process_l2_market_view(L2MarketView{.symbol = "L2",
                                                         .ts = 12,
                                                         .top = TopOfBook{100.0, 100, 101.0, 100},
                                                         .micro_price = 100.5});
        // The in-process close is the limit path: its fill is synchronous and its spread is the half spread the touch
        // crosses. A live-like close (the `pull` test's pump) is the market path and is priced from its fills instead.
        const WindDownSummary wind_down = end_pipeline.close_out(13);
        assert(wind_down.attempted());
        assert(wind_down.position == 10);
        assert(wind_down.closed_quantity == 10);
        assert(wind_down.residual == 0);
        assert(!wind_down.refused);
        // Ten contracts of a ten dollar face: the notional the two costs are measured against.
        assert(std::abs(wind_down.notional_usd - 100.0) < 1e-9);
        // Sold at the bid, marked at the midpoint: the distance is the half spread the mark was pretending not to
        // pay, and the fee is the taker rate on the same notional.
        assert(std::abs(wind_down.spread_bps - 0.5 / 100.5 * 10'000.0) < 1e-9);
        assert(std::abs(wind_down.fees_bps - 5.0) < 1e-9);
        assert(std::abs(wind_down.total_bps - (wind_down.spread_bps + wind_down.fees_bps)) < 1e-12);
        // The account is flat, so the [PNL] that follows is a closed result rather than a mark.
        const auto* flat = end_portfolio.find_position("L2");
        assert(flat != nullptr && flat->quantity == 0);
        end_portfolio.mark_to_market({{"L2", 100.5}});
        const PnlSummary pnl = end_pipeline.summary().book_pnl;
        assert(std::abs(pnl.net_pnl_usd - pnl.closed_pnl_usd) < 1e-12);
        // The close paid the taker rate, so the run's fee bill is bigger than the maker fills alone.
        assert(end_portfolio.metrics().taker_fees > 0.0);
    }

    // A band puts the closing IOC through the book instead of at the touch. The in-process engine still fills at its
    // own bid, so the closed quantity is the same; what changes is what the summary reports -- the band is the ceiling
    // a live venue's book may cost, not the price the fill gets.
    {
        test_support::TempFile band_orders_file("pipeline_close_out_band_orders.csv");
        test_support::TempFile band_trades_file("pipeline_close_out_band_trades.csv");
        std::ofstream band_orders = band_orders_file.open();
        std::ofstream band_trades = band_trades_file.open();
        L2MarketMakerBase band_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                .base_spread = 0.5,
                                                                .inventory_limit = 100,
                                                                .tick_size = 1.0,
                                                                .signal_mode = L2SignalMode::Baseline});
        MatchingEngine band_engine(1.0, FeeSchedule{.maker_rate = 0.0002,
                                                    .taker_rate = 0.0005,
                                                    .quantity_scale = 1,
                                                    .unit_notional_usd = 10.0});
        Portfolio band_portfolio(1, 1000.0, 10.0);
        Pipeline band_pipeline(band_orders, band_trades, band_strategy, band_engine, band_portfolio, 1.0);
        band_pipeline.set_collateral(1000.0, 10.0, 1, 1.0);
        band_pipeline.set_close_out_cross_bps(50.0);
        band_pipeline.process_l2_market_view(L2MarketView{.symbol = "BAND",
                                                          .ts = 10,
                                                          .top = TopOfBook{100.0, 100, 101.0, 100},
                                                          .micro_price = 100.5});
        band_pipeline.process_l2_market_trade(MarketTrade{11, "BAND", 100.0, 200, Side::Sell, 1, "deribit"});
        const auto* band_open = band_portfolio.find_position("BAND");
        assert(band_open != nullptr && band_open->quantity == 10);
        band_pipeline.process_l2_market_view(L2MarketView{.symbol = "BAND",
                                                          .ts = 12,
                                                          .top = TopOfBook{100.0, 100, 101.0, 100},
                                                          .micro_price = 100.5});
        const WindDownSummary banded = band_pipeline.close_out(13);
        assert(banded.attempted());
        assert(banded.closed_quantity == 10);
        assert(banded.residual == 0);
        assert(std::abs(banded.spread_bps - 50.0) < 1e-9);
        const auto* band_flat = band_portfolio.find_position("BAND");
        assert(band_flat != nullptr && band_flat->quantity == 0);
    }

    // A close the venue cannot finish says so. The exit is an IOC, so it takes what the touch displays and leaves the
    // rest: reporting a partial close as a finished one would be the same lie as marking it.
    {
        test_support::TempFile thin_orders_file("pipeline_close_out_thin_orders.csv");
        test_support::TempFile thin_trades_file("pipeline_close_out_thin_trades.csv");
        std::ofstream thin_orders = thin_orders_file.open();
        std::ofstream thin_trades = thin_trades_file.open();
        L2MarketMakerBase thin_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                .base_spread = 0.5,
                                                                .inventory_limit = 100,
                                                                .tick_size = 1.0,
                                                                .signal_mode = L2SignalMode::Baseline});
        MatchingEngine thin_engine(1.0, FeeSchedule{.maker_rate = 0.0002,
                                                    .taker_rate = 0.0005,
                                                    .quantity_scale = 1,
                                                    .unit_notional_usd = 10.0});
        Portfolio thin_portfolio(1, 1000.0, 10.0);
        Pipeline thin_pipeline(thin_orders, thin_trades, thin_strategy, thin_engine, thin_portfolio, 1.0);
        thin_pipeline.set_collateral(1000.0, 10.0, 1, 1.0);
        thin_pipeline.process_l2_market_view(L2MarketView{.symbol = "THIN",
                                                          .ts = 10,
                                                          .top = TopOfBook{100.0, 5, 101.0, 5},
                                                          .micro_price = 100.5});
        thin_pipeline.process_l2_market_trade(MarketTrade{11, "THIN", 100.0, 200, Side::Sell, 1, "deribit"});
        const auto* carried = thin_portfolio.find_position("THIN");
        assert(carried != nullptr && carried->quantity == 10);
        // The bid shows five, so five of the ten can be closed and five cannot.
        thin_pipeline.process_l2_market_view(L2MarketView{.symbol = "THIN",
                                                          .ts = 12,
                                                          .top = TopOfBook{100.0, 5, 101.0, 5},
                                                          .micro_price = 100.5});
        const WindDownSummary partial = thin_pipeline.close_out(13);
        assert(partial.closed_quantity == 5);
        assert(partial.residual == 5);
        const auto* left = thin_portfolio.find_position("THIN");
        assert(left != nullptr && left->quantity == 5);
    }

    // What a handover would cost, for the runs that cannot cross the touch: the residual is priced against the far side
    // of the touch plus a tick, with a taker fee on top, and the two costs are pinned here by hand on an instrument whose
    // unit value is a fixed ten dollars. This is the screen `--settle-residual` prints, and it exists because the close
    // above is the one an ended replay never gets.
    {
        test_support::TempFile settle_orders_file("pipeline_settle_orders.csv");
        test_support::TempFile settle_trades_file("pipeline_settle_trades.csv");
        std::ofstream settle_orders = settle_orders_file.open();
        std::ofstream settle_trades = settle_trades_file.open();
        L2MarketMakerBase settle_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                   .base_spread = 0.5,
                                                                   .inventory_limit = 100,
                                                                   .tick_size = 1.0,
                                                                   .signal_mode = L2SignalMode::Baseline});
        MatchingEngine settle_engine(1.0, FeeSchedule{.maker_rate = 0.0002,
                                                      .taker_rate = 0.0005,
                                                      .quantity_scale = 1,
                                                      .unit_notional_usd = 10.0});
        Portfolio settle_portfolio(1, 1000.0, 10.0);
        Pipeline settle_pipeline(settle_orders, settle_trades, settle_strategy, settle_engine,
                                 settle_portfolio, 1.0);
        settle_pipeline.set_collateral(1000.0, 10.0, 1, 1.0);
        settle_pipeline.process_l2_market_view(L2MarketView{.symbol = "SETTLE",
                                                            .ts = 10,
                                                            .top = TopOfBook{100.0, 100, 101.0, 100},
                                                            .micro_price = 100.5});
        settle_pipeline.process_l2_market_trade(MarketTrade{11, "SETTLE", 100.0, 200, Side::Sell, 1, "deribit"});
        // No close: the run ends long ten contracts, which is the state a window hands over.
        const auto* held = settle_portfolio.find_position("SETTLE");
        assert(held != nullptr && held->quantity == 10);
        settle_portfolio.mark_to_market({{"SETTLE", 100.5}});

        const ResidualSettlement settlement = settle_pipeline.settle_residual(5.0, 1.0);
        assert(settlement.attempted());
        assert(settlement.position == 10);
        assert(std::abs(settlement.mark_price - 100.5) < 1e-9);
        // A long is sold one tick past the bid, not at it.
        assert(std::abs(settlement.settle_price - 99.0) < 1e-9);
        // Ten contracts of a ten dollar face: 100 dollars of notional marked at the midpoint, crossed 1.5 lower.
        assert(std::abs(settlement.slippage_usd - 100.0 * 1.5 / 100.5) < 1e-9);
        // Five basis points of the same face, charged on what the exit trades.
        assert(std::abs(settlement.fees_usd - 100.0 * 5.0 / 10'000.0) < 1e-9);
        assert(std::abs(settlement.cost_usd - (settlement.slippage_usd + settlement.fees_usd)) < 1e-12);
        const PnlSummary marked = settle_pipeline.summary().pnl;
        // The same denominator every other basis point in this project divides by.
        assert(std::abs(settlement.cost_bps - settlement.cost_usd / marked.maker_notional_usd * 10'000.0) <
               1e-9);
        // The line is the mark minus the handover, and it says so rather than replacing the mark.
        assert(std::abs(settlement.net_after_settle_usd - (marked.net_pnl_usd - settlement.cost_usd)) < 1e-12);
        assert(std::abs(settlement.net_after_settle_bps -
                        settlement.net_after_settle_usd / marked.maker_notional_usd * 10'000.0) < 1e-9);
        assert(settlement.net_after_settle_usd < marked.net_pnl_usd);
    }

    // of reading the position once. The wait is bounded and it changes no decision: a venue that shows nothing inside
    // the budget still returns its residual and says `no_liquidity`, which is the one case the pump exists to tell
    // apart from a fill.
    {
        test_support::TempFile wait_orders_file("pipeline_close_out_wait_orders.csv");
        test_support::TempFile wait_trades_file("pipeline_close_out_wait_trades.csv");
        std::ofstream wait_orders = wait_orders_file.open();
        std::ofstream wait_trades = wait_trades_file.open();
        L2MarketMakerBase wait_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                .base_spread = 0.5,
                                                                .inventory_limit = 100,
                                                                .tick_size = 1.0,
                                                                .signal_mode = L2SignalMode::Baseline});
        MatchingEngine wait_engine(1.0, FeeSchedule{.maker_rate = 0.0002,
                                                    .taker_rate = 0.0005,
                                                    .quantity_scale = 1,
                                                    .unit_notional_usd = 10.0});
        Portfolio wait_portfolio(1, 1000.0, 10.0);
        Pipeline wait_pipeline(wait_orders, wait_trades, wait_strategy, wait_engine, wait_portfolio, 1.0);
        wait_pipeline.set_collateral(1000.0, 10.0, 1, 1.0);
        wait_pipeline.process_l2_market_view(L2MarketView{.symbol = "WAIT",
                                                          .ts = 10,
                                                          .top = TopOfBook{100.0, 100, 101.0, 100},
                                                          .micro_price = 100.5});
        wait_pipeline.process_l2_market_trade(MarketTrade{11, "WAIT", 100.0, 200, Side::Sell, 1, "deribit"});
        const auto* wait_open = wait_portfolio.find_position("WAIT");
        assert(wait_open != nullptr && wait_open->quantity == 10);
        // The book empties before the close: the venue has nothing to sell into, so the close waits, finds no fill,
        // and reports the ten contracts it kept rather than a fill the engine imagined.
        wait_pipeline.process_l2_market_view(L2MarketView{.symbol = "WAIT",
                                                          .ts = 12,
                                                          .top = TopOfBook{100.0, 0, 101.0, 0},
                                                          .micro_price = 100.5});
        int pumped = 0;
        wait_pipeline.set_venue_pump([&pumped] { ++pumped; });
        wait_pipeline.set_close_out_wait_ms(30);
        const WindDownSummary waited = wait_pipeline.close_out(13);
        assert(pumped >= 1);
        assert(waited.attempted());
        assert(waited.closed_quantity == 0);
        assert(waited.residual == 10);
        assert(waited.no_liquidity);
        assert(!waited.refused);
        const auto* wait_left = wait_portfolio.find_position("WAIT");
        assert(wait_left != nullptr && wait_left->quantity == 10);
    }

    // A live close pulls the strategy's quotes before it sends the exit: a quote that fills while the exit works is a
    // position on the other side, which is how the demo's last run ended short one contract. The in-process cancels are
    // synchronous, so the pump is never needed here; the cancels, the working set and the flat result are what this
    // pins.
    {
        test_support::TempFile pull_orders_file("pipeline_close_out_pull_orders.csv");
        test_support::TempFile pull_trades_file("pipeline_close_out_pull_trades.csv");
        std::ofstream pull_orders = pull_orders_file.open();
        std::ofstream pull_trades = pull_trades_file.open();
        L2MarketMakerBase pull_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                .base_spread = 0.5,
                                                                .inventory_limit = 100,
                                                                .tick_size = 1.0,
                                                                .signal_mode = L2SignalMode::Baseline});
        MatchingEngine pull_engine(1.0, FeeSchedule{.maker_rate = 0.0002,
                                                    .taker_rate = 0.0005,
                                                    .quantity_scale = 1,
                                                    .unit_notional_usd = 10.0});
        Portfolio pull_portfolio(1, 1000.0, 10.0);
        Pipeline pull_pipeline(pull_orders, pull_trades, pull_strategy, pull_engine, pull_portfolio, 1.0);
        pull_pipeline.set_collateral(1000.0, 10.0, 1, 1.0);
        pull_pipeline.process_l2_market_view(L2MarketView{.symbol = "PULL",
                                                          .ts = 10,
                                                          .top = TopOfBook{100.0, 100, 101.0, 100},
                                                          .micro_price = 100.5});
        pull_pipeline.process_l2_market_trade(MarketTrade{11, "PULL", 100.0, 200, Side::Sell, 1, "deribit"});
        const auto* pull_open = pull_portfolio.find_position("PULL");
        assert(pull_open != nullptr && pull_open->quantity == 10);
        pull_pipeline.process_l2_market_view(L2MarketView{.symbol = "PULL",
                                                          .ts = 12,
                                                          .top = TopOfBook{100.0, 100, 101.0, 100},
                                                          .micro_price = 100.5});
        const uint64_t working = pull_pipeline.summary().working_orders;
        const uint64_t cancels_before = pull_pipeline.summary().cancel_requests;
        assert(working >= 1);
        int pumped = 0;
        pull_pipeline.set_venue_pump([&pumped] { ++pumped; });
        const WindDownSummary pulled = pull_pipeline.close_out(13);
        assert(pulled.attempted());
        assert(pulled.closed_quantity == 10);
        assert(pulled.residual == 0);
        // Every working order was cancelled before the exit, and the in-process engine confirmed each cancel inside
        // the call, so the pump never had to run.
        assert(pull_pipeline.summary().cancel_requests == cancels_before + working);
        assert(pull_pipeline.summary().working_orders == 0);
        assert(pumped == 0);
        // The live exit is a reduce-only market order, so its `spread_bps` is the cash the fill moved against the
        // position's notional. This instrument's notional is a fixed ten dollars, so the price it filled at does not
        // move that cash: the spread is zero here by construction, and the fee is the taker rate on the notional.
        assert(pulled.spread_bps < 1e-6);
        assert(std::abs(pulled.fees_bps - 5.0) < 1e-9);
    }

    // A latched kill switch closes what the run is holding. Two things had to be true for this to work and neither
    // was: the switch refused *every* order, including the one that would take the position down, and the account was
    // never marked during a run, so the loss it reads carried the position in `unrealized_pnl`, which stayed at zero.
    // Here the market moves against an open long, the loss passes the limit, and the run closes at the touch -- then
    // refuses to quote again, which is what the switch is for.
    {
        test_support::TempFile halt_orders_file("pipeline_halt_orders.csv");
        test_support::TempFile halt_trades_file("pipeline_halt_trades.csv");
        std::ofstream halt_orders = halt_orders_file.open();
        std::ofstream halt_trades = halt_trades_file.open();
        L2MarketMakerBase halt_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                .base_spread = 0.5,
                                                                .inventory_limit = 100,
                                                                .tick_size = 1.0,
                                                                .signal_mode = L2SignalMode::Baseline});
        MatchingEngine halt_engine(1.0, FeeSchedule{.maker_rate = 0.0002,
                                                    .taker_rate = 0.0005,
                                                    .quantity_scale = 1,
                                                    .unit_notional_usd = 10.0});
        Portfolio halt_portfolio(1, 1000.0, 10.0);
        Pipeline halt_pipeline(halt_orders, halt_trades, halt_strategy, halt_engine, halt_portfolio, 1.0);
        halt_pipeline.set_collateral(1000.0, 10.0, 1, 1.0);
        halt_pipeline.set_kill_switch_loss_usd(0.1);
        halt_pipeline.process_l2_market_view(L2MarketView{.symbol = "HALT",
                                                          .ts = 10,
                                                          .top = TopOfBook{100.0, 100, 101.0, 100},
                                                          .micro_price = 100.5});
        halt_pipeline.process_l2_market_trade(MarketTrade{11, "HALT", 100.0, 200, Side::Sell, 1, "deribit"});
        const auto* open_position = halt_portfolio.find_position("HALT");
        assert(open_position != nullptr && open_position->quantity == 10);
        assert(!halt_pipeline.summary().kill_switch_triggered);

        // The market drops a full point: ten contracts of a ten dollar face are worth 100 USD, so this is about a
        // dollar of unrealized loss against a limit of ten cents.
        halt_pipeline.process_l2_market_view(L2MarketView{.symbol = "HALT",
                                                          .ts = 12,
                                                          .top = TopOfBook{99.0, 100, 100.0, 100},
                                                          .micro_price = 99.5});
        auto halted = halt_pipeline.summary();
        assert(halted.kill_switch_triggered);
        assert(halted.kill_switch_closed_quantity == 10);
        // The exit paid the taker rate and gave up the half spread, so the halt has a price like any other exit.
        assert(halted.kill_switch_close_bps > 5.0);
        const auto* flat_position = halt_portfolio.find_position("HALT");
        assert(flat_position != nullptr && flat_position->quantity == 0);
        assert(halted.to_log_string().find("kill_switch_closed_quantity=10") != std::string::npos);

        // And it does not quote again: the strategy keeps asking and the gate keeps refusing.
        halt_pipeline.process_l2_market_view(L2MarketView{.symbol = "HALT",
                                                          .ts = 14,
                                                          .top = TopOfBook{99.0, 100, 100.0, 100},
                                                          .micro_price = 99.5});
        halted = halt_pipeline.summary();
        assert(halted.kill_switch_rejected_orders > 0);
        const auto* still_flat = halt_portfolio.find_position("HALT");
        assert(still_flat != nullptr && still_flat->quantity == 0);
    }

    // The prints against the book. On the L2 path the midpoint is the only reference there is, so the two ways a
    // print can disagree with it are counted apart: a print that arrives before the run has a midpoint, or after the
    // depth input stopped, is stale -- a fact about the input's coverage -- and only a print against a current
    // midpoint can be dislocated, which is a fact about the print. Neither drops the print.
    {
        test_support::TempFile ref_orders_file("pipeline_trade_reference_orders.csv");
        test_support::TempFile ref_trades_file("pipeline_trade_reference_trades.csv");
        std::ofstream ref_orders = ref_orders_file.open();
        std::ofstream ref_trades = ref_trades_file.open();
        L2MarketMakerBase ref_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                               .base_spread = 0.5,
                                                               .inventory_limit = 100,
                                                               .tick_size = 0.01,
                                                               .signal_mode = L2SignalMode::Baseline});
        MatchingEngine ref_engine(0.01);
        Portfolio ref_portfolio;
        Pipeline ref_pipeline(ref_orders, ref_trades, ref_strategy, ref_engine, ref_portfolio, 0.01);
        assert(ref_pipeline.trade_reference().stale == 0);

        // Before any view: there is no midpoint, so the print is stale rather than dislocated.
        ref_pipeline.process_l2_market_trade(MarketTrade{10, "REF", 100.0, 1, Side::Sell, 1, "deribit"});
        assert(ref_pipeline.trade_reference().stale == 1);
        assert(ref_pipeline.trade_reference().dislocated == 0);

        // A view at 100.5 makes the print at 100.5 (0.00 bps) and at 100.54 (4.0 bps) both fine, and the one at
        // 100.56 (5.97 bps) dislocated: the limit is 5 bps, and the tick here is a cent, so two prices can sit on
        // either side of it.
        ref_pipeline.process_l2_market_view(L2MarketView{.symbol = "REF",
                                                         .ts = 1000,
                                                         .top = TopOfBook{100.49, 10, 100.51, 10},
                                                         .micro_price = 100.5});
        ref_pipeline.process_l2_market_trade(MarketTrade{1001, "REF", 100.5, 1, Side::Sell, 2, "deribit"});
        ref_pipeline.process_l2_market_trade(MarketTrade{1002, "REF", 100.54, 1, Side::Sell, 3, "deribit"});
        assert(ref_pipeline.trade_reference().stale == 1);
        assert(ref_pipeline.trade_reference().dislocated == 0);
        ref_pipeline.process_l2_market_trade(MarketTrade{1003, "REF", 100.56, 1, Side::Sell, 4, "deribit"});
        assert(ref_pipeline.trade_reference().dislocated == 1);

        // A print more than a second after the last midpoint is stale even though the midpoint itself is far away
        // from it: a frozen book is not a mispriced print, and counting the two together would report a depth file
        // that ended as a market full of bad ticks.
        ref_pipeline.process_l2_market_trade(
            MarketTrade{1000 + 1000001, "REF", 50.0, 1, Side::Sell, 5, "deribit"});
        assert(ref_pipeline.trade_reference().stale == 2);
        assert(ref_pipeline.trade_reference().dislocated == 1);

        // The limits come from the validation config, so a run that widens them counts less without any other change.
        ref_pipeline.set_trade_reference_limits(MarketDataValidationConfig{.max_bbo_age_ms = 1000,
                                                                          .max_trade_deviation_bps = 10.0});
        ref_pipeline.process_l2_market_view(L2MarketView{.symbol = "REF",
                                                         .ts = 2000000,
                                                         .top = TopOfBook{100.49, 10, 100.51, 10},
                                                         .micro_price = 100.5});
        ref_pipeline.process_l2_market_trade(MarketTrade{2000001, "REF", 100.56, 1, Side::Sell, 6, "deribit"});
        assert(ref_pipeline.trade_reference().dislocated == 1);
    }

    // A late fill belongs to the strategy that submitted the order, and the close that follows is priced against
    // it. The venue answers a sell quote's cancel with `sCode 51400` -- a refusal here -- and then sends the fill
    // that made the order unfillable, which is the demo's sequence: the order leaves the strategy's working set at
    // the refusal and its quantity arrives afterwards. Counting it is what makes the strategy's position the
    // account's; the demo's close read `strategy_net_position=20000` against a flat account because the fills that
    // made the position were dropped this way.
    {
        test_support::TempFile late_orders_file("pipeline_late_fill_orders.csv");
        test_support::TempFile late_trades_file("pipeline_late_fill_trades.csv");
        std::ofstream late_orders = late_orders_file.open();
        std::ofstream late_trades = late_trades_file.open();
        L2MarketMakerBase late_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                .base_spread = 1.0,
                                                                .inventory_limit = 100,
                                                                .tick_size = 0.5,
                                                                .signal_mode = L2SignalMode::Baseline});
        LateFillVenue late_venue;
        Portfolio late_portfolio(1, 1000.0);
        Pipeline late_pipeline(late_orders, late_trades, late_strategy, late_venue, late_portfolio, 0.5);
        // One cancel is answered in the demo's shape; the sweep the close performs afterwards cancels normally.
        late_venue.fill_next_cancel(1);
        // A live close pulls the working quotes before it sends the exit, which is the path the pump marks.
        late_pipeline.set_venue_pump([] {});

        late_pipeline.process_l2_market_view(L2MarketView{.symbol = "LATE",
                                                          .ts = 10,
                                                          .top = TopOfBook{100.0, 100, 100.5, 100},
                                                          .micro_price = 100.25});
        assert(late_pipeline.summary().submitted_orders == 2);

        // The mid moves a tick, so the quotes are stale and pulled: the venue refuses the sell's cancel and the
        // fill follows it. The account books the fill either way; the strategy's own inventory has to as well.
        late_pipeline.process_l2_market_view(L2MarketView{.symbol = "LATE",
                                                          .ts = 11,
                                                          .top = TopOfBook{100.5, 100, 101.0, 100},
                                                          .micro_price = 100.75});
        const auto* late_position = late_portfolio.find_position("LATE");
        assert(late_position != nullptr && late_position->quantity == -10);
        assert(late_strategy.net_position() == -10);
        assert(late_pipeline.summary().strategy_position_mismatches == 0);

        // The next view quotes again, and the close is sized against the position the late fill left: ten units
        // bought back, filled on placement, so the run ends flat on both sides and the metric agrees.
        late_pipeline.process_l2_market_view(L2MarketView{.symbol = "LATE",
                                                          .ts = 12,
                                                          .top = TopOfBook{100.5, 100, 101.0, 100},
                                                          .micro_price = 100.75});
        const WindDownSummary late_close = late_pipeline.close_out(13);
        assert(late_close.closed_quantity == 10);
        assert(late_close.residual == 0);
        const auto late_summary = late_pipeline.summary();
        const auto* late_flat = late_portfolio.find_position("LATE");
        assert(late_flat != nullptr && late_flat->quantity == 0);
        assert(late_summary.strategy_net_position == 0);
        assert(late_summary.strategy_position_mismatches == 0);
        // The fill that followed the venue's refusal is the run's own fill, not a state-machine fault: it is
        // counted apart with its quantity, and the health counter stays zero because nothing was unattributable.
        assert(late_summary.execution_quality.late_fill_reports == 1);
        assert(late_summary.execution_quality.late_fill_quantity == 10);
        assert(late_summary.execution_quality.orphan_order_reports == 0);
        assert(late_summary.to_log_string().find("late_fill_reports=1 late_fill_quantity=10") !=
               std::string::npos);
    }
    // A venue that refuses the closing order is not a book that showed no size, and until now the run could not tell
    // them apart. Measured 2026-11-21: a reduce-only sell against an account that was already short is answered
    // `REJECT <id> venue`, and that run reported `no_liquidity=1` for it -- a reader's attention sent to the book
    // instead of to the refusal. The exit ends its wait when the venue says so and reports `refused=1`.
    {
        test_support::TempFile refuse_orders_file("pipeline_close_refused_orders.csv");
        test_support::TempFile refuse_trades_file("pipeline_close_refused_trades.csv");
        std::ofstream refuse_orders = refuse_orders_file.open();
        std::ofstream refuse_trades = refuse_trades_file.open();
        L2MarketMakerBase refuse_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                                  .base_spread = 1.0,
                                                                  .inventory_limit = 100,
                                                                  .tick_size = 0.5,
                                                                  .signal_mode = L2SignalMode::Baseline});
        LateFillVenue refuse_venue;
        Portfolio refuse_portfolio(1, 1000.0);
        Pipeline refuse_pipeline(refuse_orders, refuse_trades, refuse_strategy, refuse_venue, refuse_portfolio, 0.5);
        // One sell quote's cancel is answered with the refusal and then the fill, which is how the account acquires the
        // short the exit is refused for; the close's own reduce-only sell is what this test is about.
        refuse_venue.fill_next_cancel(1);
        // The exit is refused *and* a quote fills the other way during its wait: the second fact is what `closed=0`
        // used to hide, so both are asserted below.
        bool pumped = false;
        refuse_pipeline.set_venue_pump([&refuse_venue, &pumped] {
            if (pumped) return;
            pumped = true;
            refuse_venue.fill_extra("REFUSE", exchange::Side::Sell, 100.0, 10);
        });
        refuse_pipeline.process_l2_market_view(L2MarketView{.symbol = "REFUSE",
                                                            .ts = 10,
                                                            .top = TopOfBook{100.0, 100, 100.5, 100},
                                                            .micro_price = 100.25});
        refuse_pipeline.process_l2_market_view(L2MarketView{.symbol = "REFUSE",
                                                            .ts = 11,
                                                            .top = TopOfBook{100.5, 100, 101.0, 100},
                                                            .micro_price = 100.75});
        const auto* refuse_open = refuse_portfolio.find_position("REFUSE");
        assert(refuse_open != nullptr && refuse_open->quantity == -10);
        refuse_venue.refuse_closes(true);
        // A budget that would be spent in full if the refusal were not noticed: the wait ends on the report.
        refuse_pipeline.set_close_out_wait_ms(300);
        const WindDownSummary refused_close = refuse_pipeline.close_out(12);
        assert(refused_close.attempted());
        assert(refused_close.closed_quantity == 0);
        assert(refused_close.residual == -20);
        assert(refused_close.refused);
        assert(!refused_close.no_liquidity);
        assert(refused_close.grew);
        assert(refused_close.grew_quantity == 10);
        assert(refused_close.to_log_string().find(" refused=1") != std::string::npos);
        assert(refused_close.to_log_string().find("no_liquidity") == std::string::npos);
        assert(refused_close.to_log_string().find(" grew=10") != std::string::npos);
        const auto* refuse_left = refuse_portfolio.find_position("REFUSE");
        assert(refuse_left != nullptr && refuse_left->quantity == -20);
    }



}