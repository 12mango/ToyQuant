#include <cassert>
#include <cmath>
#include <fstream>
#include <memory>
#include <string>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "app/session_state.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "lines/paper_broker.h"
#include "lines/reconciliation.h"
#include "orderbook/top_of_book.h"
#include "strategy/l2_market_maker_base.h"
#include "support/test_support.h"

namespace
{
constexpr double kTick = 1.0;

InstrumentSpec instrument()
{
    return InstrumentSpec{.exchange = "deribit",
                          .symbol = "BTC-PERPETUAL",
                          .tick_size = kTick,
                          .quantity_scale = 1,
                          .unit_notional_usd = 10.0,
                          .lot_size = 1,
                          .min_order_quantity = 1,
                          .maker_fee_rate = 0.0002,
                          .taker_fee_rate = 0.0005};
}

std::unique_ptr<L2MarketMakerBase> make_strategy()
{
    return std::make_unique<L2MarketMakerBase>(L2MarketMakerBaseConfig{.order_size = 1,
                                                                      .base_spread = 0.5,
                                                                      .inventory_limit = 100,
                                                                      .tick_size = kTick,
                                                                      .signal_mode = L2SignalMode::Baseline});
}

FeeSchedule fee_schedule()
{
    return FeeSchedule{.maker_rate = 0.0002,
                       .taker_rate = 0.0005,
                       .quantity_scale = 1,
                       .unit_notional_usd = 10.0};
}

L2MarketView view(uint64_t ts)
{
    return L2MarketView{.symbol = "BTC-PERPETUAL",
                        .ts = ts,
                        .top = TopOfBook{6400.0, 100, 6401.0, 100},
                        .micro_price = 6400.5};
}
}  // namespace

int main()
{
    test_support::TempFile orders_file("session_orders.csv");
    test_support::TempFile trades_file("session_trades.csv");
    std::ofstream orders = orders_file.open();
    std::ofstream trades = trades_file.open();
    const InstrumentSpec btc = instrument();

    // The text form, and the file it goes into. A snapshot that loses a digit of a cash balance describes a
    // different account, so the comparison is exact rather than approximate.
    SessionSnapshot snapshot;
    {
        MatchingEngine engine(kTick, fee_schedule());
        Portfolio account(1, 1000.0, 10.0);
        auto strategy = make_strategy();
        Pipeline pipeline(orders, trades, *strategy, engine, account, kTick);
        pipeline.process_l2_market_view(view(10));
        assert(pipeline.summary().submitted_orders == 2);
        assert(pipeline.working_orders().size() == 2);
        snapshot = capture_session(pipeline, account, "BTC-PERPETUAL");
        assert(snapshot.books.size() == 1);
        assert(snapshot.books.front().symbol == "BTC-PERPETUAL");
        assert(snapshot.books.front().last_ts == 10);
        assert(snapshot.books.front().next_order_id == 3);
        assert(std::abs(snapshot.marks.at("BTC-PERPETUAL") - 6400.5) < 1e-12);
    }

    SessionSnapshot parsed;
    std::string error;
    assert(SessionSnapshot::from_text(snapshot.to_text(), parsed, error));
    assert(parsed.books.size() == snapshot.books.size());
    assert(parsed.books.front().symbol == snapshot.books.front().symbol);
    assert(parsed.books.front().last_ts == snapshot.books.front().last_ts);
    assert(parsed.books.front().next_order_id == snapshot.books.front().next_order_id);
    assert(parsed.account.cash == snapshot.account.cash);
    assert(parsed.account.realized_pnl == snapshot.account.realized_pnl);
    assert(parsed.account.fees_paid == snapshot.account.fees_paid);
    assert(parsed.marks == snapshot.marks);
    const auto& parsed_orders = parsed.books.front().working_orders;
    const auto& original_orders = snapshot.books.front().working_orders;
    assert(parsed_orders.size() == original_orders.size());
    for (std::size_t index = 0; index < parsed_orders.size(); ++index)
    {
        assert(parsed_orders[index].id == original_orders[index].id);
        assert(parsed_orders[index].side == original_orders[index].side);
        assert(parsed_orders[index].price == original_orders[index].price);
    }
    // The same text twice: a snapshot of one state is one file, which is what makes a diff of two snapshots mean
    // something.
    assert(parsed.to_text() == snapshot.to_text());

    test_support::TempFile snapshot_file("session.snapshot");
    assert(write_snapshot(snapshot_file.path().string(), snapshot, error));
    SessionSnapshot read_back;
    assert(read_snapshot(snapshot_file.path().string(), read_back, error));
    assert(read_back.to_text() == snapshot.to_text());
    // A file this build did not write is refused rather than half-read.
    snapshot_file.write("toyquant-session 9\nsymbol=X\n");
    assert(!read_snapshot(snapshot_file.path().string(), read_back, error));
    assert(!error.empty());

    // A session taken over. The pipeline and its account are destroyed at the end of the block; the venue is not,
    // which is the whole point: the orders are still there, and only the client went away.
    {
        MatchingEngine engine(kTick, fee_schedule());
        Portfolio account(1, 1000.0, 10.0);
        auto strategy = make_strategy();
        Pipeline pipeline(orders, trades, *strategy, engine, account, kTick);
        pipeline.process_l2_market_view(view(10));
        assert(engine.query_order(1) == OrderQuery::Working);
        snapshot = capture_session(pipeline, account, "BTC-PERPETUAL");

        Portfolio resumed_account(1, 1000.0, 10.0);
        const std::vector<ResumeSummary> summaries =
            resume_session(snapshot, resumed_account, {{"BTC-PERPETUAL", &engine}});
        assert(summaries.size() == 1);
        const ResumeSummary& resumed = summaries.front();
        assert(resumed.orders == 2);
        assert(resumed.orders_live == 2);
        assert(resumed.cancelled == 2);
        assert(resumed.orders_gone == 0);
        assert(resumed.legs == 0);
        assert(std::abs(resumed.cash_usd - account.metrics().cash) < 1e-12);
        // Every order the snapshot listed is resolved, and the venue holds none of them any more: a new process
        // cannot have a queue position it did not stand in.
        assert(engine.query_order(1) != OrderQuery::Working);
        assert(engine.query_order(2) != OrderQuery::Working);
        // The account came back exactly: an empty book and the cash the run started with.
        assert(resumed_account.positions().empty());
        assert(std::abs(resumed_account.metrics().cash - 1000.0) < 1e-12);

        // And the new client continues the id sequence, because the venue refuses a duplicate id rather than
        // filling it twice.
        auto next_strategy = make_strategy();
        Pipeline next(orders, trades, *next_strategy, engine, resumed_account, kTick);
        next.set_next_order_id(resumed.next_order_id);
        next.process_l2_market_view(view(20));
        assert(next.summary().submitted_orders == 2);
        assert(next.working_orders().front().id == 3);
    }

    // A fill that happened while nobody was listening. The venue fills the resting bid after the client that
    // placed it is gone, so the snapshot is out of date the moment it is read: the order comes back as unknown
    // rather than working, and the account the recovery restores does not have the position. The point of this
    // block is that the disagreement is *reported* -- against the venue's own statement -- instead of being
    // silently absorbed into a wrong number.
    {
        MatchingEngine engine(kTick, fee_schedule());
        PaperBroker broker(engine);
        Portfolio account(1, 1000.0, 10.0);
        {
            auto strategy = make_strategy();
            Pipeline pipeline(orders, trades, *strategy, broker, account, kTick);
            pipeline.process_l2_market_view(view(10));
            assert(pipeline.working_orders().size() == 2);
            snapshot = capture_session(pipeline, account, "BTC-PERPETUAL");
        }
        // The market comes to the bid and takes it, with no client attached to hear about it.
        engine.process_market_trade(MarketTrade{11, "BTC-PERPETUAL", 6400.0, 200, Side::Sell, 1, "deribit"});
        assert(broker.statement().executions == 1);

        Portfolio resumed_account(1, 1000.0, 10.0);
        const std::vector<ResumeSummary> summaries =
            resume_session(snapshot, resumed_account, {{"BTC-PERPETUAL", &broker}});
        assert(summaries.size() == 1);
        const ResumeSummary& resumed = summaries.front();
        assert(resumed.orders == 2);
        // The bid was filled and the ask is still resting: one order the venue has forgotten and one it still
        // holds, which is the pair of outcomes the recovery has to tell apart.
        assert(resumed.orders_live == 1);
        assert(resumed.orders_gone == 1);
        assert(resumed.cancelled == 1);
        // The account is what the snapshot said, and the venue says otherwise: that difference is the whole reason
        // a recovery asks rather than believes.
        assert(resumed_account.positions().empty());
        const ReconciliationSummary check = reconcile_positions(broker.statement(), resumed_account);
        assert(check.broken());
        assert(check.quantity_breaks == 1);
        assert(check.max_quantity_break == 1);
    }

    // Holding the submissions is what lets a resumed run rebuild the market without repeating the decisions: the
    // views arrive, the strategy sees them, and nothing is sent until the snapshot's clock has passed.
    {
        MatchingEngine engine(kTick, fee_schedule());
        Portfolio account(1, 1000.0, 10.0);
        auto strategy = make_strategy();
        Pipeline pipeline(orders, trades, *strategy, engine, account, kTick);
        pipeline.hold_submissions_until(20);
        pipeline.process_l2_market_view(view(10));
        assert(pipeline.summary().submitted_orders == 0);
        assert(pipeline.working_orders().empty());
        pipeline.process_l2_market_view(view(21));
        assert(pipeline.summary().submitted_orders == 2);
    }

    // Two books in one snapshot. The account is written once and each book carries its own clock, id sequence and
    // working orders, because each leg trades its own venue: an id that is free in one book may already be taken in
    // the other, and a resumed run that shared one would have its second leg refused as a duplicate.
    {
        test_support::TempFile pair_orders_file("session_pair_orders.csv");
        test_support::TempFile pair_trades_file("session_pair_trades.csv");
        std::ofstream pair_orders = pair_orders_file.open();
        std::ofstream pair_trades = pair_trades_file.open();
        const InstrumentSpec eth = deribit_eth_perpetual_spec();
        Portfolio account(1, 1000.0, 10.0);
        account.set_instrument(btc);
        account.set_instrument(eth);
        MatchingEngine venue_a(btc.tick_size, fee_schedule());
        MatchingEngine venue_b(eth.tick_size, fee_schedule());
        auto strategy_a = make_strategy();
        auto strategy_b = make_strategy();
        Pipeline pipeline_a(pair_orders, pair_trades, *strategy_a, venue_a, account, btc.tick_size);
        Pipeline pipeline_b(pair_orders, pair_trades, *strategy_b, venue_b, account, eth.tick_size);
        pipeline_a.process_l2_market_view(view(10));
        pipeline_b.process_l2_market_view(L2MarketView{.symbol = "ETH-PERPETUAL",
                                                        .ts = 12,
                                                        .top = TopOfBook{130.0, 100, 131.0, 100},
                                                        .micro_price = 130.5});
        assert(pipeline_a.working_orders().size() == 2);
        assert(pipeline_b.working_orders().size() == 2);

        SessionSnapshot pair_snapshot;
        pair_snapshot.account = account.state();
        pair_snapshot.marks = pipeline_a.latest_marks();
        for (const auto& [symbol, mark] : pipeline_b.latest_marks()) pair_snapshot.marks[symbol] = mark;
        for (const Pipeline* pipeline : {&pipeline_a, &pipeline_b})
        {
            SessionBook book;
            book.symbol = pipeline == &pipeline_a ? btc.symbol : eth.symbol;
            book.last_ts = pipeline->last_event_ts();
            book.next_order_id = pipeline->next_order_id();
            book.working_orders = pipeline->working_orders();
            pair_snapshot.books.push_back(std::move(book));
        }
        assert(pair_snapshot.books.size() == 2);

        SessionSnapshot pair_parsed;
        std::string pair_error;
        assert(SessionSnapshot::from_text(pair_snapshot.to_text(), pair_parsed, pair_error));
        assert(pair_parsed.books.size() == 2);
        assert(pair_parsed.books[0].symbol == "BTC-PERPETUAL");
        assert(pair_parsed.books[1].symbol == "ETH-PERPETUAL");
        assert(pair_parsed.books[1].last_ts == 12);
        assert(pair_parsed.books[0].working_orders.size() == 2);
        assert(pair_parsed.books[1].working_orders.size() == 2);
        assert(pair_parsed.to_text() == pair_snapshot.to_text());

        // Each book's orders are resolved at that book's own venue, and a book the run does not trade is refused
        // rather than silently left with its orders at a venue nobody will cancel them at.
        Portfolio pair_account(1, 1000.0, 10.0);
        const std::vector<ResumeSummary> pair_summaries =
            resume_session(pair_parsed, pair_account, {{btc.symbol, &venue_a}, {eth.symbol, &venue_b}});
        assert(pair_summaries.size() == 2);
        assert(pair_summaries[0].symbol == "BTC-PERPETUAL" && pair_summaries[0].cancelled == 2);
        assert(pair_summaries[1].symbol == "ETH-PERPETUAL" && pair_summaries[1].cancelled == 2);
        assert(pair_summaries[1].last_ts == 12);
        assert(venue_a.query_order(1) != OrderQuery::Working);
        assert(venue_b.query_order(1) != OrderQuery::Working);
        bool refused_partial = false;
        try
        {
            Portfolio other(1, 1000.0, 10.0);
            resume_session(pair_parsed, other, {{btc.symbol, &venue_a}});
        }
        catch (const std::invalid_argument&)
        {
            refused_partial = true;
        }
        assert(refused_partial);
    }

    // The single-book format this project wrote first is still read, because a restart reads a file an older build
    // left behind.
    {
        SessionSnapshot legacy;
        std::string legacy_error;
        const std::string text =
            "toyquant-session 1\n"
            "symbol=BTC-PERPETUAL\n"
            "last_ts=99\n"
            "next_order_id=7\n"
            "cash=1000\n";
        assert(SessionSnapshot::from_text(text, legacy, legacy_error));
        assert(legacy.books.size() == 1);
        assert(legacy.books.front().symbol == "BTC-PERPETUAL");
        assert(legacy.books.front().last_ts == 99);
        assert(legacy.books.front().next_order_id == 7);
        assert(std::abs(legacy.account.cash - 1000.0) < 1e-12);
    }

    return 0;
}
