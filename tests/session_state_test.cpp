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
        assert(snapshot.symbol == "BTC-PERPETUAL");
        assert(snapshot.last_ts == 10);
        assert(snapshot.next_order_id == 3);
        assert(std::abs(snapshot.marks.at("BTC-PERPETUAL") - 6400.5) < 1e-12);
    }

    SessionSnapshot parsed;
    std::string error;
    assert(SessionSnapshot::from_text(snapshot.to_text(), parsed, error));
    assert(parsed.symbol == snapshot.symbol);
    assert(parsed.last_ts == snapshot.last_ts);
    assert(parsed.next_order_id == snapshot.next_order_id);
    assert(parsed.account.cash == snapshot.account.cash);
    assert(parsed.account.realized_pnl == snapshot.account.realized_pnl);
    assert(parsed.account.fees_paid == snapshot.account.fees_paid);
    assert(parsed.marks == snapshot.marks);
    assert(parsed.working_orders.size() == snapshot.working_orders.size());
    for (std::size_t index = 0; index < parsed.working_orders.size(); ++index)
    {
        assert(parsed.working_orders[index].id == snapshot.working_orders[index].id);
        assert(parsed.working_orders[index].side == snapshot.working_orders[index].side);
        assert(parsed.working_orders[index].price == snapshot.working_orders[index].price);
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
        const ResumeSummary resumed = resume_session(snapshot, resumed_account, engine);
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
        const ResumeSummary resumed = resume_session(snapshot, resumed_account, broker);
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

    return 0;
}
