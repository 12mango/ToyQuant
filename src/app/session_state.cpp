#include "app/session_state.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

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
    // A header line a reader can refuse on, so a file from a later format is not read as this one.
    stream << "toyquant-session 1\n";
    stream << "symbol=" << symbol << '\n';
    stream << "last_ts=" << last_ts << '\n';
    stream << "next_order_id=" << next_order_id << '\n';
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
    std::vector<SessionOrder> orders = working_orders;
    std::sort(orders.begin(), orders.end(),
              [](const SessionOrder& left, const SessionOrder& right) { return left.id < right.id; });
    for (const SessionOrder& order : orders)
        stream << "order=" << order.id << ':' << order.symbol << ':' << side_name(order.side) << ':'
               << number(order.price) << ':' << order.filled_quantity << '\n';
    return stream.str();
}

bool SessionSnapshot::from_text(const std::string& text, SessionSnapshot& out, std::string& error)
{
    std::istringstream stream(text);
    std::string line;
    if (!std::getline(stream, line) || line != "toyquant-session 1")
    {
        error = "not a session snapshot this build can read";
        return false;
    }
    bool saw_symbol = false;
    bool saw_clock = false;
    bool saw_ids = false;
    bool saw_cash = false;
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
            out.symbol = value;
            saw_symbol = true;
        }
        else if (key == "last_ts")
        {
            if (!parse_u64(value, out.last_ts)) return fail("snapshot clock is not an integer: " + value);
            saw_clock = true;
        }
        else if (key == "next_order_id")
        {
            if (!parse_u64(value, out.next_order_id))
                return fail("snapshot next_order_id is not an integer: " + value);
            saw_ids = true;
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
            out.working_orders.push_back(order);
        }
        else
        {
            // A key this build does not know is refused rather than ignored: a snapshot is written and read by one
            // program, and a field that silently disappears is a field a resumed run is missing.
            return fail("unknown snapshot key: " + key);
        }
    }
    if (!saw_symbol || !saw_clock || !saw_ids || !saw_cash)
        return fail("snapshot is missing symbol, last_ts, next_order_id or cash");
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
    snapshot.symbol = symbol;
    snapshot.last_ts = pipeline.last_event_ts();
    snapshot.next_order_id = pipeline.next_order_id();
    snapshot.account = account.state();
    snapshot.marks = pipeline.latest_marks();
    snapshot.working_orders = pipeline.working_orders();
    return snapshot;
}

ResumeSummary resume_session(const SessionSnapshot& snapshot, Portfolio& account, IMatchingEngine& venue)
{
    account.restore(snapshot.account);

    ResumeSummary summary;
    summary.symbol = snapshot.symbol;
    summary.last_ts = snapshot.last_ts;
    summary.orders = snapshot.working_orders.size();
    summary.cash_usd = snapshot.account.cash;
    summary.legs = snapshot.account.positions.size();
    summary.next_order_id = snapshot.next_order_id;
    for (const SessionOrder& order : snapshot.working_orders)
    {
        // The query says whether the venue still holds the order and nothing else: filled and cancelled both come
        // back as unknown, which is why the second bucket is counted rather than interpreted. A fill that happened
        // while nobody was listening shows up against the venue's own statement, not here.
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
    return summary;
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
