#include "app/multi_book.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

void MultiBook::add_leg(std::unique_ptr<BookLeg> leg)
{
    if (!leg)
        throw std::invalid_argument("a book leg cannot be null");
    if (!leg->book || !leg->strategy || !leg->venue || !leg->pipeline)
        throw std::invalid_argument("a book leg needs its own book, strategy, venue and pipeline");
    if (by_symbol_.count(leg->instrument.symbol) > 0)
        throw std::invalid_argument("two books cannot share a symbol: " + leg->instrument.symbol +
                                    " would be one position with two owners");
    by_symbol_.emplace(leg->instrument.symbol, legs_.size());
    legs_.push_back(std::move(leg));
}

std::size_t MultiBook::leg_index(const std::string& symbol) const
{
    const auto found = by_symbol_.find(symbol);
    if (found == by_symbol_.end())
        throw std::invalid_argument("no book for symbol: " + symbol);
    return found->second;
}

void MultiBook::process(const MarketEvent& event)
{
    BookLeg& leg = *legs_[leg_index(market_event_symbol(event))];
    ++leg.events;

    // The same calls the single-book path makes, in the same order, so a two-book run differs from a one-book
    // run in how many books there are and in nothing else. A `BboQuote` is not handled because the L2 path
    // never produces one -- a depth file yields snapshots or batches -- and inventing a branch here would make
    // the two paths differ in a case neither of them has.
    std::visit(
        [&](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, MarketTrade>)
            {
                leg.pipeline->process_l2_market_trade(value);
            }
            else if constexpr (std::is_same_v<Event, MarketDepthSnapshot>)
            {
                if (profiler_ != nullptr) profiler_->begin(Stage::BookApply);
                leg.book->apply_snapshot(value);
                if (profiler_ != nullptr) profiler_->end(Stage::BookApply);
                if (profiler_ != nullptr) profiler_->begin(Stage::MarketView);
                const auto view = leg.book->market_view();
                if (profiler_ != nullptr) profiler_->end(Stage::MarketView);
                leg.pipeline->process_l2_market_view(view);
            }
            else if constexpr (std::is_same_v<Event, IncrementalBookBatch>)
            {
                if (profiler_ != nullptr) profiler_->begin(Stage::BookApply);
                leg.book->apply_incremental_batch(value);
                if (profiler_ != nullptr) profiler_->end(Stage::BookApply);
                if (profiler_ != nullptr) profiler_->begin(Stage::MarketView);
                const auto view = leg.book->market_view();
                if (profiler_ != nullptr) profiler_->end(Stage::MarketView);
                leg.pipeline->process_l2_market_view(view);
            }
        },
        event);
}

std::unordered_map<std::string, double> MultiBook::marks() const
{
    // Every leg's marks, unioned. A symbol only one leg has seen still appears: the account values what it
    // holds, and which pipeline happened to see the quote is not the account's business.
    std::unordered_map<std::string, double> merged;
    for (const auto& leg : legs_)
    {
        for (const auto& [symbol, mark] : leg->pipeline->latest_marks()) merged[symbol] = mark;
    }
    return merged;
}

PortfolioExposure MultiBook::exposure() const
{
    return account_.exposure(marks());
}

double MultiBook::mid(const std::string& symbol) const
{
    // The pipeline only keeps the marks of symbols it has seen a two-sided quote for, so a leg that has not
    // seen one yet has no entry and the mark reads as zero -- the same value `exposure` gives that leg.
    const auto marks = legs_[leg_index(symbol)]->pipeline->latest_marks();
    const auto found = marks.find(symbol);
    return found == marks.end() ? 0.0 : found->second;
}

uint64_t MultiBook::events(const std::string& symbol) const
{
    return legs_[leg_index(symbol)]->events;
}

std::vector<WindDownSummary> MultiBook::close_out()
{
    // Each book at its own clock: the two streams need not end at the same microsecond, and a close priced against
    // the wrong book's clock is a close at a touch that book never saw.
    std::vector<WindDownSummary> summaries;
    for (const auto& leg : legs_)
    {
        WindDownSummary summary = leg->pipeline->close_out(leg->pipeline->last_event_ts());
        if (summary.attempted()) summaries.push_back(std::move(summary));
    }
    return summaries;
}
