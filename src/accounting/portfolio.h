#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/instrument_spec.h"
#include "exchange/execution_report.h"

struct PortfolioPosition
{
    int64_t quantity{0};
    double average_price{0.0};
    // When this position was opened, in the report clock's microseconds. Adding to a position keeps the
    // original moment, because the question a holding time answers is how long the exposure was carried,
    // and a position that grew was carried from the first fill. It resets when the position goes flat and
    // a later fill opens a new one, including the opening half of a flip.
    uint64_t entry_ts{0};
    // The fees paid to build the quantity still held. A round trip's cost is what it cost to get in plus
    // what it cost to get out, and both halves are charged to the account when the fill happens, so the
    // entry half is carried here until a close claims its share.
    double entry_fees{0.0};
    // Quantity traded in this symbol, both directions counted, which is not recoverable from the position:
    // a buy and a sell of the same size leave `quantity` where it was. It exists for reconciliation,
    // where a lost fill that happened to net out is still a lost fill, and it is a plain count of what
    // was applied rather than a second opinion about the position.
    uint64_t traded_quantity{0};
};

// One closing fill, as the account books it: the side the position was opened on, the price it was opened
// at, and the price this fill closed it at. A run-level PnL says nothing about the distribution of the
// trades behind it, and the distribution is what a maker's sizing, its win rate and its holding time are
// read from.
//
// `quantity` is in the unit the position is held in (raw quantity over quantity_scale), `pnl` is the
// closing portion before fees, and `fee` is what the round trip cost: this fill's fee apportioned to the
// quantity it closed plus the share of the entry fees the position was carrying, so `pnl - fee` is the
// trade's own contribution to realized PnL. `entry_ts` is the position's opening moment, so a position
// built from several fills and closed in slices reports the same entry time for each slice.
struct ClosedTrade
{
    std::string symbol;
    exchange::Side side{exchange::Side::Buy};
    double entry_price{0.0};
    double exit_price{0.0};
    double quantity{0.0};
    double pnl{0.0};
    double fee{0.0};
    uint64_t entry_ts{0};
    uint64_t exit_ts{0};
};

struct PortfolioMetrics
{
    double cash{0.0};
    double realized_pnl{0.0};
    double unrealized_pnl{0.0};
    double equity{0.0};
    double fees_paid{0.0};
    double maker_fees{0.0};
    double taker_fees{0.0};
    uint64_t maker_trade_count{0};
    uint64_t taker_trade_count{0};
    // Whether every position still open was marked at a positive price. False means `unrealized_pnl` and
    // `equity` are not numbers -- cash plus an unmarked position is not what the account is worth -- and the
    // line prints `n/a` for them rather than a cash-only figure a reader would quote as equity. A run that
    // carried a short into a closed feed was printing exactly that: `equity` equal to `cash`, with the
    // liability of the position nowhere.
    bool marks_complete{true};

    std::string to_log_string() const
    {
        std::ostringstream stream;
        stream << "cash=" << cash << " realized_pnl=" << realized_pnl << " unrealized_pnl=";
        if (marks_complete) stream << unrealized_pnl;
        else stream << "n/a";
        stream << " equity=";
        if (marks_complete) stream << equity;
        else stream << "n/a";
        stream << " fees_paid=" << fees_paid << " maker_fees=" << maker_fees
               << " taker_fees=" << taker_fees << " maker_trade_count=" << maker_trade_count
               << " taker_trade_count=" << taker_trade_count;
        // Printed only when it is not true, like the other "absent means zero" counters in this tree: a run whose
        // marks are complete prints exactly the line it printed before this field existed.
        if (!marks_complete) stream << " marks_complete=0";
        return stream.str();
    }
};

// One symbol's contribution to the book, valued at the marks the caller passes in. Signed: a long is
// positive and a short negative, because the whole point of the report is that the two cancel.
struct ExposureLeg
{
    std::string symbol;
    int64_t quantity{0};
    double notional_usd{0.0};
    // False when the run had no positive mark for this symbol, so its notional is not a number: the leg prints
    // `n/a` and the line counts it. Valuing an open position at zero and reporting no exposure is the failure
    // this flag exists to stop.
    bool marked{true};
};

// What the book is exposed to, which a single position cannot answer. One account can be long one instrument
// and short another, and then its **net** exposure is near zero while its **gross** is not: the carry line's
// position term is the net number and the fee bill is charged on the gross one. A one-symbol book reports the
// same value twice, which is the identity that makes the two columns readable.
struct PortfolioExposure
{
    std::vector<ExposureLeg> legs; // sorted by symbol, so two runs of the same book print the same line
    double gross_usd{0.0};         // the sum of |notional|
    double net_usd{0.0};           // the absolute value of the sum of signed notionals
    double fees_usd{0.0};          // fees booked to this account, either side
    std::size_t symbol_count{0};
    std::size_t unmarked{0};        // legs the run had no positive mark for, so the totals are `n/a`

    // Gross carried per unit of net: 1.0 for a single leg, and unbounded for a book that holds two legs while
    // being flat in net terms. Zero is reported as zero rather than as infinity, because a flat book has no
    // ratio to state.
    double gross_to_net() const { return net_usd > 0.0 ? gross_usd / net_usd : 0.0; }

    std::string to_log_string() const
    {
        std::ostringstream stream;
        stream << "symbols=" << symbol_count << " legs=";
        for (size_t index = 0; index < legs.size(); ++index)
        {
            if (index != 0) stream << ',';
            stream << legs[index].symbol << ':' << legs[index].quantity << ':';
            if (legs[index].marked) stream << legs[index].notional_usd;
            else stream << "n/a";
        }
        if (legs.empty()) stream << "none";
        stream << " gross_usd=";
        if (unmarked == 0) stream << gross_usd;
        else stream << "n/a";
        stream << " net_usd=";
        if (unmarked == 0) stream << net_usd;
        else stream << "n/a";
        stream << " gross_to_net=";
        if (unmarked == 0) stream << gross_to_net();
        else stream << "n/a";
        stream << " fees_usd=" << fees_usd;
        // Only when a leg could not be priced: a run whose every leg has a mark prints exactly the line it printed
        // before this field existed, which is what keeps two-book outputs and their assertions comparable.
        if (unmarked != 0) stream << " unmarked=" << unmarked;
        return stream.str();
    }
};

class Portfolio
{
   public:
    // `unit_notional_usd` mirrors InstrumentSpec::unit_notional_usd: 0 means the traded price is
    // the USD value of one quantity unit, which is the spot case. A fixed value marks an
    // instrument whose contracts have a set USD face value.
    // `multiplier` mirrors InstrumentSpec::multiplier: the USD value of one price unit in one quantity
    // unit, which is 5 for a micro E-mini and 1 for everything whose unit value is just its price.
    explicit Portfolio(uint64_t quantity_scale = 1, double initial_cash = 1000.0,
                       double unit_notional_usd = 0.0, double multiplier = 1.0)
        : quantity_scale_(quantity_scale == 0 ? 1 : quantity_scale),
          unit_notional_usd_(unit_notional_usd),
          multiplier_(multiplier),
          initial_cash_(initial_cash),
          cash_(initial_cash),
          // Matches reset(). Leaving equity at its default of zero made a summary that never
          // marks to market report equity as zero while cash was correct.
          equity_(initial_cash)
    {
    }

    // One symbol's contribution to the account's result, which the account's own line cannot show: a book with two
    // legs has to know which leg lost the money, and a sum over both legs answers a different question. `realized`
    // is the closed result net of the fees booked against that symbol, `fees` is those fees wherever they came
    // from, and `unrealized` is the open position marked at the price the caller passes.
    struct SymbolPnl
    {
        double realized_pnl{0.0};
        double fees_paid{0.0};
        double unrealized_pnl{0.0};
    };

    SymbolPnl pnl_of(const std::string& symbol, double price) const;

    // The account as it stands, for a session another process has to take over. It carries what a mark cannot
    // recover: the cash, what was realized, the fees, the two trade counts, and every position with the entry
    // price and the fee still riding on it. Equity and unrealized PnL are deliberately absent -- the next mark
    // produces them -- and so is the marking price itself, which the caller holds.
    struct State
    {
        double cash{0.0};
        double realized_pnl{0.0};
        double fees_paid{0.0};
        double maker_fees{0.0};
        double taker_fees{0.0};
        uint64_t maker_trade_count{0};
        uint64_t taker_trade_count{0};
        std::unordered_map<std::string, PortfolioPosition> positions;
        // The per-symbol split of `realized_pnl` and `fees_paid`, which a resumed run needs to print one book's
        // result: without it the account comes back and its legs do not.
        std::unordered_map<std::string, double> realized_by_symbol;
        std::unordered_map<std::string, double> fees_by_symbol;
    };

    State state() const;
    // Put an account back where `state` says it was, which is the other half of a snapshot. The mark is left
    // for the caller: equity reads as the cash until something prices the positions, so a resumed run reports
    // equity only after its first mark, which is the same rule a fresh run follows.
    void restore(const State& state);

    // One instrument's units, for an account that holds more than one. A position is already keyed by symbol;
    // what was still account-wide was how a unit of that symbol is worth anything, and two instruments need not
    // agree: a BTC perpetual has a ten dollar face value and an ETH perpetual a one dollar face value, so an
    // account valuing both at one of those prices the other leg ten times too large.
    //
    // A symbol that was never registered is valued with the constructor's figures, which is what every
    // single-instrument run relies on and what keeps its numbers where they were.
    void set_instrument(const InstrumentSpec& instrument);

    void apply(const ExecutionReport& report);
    void mark_to_market(const std::unordered_map<std::string, double>& prices);
    // The same valuation as `mark_to_market`, read instead of written. The equity series a run samples
    // needs the marked account at an arbitrary moment, and it cannot use `mark_to_market` for that: that
    // method is not const, and it overwrites the unrealized PnL and equity the kill switch reads in the
    // middle of an event. Both go through one loop, so the sampled number and the reported one are the
    // same arithmetic rather than two implementations that agree today.
    double equity_at(const std::unordered_map<std::string, double>& prices) const;
    void reset();

    // A fee that belongs to no single execution: a schedule's floor under one order's commission is charged
    // when the order ends, because that is the only moment the order's total is known. It is booked to fees
    // and to cash exactly like the fee of a fill, and deliberately not to the maker or taker split, which
    // counts the venue's own fees per execution and would be reporting a commission as a venue charge.
    //
    // `symbol` attributes it to one leg, which is what a two-book run needs to report each book's own result; an
    // empty symbol books it to the account alone, which is what the legacy paths pass.
    void charge_fee(double fee, const std::string& symbol = {});

    // One interval of a per-interval cash flow on one position: the funding a perpetual pays, the basis
    // a future pays at roll, a dividend, or the financing a position is charged. One method for all four
    // because the ledger side is identical and only the source of the rate differs.
    //
    // `interval_rate` is the rate one USD of notional earns per interval to a long position, so the
    // position's own sign decides the direction of the payment: the flow is the rate times the signed
    // notional. One rate therefore serves both sides of the same market, and a short pays exactly what
    // a long receives. A published funding rate is the rate longs pay, so it enters as `-funding_rate`;
    // a dividend is positive, financing is negative, and the basis a future pays at roll is negative on
    // the long leg.
    //
    // The notional is the USD value this class already assigns the position at `mark_price`
    // (unit_notional_at): a price-quoted unit accrues on `quantity * mark_price` and a fixed-USD-face
    // contract on its face value, so the entry price and unrealized PnL play no part.
    //
    // Cash, realized PnL and equity move by the returned amount, because an accrued flow has been paid
    // rather than marked: unlike a price move it does not wait for a closing trade. Equity tracks cash
    // so the identity mark_to_market produced still holds between marks.
    //
    // Returns the signed USD amount credited to cash. A flat position, an unknown symbol, or a
    // non-positive mark price on a price-quoted instrument accrues nothing, because none of them has a
    // notional to charge a rate to.
    double accrue_interval_flow(const std::string& symbol, double interval_rate, double mark_price);

    // One interval across the whole book, which is the shape a rate series has and the shape two legs
    // will need: a rate per symbol and a mark per symbol. A position that either map does not name
    // accrues nothing rather than being guessed at, so a leg that pays no carry, or a symbol with no
    // price, is simply absent. Returns the summed amount.
    double accrue_interval_flows(const std::unordered_map<std::string, double>& interval_rates,
                                 const std::unordered_map<std::string, double>& mark_prices);

    const std::unordered_map<std::string, PortfolioPosition>& positions() const
    {
        return positions_;
    }

    // Round-trip recording is off by default: it stores one entry per closing fill, and a run that does
    // not ask for the distribution should not pay for it. `--metrics` turns it on, and because the switch
    // is read inside the settlement it cannot change any number the account already reports.
    void record_closed_trades(bool enabled)
    {
        record_closed_trades_ = enabled;
    }

    const std::vector<ClosedTrade>& closed_trades() const
    {
        return closed_trades_;
    }

    PortfolioMetrics metrics() const
    {
        PortfolioMetrics out{cash_,       realized_pnl_, unrealized_pnl_,    equity_,           fees_paid_,
                             maker_fees_, taker_fees_,   maker_trade_count_, taker_trade_count_};
        out.marks_complete = marks_complete();
        return out;
    }

    // A run that ends holding a position it could not mark says so here rather than letting the account's
    // value stand as cash. The caller knows the fact -- its last view had no bid or offer -- and this is the
    // one place that turns it into `equity=n/a` on the line.
    void declare_unmarked() { unmarked_declared_ = true; }

    // Whether every position still open has a positive price in the marks the account was last given. A
    // position valued at zero is not a position worth zero: an unmarked short would read as no liability at
    // all, which is the difference between an equity figure and a cash figure.
    bool marks_complete() const
    {
        if (unmarked_declared_) return false;
        for (const auto& [symbol, position] : positions_)
        {
            if (position.quantity == 0) continue;
            const auto mark = last_positive_marks_.find(symbol);
            if (mark == last_positive_marks_.end() || mark->second <= 0.0) return false;
        }
        return true;
    }

    const PortfolioPosition* find_position(const std::string& symbol) const;

    // The book's exposure at a set of marks, read without moving the account, on the same contract as
    // `equity_at`. A symbol with no mark is valued at zero: a leg the run cannot price is a fact about the
    // data rather than something to hide, and the leg still appears with its quantity so a reader can see it.
    // Flat positions are omitted, because a book that closed out holds nothing to be exposed to.
    PortfolioExposure exposure(const std::unordered_map<std::string, double>& prices) const;

   private:
    // What one unit of one symbol is worth, and how many units a raw quantity is. The three numbers the
    // constructor takes are the account's default; a registered instrument overrides them for its own symbol.
    struct Valuation
    {
        uint64_t quantity_scale{1};
        double unit_notional_usd{0.0};
        double multiplier{1.0};
    };

    // A copy rather than a reference: a fill asks for it once and uses it several times, and the map is not
    // touched in between.
    Valuation valuation_of(const std::string& symbol) const
    {
        const auto found = instruments_.find(symbol);
        if (found != instruments_.end()) return found->second;
        return {.quantity_scale = quantity_scale_,
                .unit_notional_usd = unit_notional_usd_,
                .multiplier = multiplier_};
    }

    // The one valuation loop. `mark_to_market` writes its two outputs into the account, `equity_at`
    // reads only the equity, and because the arithmetic is the same the sampled curve and the final
    // [PORTFOLIO] line cannot drift apart.
    void mark_values(const std::unordered_map<std::string, double>& prices, double& unrealized_pnl,
                     double& equity) const;

    double units(const Valuation& valuation, uint64_t quantity) const
    {
        return static_cast<double>(quantity) / static_cast<double>(valuation.quantity_scale);
    }

    // USD value of one quantity unit at `price`.
    double unit_value(const Valuation& valuation, double price) const
    {
        return unit_notional_at(valuation.unit_notional_usd, valuation.multiplier, price);
    }

    // USD value of `amount` units held from `entry_price` to `price`.
    double value_of(const Valuation& valuation, double amount, double entry_price, double price) const
    {
        return position_value(valuation.unit_notional_usd, valuation.multiplier, amount, entry_price,
                              price);
    }

    // One position at one price: what it is worth now and what it has gained or lost. `mark_values` loops over the
    // account with this and `pnl_of` asks for one symbol, so the account's total and a leg's own number cannot
    // disagree -- which they could if each had its own copy of the three lines.
    struct MarkedPosition
    {
        double value{0.0};
        double unrealized_pnl{0.0};
    };

    MarkedPosition marked_position(const Valuation& valuation, const PortfolioPosition& position,
                                   double price) const
    {
        const double amount = units(valuation, static_cast<uint64_t>(std::llabs(position.quantity)));
        const double value_now = value_of(valuation, amount, position.average_price, price);
        const double value_at_entry = unit_value(valuation, position.average_price) * amount;
        if (position.quantity > 0) return {value_now, value_now - value_at_entry};
        return {-value_now, value_at_entry - value_now};
    }

    uint64_t quantity_scale_;
    double unit_notional_usd_;
    double multiplier_;
    std::unordered_map<std::string, Valuation> instruments_;
    // The last positive prices the account was marked at, kept so `metrics()` can say whether every open
    // position was actually marked rather than assuming it. Cleared on each mark call: the completeness
    // question is about the marks the account was last given, not about every price it ever saw.
    // The last positive price seen for each symbol, and the only map the valuation reads: a caller that marks one
    // symbol of a two-book account is not saying the other leg is unmarked, and a zero price never enters here.
    std::unordered_map<std::string, double> last_positive_marks_;
    bool unmarked_declared_{false};
    // The per-symbol split of the two account totals above. A sum of them is what the totals are, and a two-book
    // run reads them one leg at a time.
    std::unordered_map<std::string, double> realized_by_symbol_;
    std::unordered_map<std::string, double> fees_by_symbol_;
    double initial_cash_;
    double cash_;
    double realized_pnl_{0.0};
    double unrealized_pnl_{0.0};
    double equity_{0.0};
    double fees_paid_{0.0};
    double maker_fees_{0.0};
    double taker_fees_{0.0};
    uint64_t maker_trade_count_{0};
    uint64_t taker_trade_count_{0};
    bool record_closed_trades_{false};
    std::vector<ClosedTrade> closed_trades_;
    std::unordered_map<std::string, PortfolioPosition> positions_;
};
