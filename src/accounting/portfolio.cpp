#include "portfolio.h"

#include <algorithm>
#include <cstdlib>

void Portfolio::reset()
{
    positions_.clear();
    closed_trades_.clear();
    realized_by_symbol_.clear();
    fees_by_symbol_.clear();
    cash_ = initial_cash_;
    realized_pnl_ = 0.0;
    unrealized_pnl_ = 0.0;
    equity_ = cash_;
    fees_paid_ = 0.0;
    maker_fees_ = 0.0;
    taker_fees_ = 0.0;
    maker_trade_count_ = 0;
    taker_trade_count_ = 0;
}

Portfolio::State Portfolio::state() const
{
    return {.cash = cash_,
            .realized_pnl = realized_pnl_,
            .fees_paid = fees_paid_,
            .maker_fees = maker_fees_,
            .taker_fees = taker_fees_,
            .maker_trade_count = maker_trade_count_,
            .taker_trade_count = taker_trade_count_,
            .positions = positions_,
            .realized_by_symbol = realized_by_symbol_,
            .fees_by_symbol = fees_by_symbol_};
}

void Portfolio::restore(const State& state)
{
    cash_ = state.cash;
    realized_pnl_ = state.realized_pnl;
    fees_paid_ = state.fees_paid;
    maker_fees_ = state.maker_fees;
    taker_fees_ = state.taker_fees;
    maker_trade_count_ = state.maker_trade_count;
    taker_trade_count_ = state.taker_trade_count;
    positions_ = state.positions;
    realized_by_symbol_ = state.realized_by_symbol;
    fees_by_symbol_ = state.fees_by_symbol;
    // A restored account is unmarked until something prices it, which is the state a run is in when it starts:
    // equity is the cash and unrealized PnL is zero. Copying the old mark in here would carry a price nobody
    // has seen since the last process died.
    unrealized_pnl_ = 0.0;
    equity_ = cash_;
}

void Portfolio::set_instrument(const InstrumentSpec& instrument)
{
    // The three numbers the valuation needs, and deliberately not the symbol: the position map is already keyed
    // by it, and a spec whose symbol disagrees with the report's would be a second place the mapping could be
    // wrong. `quantity_scale` of zero is refused at construction, so it is refused here the same way.
    instruments_[instrument.symbol] =
        Valuation{.quantity_scale = instrument.quantity_scale == 0 ? 1 : instrument.quantity_scale,
                  .unit_notional_usd = instrument.unit_notional_usd,
                  .multiplier = instrument.multiplier};
}

void Portfolio::charge_fee(double fee, const std::string& symbol)
{
    if (fee == 0.0) return;
    fees_paid_ += fee;
    cash_ -= fee;
    realized_pnl_ -= fee;
    // Attributed when the caller knows the leg, because a per-book result needs it; an empty symbol is booked to the
    // account alone, which is what the paths that predate the two-book run pass.
    if (!symbol.empty())
    {
        realized_by_symbol_[symbol] -= fee;
        fees_by_symbol_[symbol] += fee;
    }
}

void Portfolio::apply(const ExecutionReport& report)
{
    if (report.exec_type != ExecType::Trade || report.quantity == 0) return;

    auto& position = positions_[report.symbol];
    position.traded_quantity += report.executed_quantity();
    fees_paid_ += report.fee;
    fees_by_symbol_[report.symbol] += report.fee;
    if (report.liquidity_role == LiquidityRole::Maker)
    {
        maker_fees_ += report.fee;
        ++maker_trade_count_;
    }
    else if (report.liquidity_role == LiquidityRole::Taker)
    {
        taker_fees_ += report.fee;
        ++taker_trade_count_;
    }
    // One instrument's units and unit value, asked for once: the closing half, the opening half and the
    // averaging all use the same three numbers, and a symbol the account holds was registered by whoever
    // built it.
    const Valuation valuation = valuation_of(report.symbol);
    cash_ -= report.fee;
    realized_pnl_ -= report.fee;
    realized_by_symbol_[report.symbol] -= report.fee;

    // A trade can close part of an existing position and open the rest. The closing portion is
    // valued from the position's entry price and the opening portion at the trade price, so the
    // two are settled separately. For a spot style instrument the closing value reduces to
    // quantity * trade price, which is the previous behaviour.
    const double executed = static_cast<double>(report.executed_quantity());
    const auto settle_close = [&](int64_t close_quantity)
    {
        const double closed = units(valuation, static_cast<uint64_t>(close_quantity));
        const double value_now = value_of(valuation, closed, position.average_price, report.price);
        const double value_at_entry = unit_value(valuation, position.average_price) * closed;
        double pnl = 0.0;
        if (report.side == exchange::Side::Buy)
        {
            // Buying back a short: pay the current value and realise entry minus current.
            cash_ -= value_now;
            pnl = value_at_entry - value_now;
            realized_pnl_ += pnl;
            realized_by_symbol_[report.symbol] += pnl;
        }
        else
        {
            // Selling a long: receive the current value.
            cash_ += value_now;
            pnl = value_now - value_at_entry;
            realized_pnl_ += pnl;
            realized_by_symbol_[report.symbol] += pnl;
        }

        // The fee this fill paid for the quantity it closed, plus the share of the entry fees the position
        // has been carrying for that quantity. A round trip costs both halves, and a maker's edge is the
        // same order of size as the fee, so counting only the exit half would report the entry half as
        // profit. The share is taken in proportion to quantity, and a close of the whole position takes
        // all of it.
        const double closing_fee = report.fee * static_cast<double>(close_quantity) / executed;
        const double held = units(valuation, static_cast<uint64_t>(std::llabs(position.quantity)));
        const double entry_fee_share = held > 0.0 ? position.entry_fees * (closed / held) : 0.0;
        position.entry_fees -= entry_fee_share;

        if (!record_closed_trades_) return;
        // The position was opened on the other side of the fill that closes it, which is what makes a
        // round trip a long or a short rather than a buy or a sell.
        closed_trades_.push_back(
            {.symbol = report.symbol,
             .side = report.side == exchange::Side::Buy ? exchange::Side::Sell : exchange::Side::Buy,
             .entry_price = position.average_price,
             .exit_price = report.price,
             .quantity = closed,
             .pnl = pnl,
             .fee = closing_fee + entry_fee_share,
             .entry_ts = position.entry_ts,
             .exit_ts = report.ts});
    };

    int64_t remaining_quantity = static_cast<int64_t>(report.executed_quantity());
    if (report.side == exchange::Side::Buy)
    {
        if (position.quantity < 0)
        {
            const int64_t close_quantity = std::min(-position.quantity, remaining_quantity);
            settle_close(close_quantity);
            position.quantity += close_quantity;
            remaining_quantity -= close_quantity;
        }

        if (remaining_quantity > 0)
        {
            const double open = units(valuation, static_cast<uint64_t>(remaining_quantity));
            const double existing =
                units(valuation, static_cast<uint64_t>(std::max<int64_t>(0, position.quantity)));
            const double total = existing + open;
            // A position opened from flat starts its clock and its fee carry here; adding to one already
            // held keeps both.
            if (position.quantity == 0)
            {
                position.entry_ts = report.ts;
                position.entry_fees = 0.0;
            }
            // The part of this fill's fee that paid for the new quantity rides with the position until a
            // close claims its share.
            position.entry_fees += report.fee * static_cast<double>(remaining_quantity) / executed;
            position.average_price =
                total > 0.0 ? (existing * position.average_price + open * report.price) / total
                            : 0.0;
            position.quantity += remaining_quantity;
            cash_ -= unit_value(valuation, report.price) * open;
        }
    }
    else
    {
        if (position.quantity > 0)
        {
            const int64_t close_quantity = std::min(position.quantity, remaining_quantity);
            settle_close(close_quantity);
            position.quantity -= close_quantity;
            remaining_quantity -= close_quantity;
        }

        if (remaining_quantity > 0)
        {
            const double open = units(valuation, static_cast<uint64_t>(remaining_quantity));
            const double existing =
                units(valuation, static_cast<uint64_t>(std::max<int64_t>(0, -position.quantity)));
            const double total = existing + open;
            if (position.quantity == 0)
            {
                position.entry_ts = report.ts;
                position.entry_fees = 0.0;
            }
            position.entry_fees += report.fee * static_cast<double>(remaining_quantity) / executed;
            position.average_price =
                total > 0.0 ? (existing * position.average_price + open * report.price) / total
                            : 0.0;
            position.quantity -= remaining_quantity;
            cash_ += unit_value(valuation, report.price) * open;
        }
    }
}

Portfolio::SymbolPnl Portfolio::pnl_of(const std::string& symbol, double price) const
{
    SymbolPnl out;
    const auto realized = realized_by_symbol_.find(symbol);
    if (realized != realized_by_symbol_.end()) out.realized_pnl = realized->second;
    const auto fees = fees_by_symbol_.find(symbol);
    if (fees != fees_by_symbol_.end()) out.fees_paid = fees->second;
    const auto position = positions_.find(symbol);
    // A flat or unmarked position has nothing to mark: the realized part is the whole answer, which is also what a
    // run reports for a book that closed out before the window ended.
    if (position == positions_.end() || position->second.quantity == 0 || price <= 0.0) return out;
    out.unrealized_pnl = marked_position(valuation_of(symbol), position->second, price).unrealized_pnl;
    return out;
}

void Portfolio::mark_values(const std::unordered_map<std::string, double>& prices,
                            double& unrealized_pnl, double& equity) const
{
    unrealized_pnl = 0.0;
    equity = cash_;
    for (const auto& [symbol, position] : positions_)
    {
        const auto price_it = prices.find(symbol);
        if (price_it == prices.end() || position.quantity == 0) continue;

        const Valuation valuation = valuation_of(symbol);
        const MarkedPosition marked = marked_position(valuation, position, price_it->second);
        unrealized_pnl += marked.unrealized_pnl;
        equity += marked.value;
    }
}

double Portfolio::equity_at(const std::unordered_map<std::string, double>& prices) const
{
    double unrealized_pnl = 0.0;
    double equity = 0.0;
    mark_values(prices, unrealized_pnl, equity);
    return equity;
}

void Portfolio::mark_to_market(const std::unordered_map<std::string, double>& prices)
{
    // A zero or missing price is not a mark. A book whose last view had no bid or offer would otherwise value an
    // open position at nothing and report an equity that is just the cash -- which is how a carried short came to
    // read as no liability. The account is valued at the best price known for each symbol: this call's positive
    // price, or the last positive one it ever saw. That is also what `metrics()` reads to say whether every open
    // position was actually marked.
    for (const auto& [symbol, price] : prices)
        if (price > 0.0) last_positive_marks_[symbol] = price;
    mark_values(last_positive_marks_, unrealized_pnl_, equity_);
}

const PortfolioPosition* Portfolio::find_position(const std::string& symbol) const
{
    const auto it = positions_.find(symbol);
    return it == positions_.end() ? nullptr : &it->second;
}

PortfolioExposure Portfolio::exposure(const std::unordered_map<std::string, double>& prices) const
{
    PortfolioExposure out;
    double signed_sum = 0.0;
    for (const auto& [symbol, position] : positions_)
    {
        // A flat position is not an exposure, and a book that closed out should report no legs rather than a
        // leg of zero that a reader has to filter.
        if (position.quantity == 0) continue;
        const auto mark = prices.find(symbol);
        const double price = mark == prices.end() ? 0.0 : mark->second;
        const Valuation valuation = valuation_of(symbol);
        const double units = static_cast<double>(position.quantity) /
                             static_cast<double>(valuation.quantity_scale);
        const bool marked = price > 0.0;
        if (!marked) ++out.unmarked;
        out.legs.push_back(ExposureLeg{.symbol = symbol,
                                       .quantity = position.quantity,
                                       .notional_usd = marked ? units * unit_value(valuation, price) : 0.0,
                                       .marked = marked});
    }
    // Sorted by symbol: the account is a hash map, and a report whose lines move between runs is a report
    // nobody can diff.
    std::sort(out.legs.begin(), out.legs.end(),
              [](const ExposureLeg& left, const ExposureLeg& right) { return left.symbol < right.symbol; });
    for (const auto& leg : out.legs)
    {
        out.gross_usd += std::abs(leg.notional_usd);
        signed_sum += leg.notional_usd;
    }
    out.net_usd = std::abs(signed_sum);
    out.fees_usd = fees_paid_;
    out.symbol_count = out.legs.size();
    return out;
}

double Portfolio::accrue_interval_flow(const std::string& symbol, double interval_rate,
                                       double mark_price)
{
    const auto it = positions_.find(symbol);
    if (it == positions_.end() || it->second.quantity == 0) return 0.0;
    // A price-quoted unit needs a positive price before it has a notional to charge a rate to; a fixed
    // USD face value does not, because that value does not come from the price.
    const Valuation valuation = valuation_of(symbol);
    if (valuation.unit_notional_usd <= 0.0 && mark_price <= 0.0) return 0.0;

    const double notional =
        units(valuation, static_cast<uint64_t>(std::llabs(it->second.quantity))) *
        unit_value(valuation, mark_price);
    const double flow =
        it->second.quantity > 0 ? notional * interval_rate : -notional * interval_rate;
    if (flow == 0.0) return 0.0;

    cash_ += flow;
    realized_pnl_ += flow;
    realized_by_symbol_[symbol] += flow;
    equity_ += flow;
    return flow;
}

double Portfolio::accrue_interval_flows(
    const std::unordered_map<std::string, double>& interval_rates,
    const std::unordered_map<std::string, double>& mark_prices)
{
    double total = 0.0;
    // The accrual changes cash, realized PnL and equity but never positions_, so the iteration is safe
    // to hold while the single-symbol method looks the position up again.
    for (const auto& [symbol, position] : positions_)
    {
        if (position.quantity == 0) continue;
        const auto rate_it = interval_rates.find(symbol);
        if (rate_it == interval_rates.end()) continue;
        const auto mark_it = mark_prices.find(symbol);
        if (mark_it == mark_prices.end()) continue;
        total += accrue_interval_flow(symbol, rate_it->second, mark_it->second);
    }
    return total;
}
