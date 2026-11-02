// What a reader does with input nobody promised it. A market data file is evidence in the sense that the report built
// from it is only as good as its worst row, and the two failure modes that matter are opposite: a row that is silently
// misread poisons every number downstream, and a row that crashes the process takes the run with it. Every case here
// is one of those, and what is asserted is that the reader says *which* row and *what* is wrong with it -- a
// rejection that cannot be located in a two hundred megabyte file is barely better than no rejection at all.
//
// The tolerant half matters as much: CRLF endings, a missing final newline, a byte order mark and blank lines are
// shapes other people's files have, and a reader that refused them would refuse real data.

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "market/market_data_adapter.h"
#include "orderbook/l2_orderbook.h"
#include "support/test_support.h"

namespace
{
constexpr const char* kTradeHeader = "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n";
constexpr const char* kDepthHeader =
    "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n";

// Reads a reader to its end and returns the message it threw, or an empty string when it read the whole file. A
// reader that throws nothing on hostile input is the failure this test exists to catch, so the empty string is
// checked for at every call site rather than returned quietly.
std::string read_to_error(IMarketEventReader& reader)
{
    try
    {
        MarketEvent event;
        std::size_t guard = 0;
        while (reader.next(event))
            if (++guard > 64) throw std::runtime_error("the reader never stopped");
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return {};
}

bool contains(const std::string& message, std::string_view needle)
{
    return message.find(needle) != std::string::npos;
}

// One hostile row, and the words its rejection has to contain.
struct RowCase
{
    const char* name;
    const char* row;
    const char* expected;
};

void check_trade_rows()
{
    const RowCase cases[] = {
        {"nan price", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,nan,190",
         "price must be finite and positive"},
        {"infinite price", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,inf,190",
         "price must be finite and positive"},
        {"negative price", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,-6421.5,190",
         "price must be finite and positive"},
        {"zero price", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,0,190",
         "price must be finite and positive"},
        {"negative amount", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,6421.5,-190",
         "snapshot quantity must be finite and non-negative"},
        {"non numeric id", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,abc,buy,6421.5,190",
         "invalid unsigned integer"},
        {"negative id", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,-7,buy,6421.5,190",
         "invalid unsigned integer"},
        {"uppercase side", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,BUY,6421.5,190",
         "invalid Deribit trade side"},
        {"empty side", "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,,6421.5,190",
         "invalid Deribit trade side"},
        {"truncated row", "deribit,BTC-PERPETUAL,1585699201296000", "expected 8 fields"},
        {"empty timestamp", "deribit,BTC-PERPETUAL,,1585699201311299,70745369,buy,6421.5,190",
         "invalid unsigned integer"},
        {"overflowing timestamp",
         "deribit,BTC-PERPETUAL,99999999999999999999999,1585699201311299,70745369,buy,6421.5,190",
         "invalid unsigned integer"},
    };
    for (const RowCase& item : cases)
    {
        // The blank line before the bad row makes the row number an assertion rather than a repetition of the loop:
        // line 2 is empty and skipped, so the rejection has to name line 3.
        test_support::TempFile file("malformed_trade.csv");
        file.write(std::string(kTradeHeader) + "\n" + item.row + "\n");
        auto reader = make_deribit_trade_reader(file.path().string());
        const std::string message = read_to_error(*reader);
        assert(!message.empty() && "a hostile trade row was accepted");
        assert(contains(message, item.expected));
        assert(contains(message, "row 3:") && "the rejection does not say which row it came from");
    }
}
void check_depth_rows()
{
    const RowCase cases[] = {
        {"bad boolean", "deribit,BTC-PERPETUAL,1585699200245000,1585699200355684,maybe,bid,6421.5,100",
         "invalid boolean value"},
        {"nan price", "deribit,BTC-PERPETUAL,1585699200245000,1585699200355684,true,bid,nan,100",
         "price must be finite and positive"},
        {"negative amount", "deribit,BTC-PERPETUAL,1585699200245000,1585699200355684,true,bid,6421.5,-100",
         "snapshot quantity must be finite and non-negative"},
        {"non numeric timestamp", "deribit,BTC-PERPETUAL,notanumber,1585699200355684,true,bid,6421.5,100",
         "invalid unsigned integer"},
        {"truncated row", "deribit,BTC-PERPETUAL,1585699200245000", "expected 8 fields"},
    };
    for (const RowCase& item : cases)
    {
        test_support::TempFile file("malformed_depth.csv");
        // Line 2 is blank and skipped, so the bad row is line 3 -- the same arrangement the trade rows use, so a
        // reader that counted lines instead of rows would be caught.
        file.write(std::string(kDepthHeader) + "\n" + item.row + "\n");
        auto reader = make_deribit_depth_reader(file.path().string());
        const std::string message = read_to_error(*reader);
        assert(!message.empty() && "a hostile depth row was accepted");
        assert(contains(message, item.expected));
        assert(contains(message, "row 3:") && "the rejection does not say which row it came from");
    }
}

// A message reader: the action throws or the case failed. `expected` is the phrase the message has to carry, so a
// rejection that says nothing useful counts as a failure.
template <typename Action>
void expect_invalid(Action action, std::string_view expected)
{
    try
    {
        action();
    }
    catch (const std::invalid_argument& error)
    {
        assert(contains(error.what(), expected));
        return;
    }
    assert(false && "an impossible book was accepted");
}

// A snapshot the venue should never have published. These are refused by the book rather than by the reader, because
// they are well formed rows that describe an impossible market.
void check_impossible_books()
{
    const MarketDepthSnapshot base = [] {
        MarketDepthSnapshot snapshot{.ts = 100, .symbol = "BTC", .sequence = 1};
        snapshot.bids = {{.price = 99.0, .quantity = 10}};
        snapshot.asks = {{.price = 101.0, .quantity = 10}};
        return snapshot;
    }();
    {
        L2OrderBook book;
        book.apply_snapshot(base);
        assert(book.crossed_batches() == 0);
        assert(book.top_of_book().bid_size == 10);
    }
    {
        // A snapshot that fails leaves the book as it was: nothing is half applied, which is what the aside-and-swap
        // in `apply_snapshot` buys.
        L2OrderBook book;
        MarketDepthSnapshot crossed = base;
        crossed.bids = {{.price = 102.0, .quantity = 10}};
        expect_invalid([&] { book.apply_snapshot(crossed); }, "L2 snapshot is crossed");
        assert(book.top_of_book().bid_price == 0.0);
    }
    {
        L2OrderBook book;
        MarketDepthSnapshot zero = base;
        zero.bids = {{.price = 99.0, .quantity = 0}};
        expect_invalid([&] { book.apply_snapshot(zero); }, "invalid L2 bid level");
    }
    {
        L2OrderBook book;
        MarketDepthSnapshot ascending = base;
        ascending.bids = {{.price = 98.0, .quantity = 1}, {.price = 99.0, .quantity = 1}};
        expect_invalid([&] { book.apply_snapshot(ascending); }, "strictly descending");
    }
    {
        L2OrderBook book;
        MarketDepthSnapshot one_sided = base;
        one_sided.asks.clear();
        expect_invalid([&] { book.apply_snapshot(one_sided); }, "must contain both sides");
    }
    {
        L2OrderBook book;
        MarketDepthSnapshot nan_price = base;
        nan_price.bids = {{.price = std::nan(""), .quantity = 1}};
        expect_invalid([&] { book.apply_snapshot(nan_price); }, "invalid L2 bid level");
    }
}

// An incremental batch is a diff rather than a state, so it is checked field by field. Each of these used to report
// the same four words -- "invalid L2 incremental update" -- which named neither the row nor the field.
void check_incremental_updates()
{
    const auto batch = [](std::initializer_list<IncrementalBookUpdate> updates)
    {
        return IncrementalBookBatch{.ts = 200,
                                    .exchange_ts = 200,
                                    .local_ts = 201,
                                    .symbol = "BTC",
                                    .updates = IncrementalUpdates(updates),
                                    .exchange = "deribit"};
    };
    const auto update = [](Side side, double price, uint64_t amount)
    {
        return IncrementalBookUpdate{.exchange_ts = 200,
                                     .local_ts = 201,
                                     .is_snapshot = false,
                                     .side = side,
                                     .price = price,
                                     .amount = amount};
    };

    L2OrderBook book;
    book.apply_snapshot(MarketDepthSnapshot{.ts = 100,
                                            .symbol = "BTC",
                                            .sequence = 1,
                                            .bids = {{.price = 99.0, .quantity = 10}},
                                            .asks = {{.price = 101.0, .quantity = 10}}});
    expect_invalid([&] { book.apply_incremental_batch(batch({update(Side::Unknown, 100.0, 5)})); },
                   "side is neither bid nor ask");
    expect_invalid([&] { book.apply_incremental_batch(batch({update(Side::Buy, std::nan(""), 5)})); },
                   "price is not a finite number");
    expect_invalid([&] { book.apply_incremental_batch(batch({update(Side::Buy, 0.0, 5)})); },
                   "price is not positive");
    {
        auto no_timestamp = batch({update(Side::Buy, 100.0, 5)});
        no_timestamp.updates[0].exchange_ts = 0;
        expect_invalid([&] { book.apply_incremental_batch(no_timestamp); }, "no exchange timestamp");
    }
    {
        auto mixed_timestamps = batch({update(Side::Buy, 100.0, 5)});
        mixed_timestamps.updates[0].local_ts = 999;
        expect_invalid([&] { book.apply_incremental_batch(mixed_timestamps); },
                       "local timestamp disagrees with its batch");
    }
    // The book still holds what it held: a rejected batch is not half applied, and none of the rejections above
    // contributed to the crossed count.
    assert(std::abs(book.top_of_book().bid_price - 99.0) < 1e-9);
    assert(book.crossed_batches() == 0);
}

// A batch that leaves the book crossed is the one case the reader cannot catch: both sides are well formed and the
// state they leave behind is impossible. The book reports it as nothing to trade against, which is right, and counts
// it, which turns a silent skip into a number -- a run that ignored a crossed snapshot used to read exactly like a run
// whose strategy chose not to quote.
void check_crossed_batches_are_counted()
{
    L2OrderBook book;
    book.apply_snapshot(MarketDepthSnapshot{.ts = 100,
                                            .symbol = "BTC",
                                            .sequence = 1,
                                            .bids = {{.price = 99.0, .quantity = 10}},
                                            .asks = {{.price = 101.0, .quantity = 10}}});
    assert(book.crossed_batches() == 0);

    book.apply_incremental_batch(IncrementalBookBatch{.ts = 200,
                                                      .exchange_ts = 200,
                                                      .local_ts = 201,
                                                      .symbol = "BTC",
                                                      .updates = {{.exchange_ts = 200,
                                                                   .local_ts = 201,
                                                                   .is_snapshot = false,
                                                                   .side = Side::Sell,
                                                                   .price = 98.0,
                                                                   .amount = 5}},
                                                      .exchange = "deribit"});
    assert(book.crossed_batches() == 1);
    // Crossed reads as no market at all rather than as a market with a negative spread, and the levels are still
    // there: only the top is refused.
    assert(book.top_of_book().bid_price == 0.0);
    assert(book.top_of_book().ask_price == 0.0);
    assert(book.depth(Side::Buy) == 1);

    // The complementing update removes the level that crossed the book, and the book is tradeable again. The count
    // stays where it was: it counts batches that were crossed, it is not a state.
    book.apply_incremental_batch(IncrementalBookBatch{.ts = 300,
                                                      .exchange_ts = 300,
                                                      .local_ts = 301,
                                                      .symbol = "BTC",
                                                      .updates = {{.exchange_ts = 300,
                                                                   .local_ts = 301,
                                                                   .is_snapshot = false,
                                                                   .side = Side::Sell,
                                                                   .price = 98.0,
                                                                   .amount = 0}},
                                                      .exchange = "deribit"});
    assert(book.crossed_batches() == 1);
    assert(std::abs(book.top_of_book().bid_price - 99.0) < 1e-9);
    assert(std::abs(book.top_of_book().ask_price - 101.0) < 1e-9);
}

// The shapes other people's files have. None of these is an error, and a reader that refused them would refuse real
// data -- so what is asserted is the value that comes out the other side.
void check_awkward_but_legal_input()
{
    // CRLF, a byte order mark on the header, a blank line and no final newline.
    {
        test_support::TempFile file("awkward_trade.csv");
        std::string content;
        content += "\xEF\xBB\xBF";
        content += kTradeHeader;
        content += "\r\n";
        content += "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,6421.5,190\r\n";
        content += "deribit,BTC-PERPETUAL,1585699202296000,1585699202311299,70745370,sell,6422.0,190";
        file.write(content);
        auto reader = make_deribit_trade_reader(file.path().string());
        MarketEvent event;
        assert(reader->next(event));
        const auto& first = std::get<MarketTrade>(event);
        assert(std::abs(first.price - 6421.5) < 1e-9);
        assert(first.quantity == 190);
        assert(first.aggressor_side == Side::Buy);
        assert(reader->next(event));
        assert(std::get<MarketTrade>(event).aggressor_side == Side::Sell);
        assert(!reader->next(event) && "a trailing blank line produced an event");
    }
    // A header with a column the reader does not know, and rows in that wider shape: the extra column is ignored and
    // the eight it needs are still found by name.
    {
        test_support::TempFile file("wide_trade.csv");
        file.write("exchange,symbol,timestamp,local_timestamp,id,side,price,amount,unused\n"
                   "deribit,BTC-PERPETUAL,1585699201296000,1585699201311299,70745369,buy,6421.5,190,x\n");
        auto reader = make_deribit_trade_reader(file.path().string());
        MarketEvent event;
        assert(reader->next(event));
        assert(std::get<MarketTrade>(event).quantity == 190);
    }
    // A header whose columns are in another order. Names are what the reader resolves, so this has to work.
    {
        test_support::TempFile file("reordered_trade.csv");
        file.write("amount,price,side,id,local_timestamp,timestamp,symbol,exchange\n"
                   "190,6421.5,buy,70745369,1585699201311299,1585699201296000,BTC-PERPETUAL,deribit\n");
        auto reader = make_deribit_trade_reader(file.path().string());
        MarketEvent event;
        assert(reader->next(event));
        assert(std::abs(std::get<MarketTrade>(event).price - 6421.5) < 1e-9);
    }
    // A file that holds only a header is empty rather than broken: the reader reports no events and does not throw.
    {
        test_support::TempFile file("header_only.csv");
        file.write(kTradeHeader);
        auto reader = make_deribit_trade_reader(file.path().string());
        MarketEvent event;
        assert(!reader->next(event));
    }
    // A missing column is named, at open time rather than on the first row.
    {
        test_support::TempFile file("missing_column.csv");
        file.write("exchange,symbol,timestamp,local_timestamp,id,side,price\n");
        std::string message;
        try
        {
            make_deribit_trade_reader(file.path().string());
        }
        catch (const std::invalid_argument& error)
        {
            message = error.what();
        }
        assert(contains(message, "missing trade column: amount"));
    }
}
}  // namespace

int main()
{
    check_trade_rows();
    check_depth_rows();
    check_impossible_books();
    check_incremental_updates();
    check_crossed_batches_are_counted();
    check_awkward_but_legal_input();
    return 0;
}
