#include <cassert>
#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "accounting/portfolio.h"
#include "app/multi_book.h"
#include "app/pipeline.h"
#include "app/run_summary.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "market/merged_event_reader.h"
#include "orderbook/l2_orderbook.h"
#include "strategy/l2_market_maker_base.h"
#include "support/test_support.h"

namespace
{
// A reader with a scripted clock: a few trades at the stamps it was given. Everything the merge has to do is
// about ordering, so a reader that can produce any order and nothing else is the whole fixture.
class CannedReader : public IMarketEventReader
{
   public:
    CannedReader(std::vector<uint64_t> stamps, std::string symbol)
        : stamps_(std::move(stamps)), symbol_(std::move(symbol))
    {
    }

    [[nodiscard]] bool next(MarketEvent& event) override
    {
        if (index_ == stamps_.size()) return false;
        event = MarketTrade{.ts = stamps_[index_++],
                            .symbol = symbol_,
                            .price = 100.0,
                            .quantity = 1,
                            .aggressor_side = Side::Buy,
                            .sequence = index_,
                            .exchange = "test"};
        return true;
    }

   private:
    std::vector<uint64_t> stamps_;
    std::string symbol_;
    std::size_t index_{0};
};

// One instrument's machinery, built the way the run builds it: a book, a decision layer, a venue and the
// pipeline that joins them, all pointed at the account it shares with the other leg.
std::unique_ptr<BookLeg> make_leg(Portfolio& account, std::ofstream& orders, std::ofstream& trades,
                                  const InstrumentSpec& instrument)
{
    auto leg = std::make_unique<BookLeg>();
    leg->instrument = instrument;
    leg->book = std::make_unique<L2OrderBook>(instrument.tick_size);
    leg->strategy = std::make_unique<L2MarketMakerBase>(
        L2MarketMakerBaseConfig{.order_size = 10,
                                .base_spread = 0.5,
                                .inventory_limit = 1000,
                                .tick_size = instrument.tick_size,
                                .signal_mode = L2SignalMode::Baseline});
    leg->venue = std::make_unique<MatchingEngine>(
        instrument.tick_size, FeeSchedule{.maker_rate = instrument.maker_fee_rate,
                                          .taker_rate = instrument.taker_fee_rate,
                                          .quantity_scale = instrument.quantity_scale,
                                          .unit_notional_usd = instrument.unit_notional_usd});
    leg->pipeline = std::make_unique<Pipeline>(orders, trades, *leg->strategy, *leg->venue, account,
                                               instrument.tick_size);
    leg->pipeline->set_collateral(1000.0, instrument.unit_notional_usd, instrument.quantity_scale,
                                  instrument.multiplier);
    return leg;
}

InstrumentSpec two_face_value_instrument(const std::string& symbol, double face_usd)
{
    // Two instruments that differ only in what one contract is worth, which is the axis a shared account has to
    // get right: a ten dollar face and a one dollar face at the same price are not the same exposure.
    return InstrumentSpec{.exchange = "deribit",
                          .symbol = symbol,
                          .tick_size = 1.0,
                          .quantity_scale = 1,
                          .unit_notional_usd = face_usd,
                          .lot_size = 1,
                          .min_order_quantity = 1,
                          .maker_fee_rate = 0.0002,
                          .taker_fee_rate = 0.0005};
}
}  // namespace

int main()
{
    // The merge: several streams, one ordered sequence, and the order is the venue's clock with a tie going to
    // the stream the caller listed first. This is what makes a two-book run reproducible rather than dependent
    // on which file the operating system read first.
    {
        std::vector<std::unique_ptr<IMarketEventReader>> streams;
        streams.push_back(std::make_unique<CannedReader>(std::vector<uint64_t>{10, 30}, "A"));
        streams.push_back(std::make_unique<CannedReader>(std::vector<uint64_t>{10, 20}, "B"));
        MergedMarketEventReader merged(std::move(streams));
        std::string seen;
        MarketEvent event;
        while (merged.next(event))
        {
            seen += market_event_symbol(event) + std::to_string(market_event_timestamp(event)) + " ";
        }
        assert(seen == "A10 B10 B20 A30 ");

        bool refused_empty = false;
        try
        {
            MergedMarketEventReader empty(std::vector<std::unique_ptr<IMarketEventReader>>{});
        }
        catch (const std::invalid_argument&)
        {
            refused_empty = true;
        }
        assert(refused_empty);
    }

    test_support::TempFile orders_file("multi_book_orders.csv");
    test_support::TempFile trades_file("multi_book_trades.csv");
    std::ofstream orders = orders_file.open();
    std::ofstream trades = trades_file.open();

    const InstrumentSpec btc = two_face_value_instrument("BTC-PERPETUAL", 10.0);
    const InstrumentSpec eth = two_face_value_instrument("ETH-PERPETUAL", 1.0);
    // The account is built with the first instrument's valuation and told about both, which is the only way a
    // shared account can hold a ten dollar face and a one dollar face in the same book.
    Portfolio account(1, 1000.0, 10.0);
    account.set_instrument(btc);
    account.set_instrument(eth);

    MultiBook books(account);
    books.add_leg(make_leg(account, orders, trades, btc));
    books.add_leg(make_leg(account, orders, trades, eth));
    assert(books.legs().size() == 2);

    // The two books are fed one interleaved sequence. Each snapshot establishes its own top of book and makes
    // that book's strategy quote at both touches; the trade then takes one side of one book, so the account
    // ends long in the first and short in the second without either book knowing about the other.
    books.process(MarketDepthSnapshot{.ts = 10,
                                      .symbol = "BTC-PERPETUAL",
                                      .sequence = 1,
                                      .bids = {{6400.0, 100}},
                                      .asks = {{6401.0, 100}},
                                      .exchange = "deribit"});
    books.process(MarketDepthSnapshot{.ts = 11,
                                      .symbol = "ETH-PERPETUAL",
                                      .sequence = 1,
                                      .bids = {{130.0, 100}},
                                      .asks = {{131.0, 100}},
                                      .exchange = "deribit"});
    books.process(MarketTrade{12, "BTC-PERPETUAL", 6400.0, 200, Side::Sell, 2, "deribit"});
    books.process(MarketTrade{13, "ETH-PERPETUAL", 131.0, 200, Side::Buy, 2, "deribit"});

    assert(books.events("BTC-PERPETUAL") == 2);
    assert(books.events("ETH-PERPETUAL") == 2);

    const auto* long_leg = account.find_position("BTC-PERPETUAL");
    const auto* short_leg = account.find_position("ETH-PERPETUAL");
    assert(long_leg != nullptr && short_leg != nullptr);
    assert(long_leg->quantity == 10);
    assert(short_leg->quantity == -10);

    // The marks are each book's own last midpoint, and the exposure values every leg at its own face value: ten
    // BTC contracts are 100 USD of face and ten ETH contracts are 10 USD, so gross is 110 and net is 90. A
    // shared account that valued both legs with one face value would report net zero here, which is the
    // failure this assertion exists for.
    assert(std::abs(books.mid("BTC-PERPETUAL") - 6400.5) < 1e-9);
    assert(std::abs(books.mid("ETH-PERPETUAL") - 130.5) < 1e-9);
    const auto exposure = books.exposure();
    assert(exposure.symbol_count == 2);
    assert(exposure.legs.size() == 2);
    // Sorted by symbol, so the report does not move between runs.
    assert(exposure.legs[0].symbol == "BTC-PERPETUAL" && exposure.legs[1].symbol == "ETH-PERPETUAL");
    assert(std::abs(exposure.legs[0].notional_usd - 100.0) < 1e-9);
    assert(std::abs(exposure.legs[1].notional_usd + 10.0) < 1e-9);
    assert(std::abs(exposure.gross_usd - 110.0) < 1e-9);
    assert(std::abs(exposure.net_usd - 90.0) < 1e-9);
    assert(std::abs(exposure.gross_to_net() - 110.0 / 90.0) < 1e-9);
    // The fees the exposure line reports are the account's, which is both books' fees: 0.02% of 100 USD plus
    // 0.02% of 10 USD.
    const auto metrics = account.metrics();
    assert(std::abs(exposure.fees_usd - metrics.fees_paid) < 1e-15);
    assert(std::abs(metrics.fees_paid - (0.02 + 0.002)) < 1e-12);

    // The account's own identity, which is what makes the two legs' contributions add up: once marked, equity is
    // the cash the run began with plus everything realized plus everything marked, and nothing in this run
    // couples the two books except their shared cash. (Equity is written by a mark rather than on every fill,
    // which is why the mark comes first.)
    account.mark_to_market(books.marks());
    const auto marked = account.metrics();
    assert(std::abs(marked.equity - (1000.0 + marked.realized_pnl + marked.unrealized_pnl)) < 1e-9);
    // Both legs moved the mark, so the account is not holding one book's PnL for both: the long leg is worth
    // more at the higher midpoint and the short leg less.
    assert(marked.unrealized_pnl > 0.0);

    // A symbol with no book is refused rather than created: a run cannot price an instrument it never built.
    bool refused_unknown = false;
    try
    {
        books.process(MarketTrade{14, "SOL-PERPETUAL", 10.0, 1, Side::Buy, 3, "deribit"});
    }
    catch (const std::invalid_argument&)
    {
        refused_unknown = true;
    }
    assert(refused_unknown);

    // The hard cap belongs to the account, and this is the A/B that says so: the same two books, the same orders,
    // the same cap on one of them -- only the provider that tells a book what the other leg already holds differs.
    // The ETH book's own second quote is 20 dollars and the BTC book is holding 100, so a 110 dollar cap refuses
    // it once the account is read as a whole and allows it when the book looks only at itself.
    const auto capped_rejections = [&](bool share_account)
    {
        test_support::TempFile capped_orders_file("multi_book_capped_orders.csv");
        test_support::TempFile capped_trades_file("multi_book_capped_trades.csv");
        std::ofstream capped_orders = capped_orders_file.open();
        std::ofstream capped_trades = capped_trades_file.open();
        Portfolio capped_account(1, 1000.0, 10.0);
        capped_account.set_instrument(btc);
        capped_account.set_instrument(eth);
        MultiBook capped_books(capped_account);
        capped_books.add_leg(make_leg(capped_account, capped_orders, capped_trades, btc));
        capped_books.add_leg(make_leg(capped_account, capped_orders, capped_trades, eth));
        capped_books.legs()[1]->pipeline->set_hard_exposure_limit_usd(110.0);
        if (share_account)
        {
            capped_books.legs()[1]->pipeline->set_account_marks(
                [&capped_books] { return capped_books.marks(); });
        }
        capped_books.process(MarketDepthSnapshot{.ts = 10,
                                                .symbol = "BTC-PERPETUAL",
                                                .sequence = 1,
                                                .bids = {{6400.0, 100}},
                                                .asks = {{6401.0, 100}},
                                                .exchange = "deribit"});
        capped_books.process(
            MarketTrade{11, "BTC-PERPETUAL", 6400.0, 200, Side::Sell, 2, "deribit"});
        capped_books.process(MarketDepthSnapshot{.ts = 12,
                                                .symbol = "ETH-PERPETUAL",
                                                .sequence = 1,
                                                .bids = {{130.0, 100}},
                                                .asks = {{131.0, 100}},
                                                .exchange = "deribit"});
        capped_books.process(MarketTrade{13, "ETH-PERPETUAL", 131.0, 200, Side::Buy, 2, "deribit"});
        // A requote several ticks away. The decision layer cancels the quotes it holds on the view that sees the
        // move and places the new ones on the view after it, so the budget is tested on the second one: the ETH
        // book is short ten contracts, and adding ten more is the risk the account's cap has to refuse.
        for (uint64_t ts = 14; ts <= 15; ++ts)
        {
            capped_books.process(MarketDepthSnapshot{.ts = ts,
                                                    .symbol = "ETH-PERPETUAL",
                                                    .sequence = ts - 12,
                                                    .bids = {{135.0, 100}},
                                                    .asks = {{136.0, 100}},
                                                    .exchange = "deribit"});
        }
        return capped_books.legs()[1]->pipeline->summary().hard_limit_rejections;
    };
    assert(capped_rejections(false) == 0);
    assert(capped_rejections(true) == 1);

    // The summary split. A book's block names its own symbol, carries its own [PNL] -- the account's line cannot say
    // which leg lost the money -- and carries no account lines; the account's block carries those once and no
    // [EXECUTION] line. What a reader greps for stays where it was for a single-book run, which is the default
    // reading asserted last.
    const RunSummary& leg_summary = books.legs()[0]->pipeline->summary();
    const std::string leg_text = leg_summary.to_log_string(books.legs()[0]->instrument.symbol, false);
    assert(leg_text.rfind("[EXECUTION] symbol=BTC-PERPETUAL ", 0) == 0);
    assert(leg_text.find("[PORTFOLIO]") == std::string::npos);
    // The per-book PnL: the long leg's own numbers, marked at its own midpoint, not the account's.
    assert(leg_text.find("[PNL] symbol=BTC-PERPETUAL gross_pnl_usd=") != std::string::npos);
    assert(leg_text.find("[PNL] gross_pnl_usd=") == std::string::npos);
    const std::string account_text = leg_summary.account_to_log_string();
    assert(account_text.find("[PORTFOLIO]") != std::string::npos);
    assert(account_text.find("[PNL]") != std::string::npos);
    assert(account_text.find("[EXECUTION]") == std::string::npos);
    assert(leg_summary.to_log_string().rfind("[EXECUTION] submitted_orders=", 0) == 0);
    assert(leg_summary.to_log_string().find("[PORTFOLIO]") != std::string::npos);

    // The per-book numbers are a split of the account rather than a second opinion on it: the two closed results add
    // up to what the account realized, the two fee bills to what it paid, and the two marks to what it is holding.
    const auto long_book = books.legs()[0]->pipeline->summary().book_pnl;
    const auto short_book = books.legs()[1]->pipeline->summary().book_pnl;
    const double closed_sum = long_book.closed_pnl_usd + short_book.closed_pnl_usd;
    assert(std::abs(closed_sum - account.metrics().realized_pnl) < 1e-12);
    const double fees_sum = (long_book.gross_closed_pnl_usd - long_book.closed_pnl_usd) +
                            (short_book.gross_closed_pnl_usd - short_book.closed_pnl_usd);
    assert(std::abs(fees_sum - account.metrics().fees_paid) < 1e-12);
    const double open_sum = (long_book.net_pnl_usd - long_book.closed_pnl_usd) +
                            (short_book.net_pnl_usd - short_book.closed_pnl_usd);
    assert(std::abs(open_sum - account.metrics().unrealized_pnl) < 1e-12);
    return 0;
}
