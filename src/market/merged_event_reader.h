#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "market/market_data_adapter.h"

// Several readers delivered as one ordered stream.
//
// A replay of one instrument already merges two sources: its trades and its depth. A run that carries two
// books into one account has four sources -- two per instrument -- and the merge is the same operation one
// level up: hold one event from each reader and hand out the earliest.
//
// The order is decided by the venue's own timestamp, and a tie goes to the reader that was passed first, so
// two events stamped at the same microsecond are resolved by the caller's list rather than by which file the
// operating system happened to read first. That is what makes a multi-instrument run reproducible: the same
// inputs in the same order deliver the same sequence, and therefore the same decisions.
//
// It implements the single-stream reader interface rather than growing the feed a second merge loop, so both
// the direct path and the threaded one take it already merged, and a test can drive it with no feed at all.
class MergedMarketEventReader : public IMarketEventReader
{
   public:
    // An empty list, or a null entry, is refused: a reader that silently yields nothing would make an empty
    // run look like a window with no trades in it.
    explicit MergedMarketEventReader(std::vector<std::unique_ptr<IMarketEventReader>> readers);

    [[nodiscard]] bool next(MarketEvent& event) override;

   private:
    // Priming costs one event per reader and happens on the first call, so a run that never asks for an event
    // reads nothing.
    void prime();

    struct Head
    {
        MarketEvent event;
        bool has_event{false};
    };

    std::vector<std::unique_ptr<IMarketEventReader>> readers_;
    std::vector<Head> heads_;
    bool primed_{false};
};
