#include "lines/reconciliation.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace
{
int64_t magnitude(int64_t value)
{
    return value < 0 ? -value : value;
}
}  // namespace

std::string ReconciliationSummary::to_log_string() const
{
    return "symbols_checked=" + std::to_string(symbols_checked) +
           " quantity_breaks=" + std::to_string(quantity_breaks) +
           " max_quantity_break=" + std::to_string(max_quantity_break) +
           " net_quantity_break=" + std::to_string(net_quantity_break) +
           " gross_quantity_break=" + std::to_string(gross_quantity_break) +
           " report_count_difference=" + std::to_string(report_count_difference) +
           " fee_difference=" + std::to_string(fee_difference) +
           " fees_match=" + (fees_match ? "1" : "0") + " status=" + (broken() ? "break" : "ok");
}

ReconciliationSummary reconcile_positions(const BrokerStatement& statement, const Portfolio& books)
{
    ReconciliationSummary summary;
    const auto& positions = books.positions();

    // Both populations, because a symbol one account has never heard of is a disagreement rather than an
    // absence: the statement's symbols come from the fills, the portfolio's from the reports applied.
    std::unordered_map<std::string, int64_t> expected;
    std::unordered_map<std::string, uint64_t> traded;
    for (const auto& [symbol, entry] : statement.symbols)
    {
        expected.emplace(symbol, entry.net_quantity);
        traded.emplace(symbol, entry.filled_quantity);
    }
    for (const auto& [symbol, position] : positions)
    {
        expected.emplace(symbol, 0);
        traded.emplace(symbol, 0);
    }

    for (const auto& [symbol, statement_quantity] : expected)
    {
        ++summary.symbols_checked;
        const auto position = positions.find(symbol);
        const int64_t booked = position == positions.end() ? 0 : position->second.quantity;
        const int64_t difference = statement_quantity - booked;
        if (difference != 0)
        {
            ++summary.quantity_breaks;
            summary.max_quantity_break =
                std::max(summary.max_quantity_break, magnitude(difference));
            summary.net_quantity_break += difference;
        }
        // The gross is compared as well as the net: a fill lost on each side leaves the position exactly
        // where it was and is still two fills this strategy was never told about.
        const uint64_t booked_traded =
            position == positions.end() ? 0 : position->second.traded_quantity;
        const uint64_t statement_traded = traded.at(symbol);
        summary.gross_quantity_break += statement_traded > booked_traded
                                            ? statement_traded - booked_traded
                                            : booked_traded - statement_traded;
    }

    const PortfolioMetrics metrics = books.metrics();
    const uint64_t booked_fills = metrics.maker_trade_count + metrics.taker_trade_count;
    // The fills the strategy was sent against the fills it booked, so a pipeline that lost or duplicated
    // a report in transit is caught; the venue's own execution count is deliberately not the left side,
    // because a fill split into two prints is two reports from one execution.
    summary.report_count_difference = statement.forwarded_fills > booked_fills
                                          ? statement.forwarded_fills - booked_fills
                                          : booked_fills - statement.forwarded_fills;
    summary.fee_difference = statement.fees - metrics.fees_paid;
    // A split fill adds its parts in a different order from the one fill they came from, so the two sums
    // can differ in the last bit; a real break is a difference large next to the fees themselves.
    const double scale = std::max(1.0, std::abs(statement.fees));
    summary.fees_match = std::abs(summary.fee_difference) <= 1e-9 * scale;
    return summary;
}

std::string VenueEquityCheck::to_log_string() const
{
    std::ostringstream stream;
    stream << "venue_equity_usd=";
    if (venue_known) stream << venue_equity_usd;
    else stream << "n/a";
    stream << " ledger_equity_usd=";
    if (ledger_known) stream << ledger_equity_usd;
    else stream << "n/a";
    stream << " equity_break_usd=";
    if (venue_known && ledger_known) stream << equity_break_usd;
    else stream << "n/a";
    stream << " tolerance_usd=" << tolerance_usd << " status="
           << (!venue_known || !ledger_known ? "unknown" : broken() ? "break" : "ok");
    return stream.str();
}

VenueEquityCheck reconcile_equity(const std::optional<double>& venue_equity_usd,
                                  const std::optional<double>& ledger_equity_usd,
                                  double tolerance_usd)
{
    VenueEquityCheck check;
    check.tolerance_usd = tolerance_usd;
    check.venue_known = venue_equity_usd.has_value();
    check.ledger_known = ledger_equity_usd.has_value();
    if (check.venue_known) check.venue_equity_usd = *venue_equity_usd;
    if (check.ledger_known) check.ledger_equity_usd = *ledger_equity_usd;
    // Signed: a positive break is the venue holding more than the ledger booked, which is the direction a fee or
    // a fill the ledger never saw takes.
    if (check.venue_known && check.ledger_known)
        check.equity_break_usd = check.venue_equity_usd - check.ledger_equity_usd;
    return check;
}

std::string VenuePositionCheck::to_log_string() const
{
    const bool comparable = venue_known && ledger_known && units_agree();
    std::ostringstream stream;
    stream << "venue_units=";
    if (venue_known) stream << venue_units;
    else stream << "n/a";
    stream << " ledger_units=";
    if (ledger_known) stream << ledger_units;
    else stream << "n/a";
    // A difference between two quantities counted in different units is not a break, so it is not printed as one.
    stream << " position_break=";
    if (comparable) stream << position_break;
    else stream << "n/a";
    stream << " tolerance_units=" << tolerance_units;
    if (venue_known && ledger_known && !units_agree())
    {
        // Which two conventions were compared, named rather than assumed: a reader has to be able to tell a venue
        // holding another coin from an older relay that stated nothing.
        stream << " venue_units_per_coin=";
        if (venue_unit.stated()) stream << venue_unit.units_per_coin;
        else stream << "n/a";
        stream << " venue_coin=";
        if (!venue_unit.coin.empty()) stream << venue_unit.coin;
        else stream << "n/a";
        stream << " ledger_units_per_coin=";
        if (ledger_unit.stated()) stream << ledger_unit.units_per_coin;
        else stream << "n/a";
        stream << " ledger_coin=";
        if (!ledger_unit.coin.empty()) stream << ledger_unit.coin;
        else stream << "n/a";
    }
    stream << " status="
           << (!venue_known || !ledger_known || !units_agree() ? "unknown" : broken() ? "break" : "ok");
    return stream.str();
}

VenuePositionCheck reconcile_position(const std::optional<std::int64_t>& venue_units,
                                      const std::optional<std::int64_t>& ledger_units,
                                      std::int64_t tolerance_units, PositionUnits venue_unit,
                                      PositionUnits ledger_unit)
{
    VenuePositionCheck check;
    check.tolerance_units = tolerance_units;
    check.venue_unit = std::move(venue_unit);
    check.ledger_unit = std::move(ledger_unit);
    check.venue_known = venue_units.has_value();
    check.ledger_known = ledger_units.has_value();
    if (check.venue_known) check.venue_units = *venue_units;
    if (check.ledger_known) check.ledger_units = *ledger_units;
    // Signed: a positive break is the venue holding more than the ledger booked, which is a fill the run never heard.
    if (check.venue_known && check.ledger_known) check.position_break = check.venue_units - check.ledger_units;
    return check;
}
