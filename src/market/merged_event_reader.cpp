#include "market/merged_event_reader.h"

#include <stdexcept>
#include <utility>

MergedMarketEventReader::MergedMarketEventReader(
    std::vector<std::unique_ptr<IMarketEventReader>> readers)
    : readers_(std::move(readers)), heads_(readers_.size())
{
    if (readers_.empty())
        throw std::invalid_argument("a merged reader requires at least one stream");
    for (const auto& reader : readers_)
    {
        if (!reader)
            throw std::invalid_argument("a merged reader cannot take a null stream");
    }
}

void MergedMarketEventReader::prime()
{
    primed_ = true;
    for (std::size_t index = 0; index < readers_.size(); ++index)
        heads_[index].has_event = readers_[index]->next(heads_[index].event);
}

bool MergedMarketEventReader::next(MarketEvent& event)
{
    if (!primed_) prime();

    // A linear scan, because the number of streams is the number of books in one run and not a workload: the
    // alternative is a heap that costs an allocation and a comparator to save three comparisons.
    std::size_t best = heads_.size();
    uint64_t best_ts = 0;
    for (std::size_t index = 0; index < heads_.size(); ++index)
    {
        if (!heads_[index].has_event) continue;
        const uint64_t ts = market_event_timestamp(heads_[index].event);
        // Strictly earlier, so a tie keeps the stream that comes first in the caller's list.
        if (best == heads_.size() || ts < best_ts)
        {
            best = index;
            best_ts = ts;
        }
    }
    if (best == heads_.size()) return false;

    event = std::move(heads_[best].event);
    // The stream that just gave an event is read again straight away, so the head is always current. A reader
    // that has run out keeps its slot with `has_event` false rather than being erased: the index is the tie
    // breaker, and a stable index is what makes the order reproducible.
    heads_[best].has_event = readers_[best]->next(heads_[best].event);
    return true;
}
