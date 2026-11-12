#include "lines/order_recorder.h"

#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "market/market_event_recorder.h"

namespace
{
constexpr std::string_view kHeader =
    "seq,kind,order_id,symbol,side,price,qty,ts,exec_type,cancel_reason,reject_reason,liquidity_role,fee,"
    "owner,tif";
constexpr std::size_t kColumns = 15;

std::string unsigned_text(uint64_t value)
{
    return std::to_string(value);
}

// The shortest decimal that parses back to the same double, which is the rule the market recorder writes
// prices by: a recording loses nothing to formatting, and there is no second definition of a price in the
// tree that a reader has to be told about.
std::string price_text(double value)
{
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) throw std::runtime_error("a price could not be written to the order log");
    return std::string(buffer, result.ptr);
}

// A row is assembled field by field and joined, so a column is added by naming it here and in the header
// rather than by counting commas in a literal: a missing comma is a file whose columns shift one place.
template <typename... Fields>
std::string join_fields(const Fields&... fields)
{
    std::string row;
    bool first = true;
    const auto add = [&row, &first](std::string_view field)
    {
        if (!first) row += ',';
        first = false;
        row += field;
    };
    (add(fields), ...);
    return row;
}

bool parse_unsigned(std::string_view text, uint64_t& value)
{
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_price(std::string_view text, double& value)
{
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[noreturn]] void fail(const std::string& path, std::size_t number, const std::string& message)
{
    throw std::runtime_error(path + ":" + std::to_string(number) + ": " + message);
}

const char* kind_token(OrderLogKind kind)
{
    switch (kind)
    {
        case OrderLogKind::Submission:
            return "order";
        case OrderLogKind::Cancel:
            return "cancel";
        case OrderLogKind::Report:
            break;
    }
    return "report";
}

bool kind_of(std::string_view text, OrderLogKind& kind)
{
    if (text == "order")
    {
        kind = OrderLogKind::Submission;
        return true;
    }
    if (text == "cancel")
    {
        kind = OrderLogKind::Cancel;
        return true;
    }
    if (text == "report")
    {
        kind = OrderLogKind::Report;
        return true;
    }
    return false;
}

const char* side_token(exchange::Side side)
{
    return side == exchange::Side::Buy ? "B" : "S";
}

bool side_of(std::string_view text, exchange::Side& side)
{
    if (text == "B")
    {
        side = exchange::Side::Buy;
        return true;
    }
    if (text == "S")
    {
        side = exchange::Side::Sell;
        return true;
    }
    return false;
}

const char* tif_token(TimeInForce tif)
{
    switch (tif)
    {
        case TimeInForce::Ioc:
            return "ioc";
        case TimeInForce::Fok:
            return "fok";
        case TimeInForce::Gtc:
            break;
    }
    return "gtc";
}

bool tif_of(std::string_view text, TimeInForce& tif)
{
    if (text == "gtc")
    {
        tif = TimeInForce::Gtc;
        return true;
    }
    if (text == "ioc")
    {
        tif = TimeInForce::Ioc;
        return true;
    }
    if (text == "fok")
    {
        tif = TimeInForce::Fok;
        return true;
    }
    return false;
}

const char* exec_type_token(ExecType type)
{
    switch (type)
    {
        case ExecType::Trade:
            return "trade";
        case ExecType::PartialFill:
            return "partial";
        case ExecType::Filled:
            return "filled";
        case ExecType::Cancelled:
            return "cancelled";
        case ExecType::Resting:
            return "resting";
        case ExecType::Rejected:
            return "rejected";
    }
    return "resting";
}

bool exec_type_of(std::string_view text, ExecType& type)
{
    if (text == "trade") type = ExecType::Trade;
    else if (text == "partial") type = ExecType::PartialFill;
    else if (text == "filled") type = ExecType::Filled;
    else if (text == "cancelled") type = ExecType::Cancelled;
    else if (text == "resting") type = ExecType::Resting;
    else if (text == "rejected") type = ExecType::Rejected;
    else return false;
    return true;
}

const char* role_token(LiquidityRole role)
{
    switch (role)
    {
        case LiquidityRole::Maker:
            return "maker";
        case LiquidityRole::Taker:
            return "taker";
        case LiquidityRole::Unknown:
            break;
    }
    return "unknown";
}

bool role_of(std::string_view text, LiquidityRole& role)
{
    if (text == "maker") role = LiquidityRole::Maker;
    else if (text == "taker") role = LiquidityRole::Taker;
    else if (text == "unknown") role = LiquidityRole::Unknown;
    else return false;
    return true;
}

// The reasons already have one spelling in the tree -- `cancel_reason_name` and `reject_reason_name` decide
// what a `[GATEWAY]` line prints -- so the file uses that spelling rather than a second table that would drift
// from the first.
bool cancel_reason_of(std::string_view text, CancelReason& reason)
{
    for (const CancelReason candidate : {CancelReason::None, CancelReason::User, CancelReason::Venue,
                                        CancelReason::Ioc, CancelReason::Fok, CancelReason::SelfTrade,
                                        CancelReason::Timeout, CancelReason::PostOnly})
    {
        if (text == cancel_reason_name(candidate))
        {
            reason = candidate;
            return true;
        }
    }
    return false;
}

bool reject_reason_of(std::string_view text, RejectReason& reason)
{
    for (const RejectReason candidate : {RejectReason::None, RejectReason::Venue, RejectReason::Broker,
                                        RejectReason::Timeout, RejectReason::Risk})
    {
        if (text == reject_reason_name(candidate))
        {
            reason = candidate;
            return true;
        }
    }
    return false;
}

// Comma separated, unquoted, one row a line -- the same shape the replay CSVs use, which is why a field
// holding a comma is refused rather than escaped.
std::vector<std::string_view> split_fields(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size())
    {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string_view::npos)
        {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
    return fields;
}

OrderLogRow parse_row(const std::string& path, std::size_t number, const std::string& line,
                      uint64_t expected_sequence)
{
    const std::vector<std::string_view> fields = split_fields(line);
    if (fields.size() != kColumns)
        fail(path, number,
             "the row has " + std::to_string(fields.size()) + " columns and the header declares " +
                 std::to_string(kColumns));

    OrderLogRow row;
    row.line = number;
    uint64_t sequence = 0;
    if (!parse_unsigned(fields[0], sequence))
        fail(path, number, "the sequence number '" + std::string(fields[0]) + "' is not a number");
    if (sequence != expected_sequence)
        fail(path, number,
             "the sequence number is " + std::to_string(sequence) + " and this is row " +
                 std::to_string(expected_sequence) + ", so a row is missing or out of order");
    if (!kind_of(fields[1], row.kind))
        fail(path, number, "'" + std::string(fields[1]) + "' is not one of order, cancel or report");
    if (!parse_unsigned(fields[2], row.order_id))
        fail(path, number, "the order id '" + std::string(fields[2]) + "' is not a number");

    // A cancel carries an id and nothing else. A submission carries what the strategy asked for; a report
    // carries every field, because reproducing those fields is the whole point of the file.
    if (row.kind == OrderLogKind::Cancel)
    {
        for (std::size_t index = 3; index < fields.size(); ++index)
        {
            if (!fields[index].empty() && fields[index] != "0")
                fail(path, number,
                     "a cancel row carries an order id and nothing else, and column " +
                         std::to_string(index + 1) + " is '" + std::string(fields[index]) + "'");
        }
        return row;
    }

    row.symbol = std::string(fields[3]);
    if (!side_of(fields[4], row.side))
        fail(path, number, "'" + std::string(fields[4]) + "' is not a side (B or S)");
    if (!parse_price(fields[5], row.price))
        fail(path, number, "the price '" + std::string(fields[5]) + "' is not a number");
    if (!parse_unsigned(fields[6], row.quantity))
        fail(path, number, "the quantity '" + std::string(fields[6]) + "' is not a number");
    if (!parse_unsigned(fields[7], row.ts))
        fail(path, number, "the timestamp '" + std::string(fields[7]) + "' is not a number");
    row.owner = std::string(fields[13]);
    if (row.kind == OrderLogKind::Submission)
    {
        if (!tif_of(fields[14], row.time_in_force))
            fail(path, number, "'" + std::string(fields[14]) + "' is not a time in force (gtc, ioc or fok)");
        return row;
    }
    // A report carries no time in force, so the column is empty there; a token in it still has to be a real one,
    // because a reader that accepted any string would be accepting a column it does not understand.
    if (!fields[14].empty() && !tif_of(fields[14], row.time_in_force))
        fail(path, number, "'" + std::string(fields[14]) + "' is not a time in force (gtc, ioc or fok)");

    if (!parse_price(fields[12], row.fee))
        fail(path, number, "the fee '" + std::string(fields[12]) + "' is not a number");
    if (!exec_type_of(fields[8], row.exec_type))
        fail(path, number, "'" + std::string(fields[8]) + "' is not an execution type");
    if (!cancel_reason_of(fields[9], row.cancel_reason))
        fail(path, number, "'" + std::string(fields[9]) + "' is not a cancel reason");
    if (!reject_reason_of(fields[10], row.reject_reason))
        fail(path, number, "'" + std::string(fields[10]) + "' is not a reject reason");
    if (!role_of(fields[11], row.liquidity_role))
        fail(path, number, "'" + std::string(fields[11]) + "' is not a liquidity role");
    return row;
}
}  // namespace

std::string OrderLogSummary::to_log_string() const
{
    return "submissions=" + std::to_string(submissions) + " cancels=" + std::to_string(cancels) +
           " reports=" + std::to_string(reports);
}

OrderRecorder::OrderRecorder(std::string prefix) : prefix_(std::move(prefix))
{
    if (prefix_.empty()) throw std::invalid_argument("an order log needs a file prefix");
    path_ = prefix_ + "_orders.csv";
    // The prefix is usually out/... or a scratch directory, and the second is normally empty: a recorder
    // that failed on a missing directory would be asking for housekeeping on a file it creates itself.
    const std::filesystem::path parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
    }
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_) throw std::runtime_error("could not open the order log " + path_);
    out_ << kHeader << '\n';
}

void OrderRecorder::require_writable(std::string_view value, const char* what) const
{
    // The same rule the market recorder applies to its own fields, asked of the function that states it, so
    // the two recordings refuse the same characters.
    if (!replay_field_is_writable(value))
        throw std::invalid_argument(std::string("order log refused: the ") + what + " field '" +
                                   std::string(value) + "' holds a character the file cannot carry");
}

void OrderRecorder::write_row(std::string row)
{
    out_ << row << '\n';
    // Flushed as it goes, so a session that is stopped rather than closed loses at most this many rows.
    if (++sequence_ % 1024 == 0) out_.flush();
}

void OrderRecorder::record_submission(const exchange::Order& order)
{
    require_writable(order.symbol, "symbol");
    require_writable(order.owner, "owner");
    write_row(join_fields(unsigned_text(sequence_), std::string(kind_token(OrderLogKind::Submission)),
                          unsigned_text(order.id), order.symbol, side_token(order.side),
                          price_text(order.price), unsigned_text(order.qty), unsigned_text(order.ts),
                          std::string{}, std::string{}, std::string{}, std::string{}, std::string{},
                          order.owner, tif_token(order.time_in_force)));
    ++summary_.submissions;
}

void OrderRecorder::record_cancel(uint64_t order_id)
{
    write_row(join_fields(unsigned_text(sequence_), std::string(kind_token(OrderLogKind::Cancel)),
                          unsigned_text(order_id), std::string{}, std::string{}, std::string("0"),
                          std::string("0"), std::string("0"), std::string{}, std::string{},
                          std::string{}, std::string{}, std::string("0"), std::string{}, std::string{}));
    ++summary_.cancels;
}

void OrderRecorder::record_report(const ExecutionReport& report)
{
    require_writable(report.symbol, "symbol");
    require_writable(report.owner, "owner");
    write_row(join_fields(unsigned_text(sequence_), std::string(kind_token(OrderLogKind::Report)),
                          unsigned_text(report.order_id), report.symbol, side_token(report.side),
                          price_text(report.price), unsigned_text(report.quantity),
                          unsigned_text(report.ts), exec_type_token(report.exec_type),
                          cancel_reason_name(report.cancel_reason), reject_reason_name(report.reject_reason),
                          role_token(report.liquidity_role), price_text(report.fee), report.owner,
                          std::string{}));
    ++summary_.reports;
}

void OrderRecorder::flush()
{
    if (out_) out_.flush();
}

std::vector<OrderLogRow> read_order_log(const std::string& path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("the order log " + path + " could not be opened");

    std::vector<OrderLogRow> rows;
    std::string line;
    std::size_t number = 0;
    bool header = false;
    while (std::getline(input, line))
    {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!header)
        {
            if (line != kHeader) fail(path, number, "the first line is not the order log's header");
            header = true;
            continue;
        }
        if (line.empty()) fail(path, number, "the row is empty, so a row is missing rather than blank");
        rows.push_back(parse_row(path, number, line, rows.size()));
    }
    if (!header) fail(path, number == 0 ? 1 : number, "the file is empty: it has no header and no rows");
    return rows;
}
