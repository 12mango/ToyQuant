#include "app/session_state.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "app/multi_book.h"
#include "app/pipeline.h"
#include "exchange/matching_engine.h"

namespace
{
// The shortest text that reads back as the same double. A snapshot that loses the last bit of a cash balance is a
// snapshot that describes a different account, so this is exactness before tidiness.
std::string number(double value)
{
    std::array<char, 40> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

bool parse_double(const std::string& text, double& out)
{
    if (text.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) return false;
    out = parsed;
    return true;
}

bool parse_u64(const std::string& text, uint64_t& out)
{
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_i64(const std::string& text, int64_t& out)
{
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

Side parse_side(const std::string& text)
{
    if (text == "buy") return Side::Buy;
    if (text == "sell") return Side::Sell;
    return Side::Unknown;
}

const char* side_name(Side side)
{
    return side == Side::Sell ? "sell" : "buy";
}

// `a:b:c` split, used by the records that have more than one field. An empty piece is kept, so a record with a
// missing field fails the arity check rather than silently shifting its columns.
std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> pieces;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, separator)) pieces.push_back(current);
    if (!text.empty() && text.back() == separator) pieces.emplace_back();
    return pieces;
}
}  // namespace

std::string SessionSnapshot::to_text() const
{
    std::ostringstream stream;
    // A header line a reader can refuse on, so a file from a later format is not read as this one. Version 2 adds
    // the per-book records and the per-symbol PnL split; version 1 is still read, because a restart reads a file an
    // older build wrote.
    stream << "toyquant-session 2\n";
    stream << "cash=" << number(account.cash) << '\n';
    stream << "realized_pnl=" << number(account.realized_pnl) << '\n';
    stream << "fees_paid=" << number(account.fees_paid) << '\n';
    stream << "maker_fees=" << number(account.maker_fees) << '\n';
    stream << "taker_fees=" << number(account.taker_fees) << '\n';
    stream << "maker_trade_count=" << account.maker_trade_count << '\n';
    stream << "taker_trade_count=" << account.taker_trade_count << '\n';
    // Sorted, so two snapshots of the same state are the same file and a diff means something.
    std::vector<std::string> names;
    names.reserve(account.positions.size());
    for (const auto& [name, position] : account.positions) names.push_back(name);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names)
    {
        const PortfolioPosition& position = account.positions.at(name);
        stream << "position=" << name << ':' << position.quantity << ':' << number(position.average_price)
               << ':' << number(position.entry_fees) << ':' << position.entry_ts << ':'
               << position.traded_quantity << '\n';
    }
    std::vector<std::string> marked;
    marked.reserve(marks.size());
    for (const auto& [name, mark] : marks) marked.push_back(name);
    std::sort(marked.begin(), marked.end());
    for (const std::string& name : marked)
        stream << "mark=" << name << ':' << number(marks.at(name)) << '\n';
    // The per-symbol split of the two account totals: a resumed run reports one book's result, and a total alone
    // cannot say which leg produced it. The union of both maps, so a symbol that only paid fees or only realized is
    // written once with the other field zero.
    std::vector<std::string> pnl_names;
    for (const auto& [name, value] : account.realized_by_symbol) pnl_names.push_back(name);
    for (const auto& [name, value] : account.fees_by_symbol)
    {
        if (account.realized_by_symbol.count(name) == 0) pnl_names.push_back(name);
    }
    std::sort(pnl_names.begin(), pnl_names.end());
    for (const std::string& name : pnl_names)
    {
        const auto realized = account.realized_by_symbol.find(name);
        const auto fees = account.fees_by_symbol.find(name);
        stream << "symbol_pnl=" << name << ':'
               << number(realized == account.realized_by_symbol.end() ? 0.0 : realized->second) << ':'
               << number(fees == account.fees_by_symbol.end() ? 0.0 : fees->second) << '\n';
    }
    // The books, then their orders: a book record has to precede the orders it owns, or a reader has to guess which
    // venue an order belongs to.
    std::vector<SessionBook> sorted_books = books;
    std::sort(sorted_books.begin(), sorted_books.end(),
              [](const SessionBook& left, const SessionBook& right) { return left.symbol < right.symbol; });
    for (const SessionBook& book : sorted_books)
    {
        stream << "book=" << book.symbol << ':' << book.last_ts << ':' << book.next_order_id << '\n';
    }
    for (const SessionBook& book : sorted_books)
    {
        std::vector<SessionOrder> orders = book.working_orders;
        std::sort(orders.begin(), orders.end(),
                  [](const SessionOrder& left, const SessionOrder& right) { return left.id < right.id; });
        for (const SessionOrder& order : orders)
        {
            stream << "order=" << order.id << ':' << order.symbol << ':' << side_name(order.side) << ':'
                   << number(order.price) << ':' << order.filled_quantity << '\n';
        }
    }
    return stream.str();
}

bool SessionSnapshot::from_text(const std::string& text, SessionSnapshot& out, std::string& error)
{
    std::istringstream stream(text);
    std::string line;
    // Both formats start with the same key=value lines and differ in how the books are recorded: version 1 wrote the
    // single book's symbol, clock and id at the top level, version 2 writes one `book=` record per instrument. A
    // restart reads whatever the previous build wrote, so both are accepted and the version only decides the parse.
    if (!std::getline(stream, line) || (line != "toyquant-session 1" && line != "toyquant-session 2"))
    {
        error = "not a session snapshot this build can read";
        return false;
    }
    const bool single_book_format = line == "toyquant-session 1";
    // The single-book format's three top-level fields, held until the end so a version 1 file becomes one book.
    SessionBook legacy_book;
    bool saw_symbol = false;
    bool saw_clock = false;
    bool saw_ids = false;
    bool saw_cash = false;
    // Orders are collected by symbol and attached after every book record is known, because a file is only ordered
    // by convention and a reader that depended on it would be one reordering away from losing an order.
    std::unordered_map<std::string, std::vector<SessionOrder>> orders_by_symbol;
    const auto fail = [&error](const std::string& message)
    {
        error = message;
        return false;
    };
    while (std::getline(stream, line))
    {
        if (line.empty()) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) return fail("snapshot line without a key: " + line);
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);

        if (key == "symbol")
        {
            legacy_book.symbol = value;
            saw_symbol = true;
        }
        else if (key == "last_ts")
        {
            if (!parse_u64(value, legacy_book.last_ts))
                return fail("snapshot clock is not an integer: " + value);
            saw_clock = true;
        }
        else if (key == "next_order_id")
        {
            if (!parse_u64(value, legacy_book.next_order_id))
                return fail("snapshot next_order_id is not an integer: " + value);
            saw_ids = true;
        }
        else if (key == "book")
        {
            const std::vector<std::string> fields = split(value, ':');
            SessionBook book;
            if (fields.size() != 3 || fields[0].empty() || !parse_u64(fields[1], book.last_ts) ||
                !parse_u64(fields[2], book.next_order_id))
            {
                return fail("snapshot book is malformed: " + value);
            }
            book.symbol = fields[0];
            out.books.push_back(std::move(book));
        }
        else if (key == "symbol_pnl")
        {
            const std::vector<std::string> fields = split(value, ':');
            double realized = 0.0;
            double fees = 0.0;
            if (fields.size() != 3 || fields[0].empty() || !parse_double(fields[1], realized) ||
                !parse_double(fields[2], fees))
            {
                return fail("snapshot symbol_pnl is malformed: " + value);
            }
            out.account.realized_by_symbol[fields[0]] = realized;
            out.account.fees_by_symbol[fields[0]] = fees;
        }
        else if (key == "cash")
        {
            if (!parse_double(value, out.account.cash)) return fail("snapshot cash is not a number: " + value);
            saw_cash = true;
        }
        else if (key == "realized_pnl")
        {
            if (!parse_double(value, out.account.realized_pnl)) return fail("snapshot realized_pnl: " + value);
        }
        else if (key == "fees_paid")
        {
            if (!parse_double(value, out.account.fees_paid)) return fail("snapshot fees_paid: " + value);
        }
        else if (key == "maker_fees")
        {
            if (!parse_double(value, out.account.maker_fees)) return fail("snapshot maker_fees: " + value);
        }
        else if (key == "taker_fees")
        {
            if (!parse_double(value, out.account.taker_fees)) return fail("snapshot taker_fees: " + value);
        }
        else if (key == "maker_trade_count")
        {
            if (!parse_u64(value, out.account.maker_trade_count))
                return fail("snapshot maker_trade_count: " + value);
        }
        else if (key == "taker_trade_count")
        {
            if (!parse_u64(value, out.account.taker_trade_count))
                return fail("snapshot taker_trade_count: " + value);
        }
        else if (key == "position")
        {
            const std::vector<std::string> fields = split(value, ':');
            PortfolioPosition position;
            if (fields.size() != 6 || fields[0].empty() || !parse_i64(fields[1], position.quantity) ||
                !parse_double(fields[2], position.average_price) ||
                !parse_double(fields[3], position.entry_fees) || !parse_u64(fields[4], position.entry_ts) ||
                !parse_u64(fields[5], position.traded_quantity))
            {
                return fail("snapshot position is malformed: " + value);
            }
            out.account.positions[fields[0]] = position;
        }
        else if (key == "mark")
        {
            const std::vector<std::string> fields = split(value, ':');
            double mark = 0.0;
            if (fields.size() != 2 || fields[0].empty() || !parse_double(fields[1], mark))
                return fail("snapshot mark is malformed: " + value);
            out.marks[fields[0]] = mark;
        }
        else if (key == "order")
        {
            const std::vector<std::string> fields = split(value, ':');
            SessionOrder order;
            if (fields.size() != 5 || !parse_u64(fields[0], order.id) || fields[1].empty() ||
                !parse_double(fields[3], order.price) || !parse_u64(fields[4], order.filled_quantity))
            {
                return fail("snapshot order is malformed: " + value);
            }
            order.symbol = fields[1];
            order.side = parse_side(fields[2]);
            if (order.side == Side::Unknown) return fail("snapshot order side: " + fields[2]);
            orders_by_symbol[order.symbol].push_back(order);
        }
        else
        {
            // A key this build does not know is refused rather than ignored: a snapshot is written and read by one
            // program, and a field that silently disappears is a field a resumed run is missing.
            return fail("unknown snapshot key: " + key);
        }
    }
    if (!saw_cash) return fail("snapshot is missing cash");
    // A version 1 file carries one book and it is the top-level triple; a version 2 file carries one `book=` record
    // per instrument. Neither shape is inferred from the other.
    if (single_book_format)
    {
        if (!saw_symbol || !saw_clock || !saw_ids)
            return fail("snapshot is missing symbol, last_ts or next_order_id");
        out.books.push_back(std::move(legacy_book));
    }
    if (out.books.empty()) return fail("snapshot names no book");
    for (const auto& [symbol, orders] : orders_by_symbol)
    {
        const auto book = std::find_if(out.books.begin(), out.books.end(),
                                       [&symbol](const SessionBook& candidate)
                                       { return candidate.symbol == symbol; });
        if (book == out.books.end())
            return fail("snapshot has an order for a book it does not name: " + symbol);
        book->working_orders.insert(book->working_orders.end(), orders.begin(), orders.end());
    }
    return true;
}

std::string ResumeSummary::to_log_string() const
{
    std::ostringstream stream;
    stream << "symbol=" << symbol << " last_ts=" << last_ts << " orders=" << orders
           << " live=" << orders_live << " gone=" << orders_gone << " cancelled=" << cancelled
           << " cash_usd=" << cash_usd << " legs=" << legs << " next_order_id=" << next_order_id;
    return stream.str();
}

SessionSnapshot capture_session(const Pipeline& pipeline, const Portfolio& account,
                                const std::string& symbol)
{
    SessionSnapshot snapshot;
    snapshot.account = account.state();
    snapshot.marks = pipeline.latest_marks();
    SessionBook book;
    book.symbol = symbol;
    book.last_ts = pipeline.last_event_ts();
    book.next_order_id = pipeline.next_order_id();
    book.working_orders = pipeline.working_orders();
    snapshot.books.push_back(std::move(book));
    return snapshot;
}

SessionSnapshot capture_session(const MultiBook& books, const Portfolio& account)
{
    // One account and one record per leg. Each leg's clock and id sequence are its own, because each leg trades its
    // own venue: an id that is free in one book may already be taken in the other.
    SessionSnapshot snapshot;
    snapshot.account = account.state();
    snapshot.marks = books.marks();
    for (const auto& leg : books.legs())
    {
        SessionBook book;
        book.symbol = leg->instrument.symbol;
        book.last_ts = leg->pipeline->last_event_ts();
        book.next_order_id = leg->pipeline->next_order_id();
        book.working_orders = leg->pipeline->working_orders();
        snapshot.books.push_back(std::move(book));
    }
    return snapshot;
}

std::vector<ResumeSummary> resume_session(
    const SessionSnapshot& snapshot, Portfolio& account,
    const std::unordered_map<std::string, IMatchingEngine*>& venues)
{
    account.restore(snapshot.account);

    std::vector<ResumeSummary> summaries;
    summaries.reserve(snapshot.books.size());
    for (const SessionBook& book : snapshot.books)
    {
        // A book the snapshot names and this run does not trade is refused before anything is resolved: the account
        // has already been restored at that point, so half a recovery would be the worst of both.
        const auto venue_entry = venues.find(book.symbol);
        if (venue_entry == venues.end() || venue_entry->second == nullptr)
            throw std::invalid_argument("session snapshot names a book this run does not trade: " + book.symbol);
        IMatchingEngine& venue = *venue_entry->second;

        ResumeSummary summary;
        summary.symbol = book.symbol;
        summary.last_ts = book.last_ts;
        summary.orders = book.working_orders.size();
        summary.cash_usd = snapshot.account.cash;
        summary.legs = snapshot.account.positions.size();
        summary.next_order_id = book.next_order_id;
        for (const SessionOrder& order : book.working_orders)
        {
            // The query says whether the venue still holds the order and nothing else: filled and cancelled both
            // come back as unknown, which is why the second bucket is counted rather than interpreted. A fill that
            // happened while nobody was listening shows up against the venue's own statement, not here.
            if (venue.query_order(order.id) != OrderQuery::Working)
            {
                ++summary.orders_gone;
                continue;
            }
            ++summary.orders_live;
            venue.cancel_order(order.id);
            // The order is asked about again, because a cancel that did not take is a recovery that did not finish:
            // `cancelled` is the count that says so, and it can be smaller than `live`.
            if (venue.query_order(order.id) != OrderQuery::Working) ++summary.cancelled;
        }
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

bool write_snapshot(const std::string& path, const SessionSnapshot& snapshot, std::string& error)
{
    const std::string partial = path + ".partial";
    {
        std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            error = "cannot write session snapshot: " + partial;
            return false;
        }
        stream << snapshot.to_text();
        stream.flush();
        if (!stream)
        {
            error = "session snapshot write failed: " + partial;
            return false;
        }
    }
    // Renamed into place, so a reader sees the previous snapshot or this one and never half of either.
    std::error_code code;
    std::filesystem::rename(partial, path, code);
    if (code)
    {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        error = "cannot move the session snapshot into place: " + path;
        return false;
    }
    return true;
}

bool read_snapshot(const std::string& path, SessionSnapshot& out, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "cannot read session snapshot: " + path;
        return false;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    SessionSnapshot parsed;
    if (!SessionSnapshot::from_text(text.str(), parsed, error)) return false;
    out = std::move(parsed);
    return true;
}
