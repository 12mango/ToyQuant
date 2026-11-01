#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "common/instrument_spec.h"
#include "common/market_event.h"
#include "exchange/matching_engine.h"
#include "orderbook/l2_orderbook.h"
#include "strategy/strategy.h"

// One instrument's machinery inside a run that carries several of them.
//
// It is everything the single-instrument path builds for itself: a book, a decision module, whatever the
// pipeline trades against, and the pipeline that joins them. The account is deliberately **not** here,
// because one account shared by several books is the whole point of the run; see `MultiBook`.
struct BookLeg
{
    // The instrument is a member so that its address is stable for the leg's whole life: the strategy and the
    // venue are both built with a pointer to it, which is why a leg is held behind a pointer and never moved.
    InstrumentSpec instrument;
    std::unique_ptr<L2OrderBook> book;
    std::unique_ptr<Strategy> strategy;
    // The venue, or the venue wrapped by a broker or a gateway. Held as the interface the pipeline already
    // takes, so nothing in this class knows which of the three it is.
    std::unique_ptr<IMatchingEngine> venue;
    std::unique_ptr<Pipeline> pipeline;
    // How many events this book has been given, which is the one number that says the merge delivered both
    // streams rather than one of them twice.
    uint64_t events{0};
};

// Several books, one account.
//
// The pipeline is one engine and one strategy, so a run that trades two instruments builds two of everything
// except the account: what a two-leg position *is* -- gross exposure against net -- only exists at the moment
// both legs are valued at the same time, and that read belongs to the object that holds both books.
//
// Dispatching by symbol rather than by reader is what lets the streams be merged first: the merged stream is
// one ordered sequence of several instruments' events, and the only thing that says which book an event
// belongs to is the event itself.
class MultiBook
{
   public:
    explicit MultiBook(Portfolio& account) : account_(account) {}

    // The leg is built by the caller, because that is where the fee schedule, the section overrides and the risk
    // limits are read from; this class insists only that the leg holds a book, a strategy, a venue and a
    // pipeline, and that no two legs name the same symbol. The leg is taken by pointer because nothing may move
    // it: the strategy was given the address of its instrument.
    void add_leg(std::unique_ptr<BookLeg> leg);

    // One event, to the book its symbol names. A book that the event does not belong to is refused rather
    // than created: a symbol arriving without a spec would otherwise be priced by whichever leg ran last.
    void process(const MarketEvent& event);

    // The account's exposure with every leg valued at its own last midpoint, read once. This is the number
    // the two books exist to produce: one leg alone reports its own notional, and two legs report the sum of
    // their sizes against the size of their sum.
    PortfolioExposure exposure() const;

    // Every leg's last midpoint, in one map, for the account's final mark to market.
    std::unordered_map<std::string, double> marks() const;

    // Zero when that book has not seen a two-sided quote yet, which is what `marks` then leaves out.
    double mid(const std::string& symbol) const;

    const std::vector<std::unique_ptr<BookLeg>>& legs() const
    {
        return legs_;
    }

    uint64_t events(const std::string& symbol) const;

    // Optional per-stage timing, the same contract the single-book path has: the book and market-view
    // stages are timed here when it is set, and the pipeline times its own.
    void set_profiler(StageProfiler* profiler)
    {
        profiler_ = profiler;
    }

   private:
    std::size_t leg_index(const std::string& symbol) const;

    Portfolio& account_;
    std::vector<std::unique_ptr<BookLeg>> legs_;
    std::unordered_map<std::string, std::size_t> by_symbol_;
    StageProfiler* profiler_{nullptr};
};
