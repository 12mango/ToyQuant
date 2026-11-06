#pragma once

// The v2 event the feeds produce and the pipeline consumes. It is a **core type**: the order book, the strategies and
// the matching engine all name it, so it lives here rather than beside the venue readers that construct it, which is
// what keeps `common <- core <- lines <- app` readable in one direction.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "common/symbol.h"
#include "common/types.h"

struct MarketTrade
{
    uint64_t ts{};
    std::string symbol;
    double price{};
    uint64_t quantity{};
    Side aggressor_side{Side::Unknown};
    uint64_t sequence{};
    std::string exchange;
};

struct BboQuote
{
    uint64_t ts{};
    // The quote is the type that travels furthest: the pipeline builds one per event, the engine caches two, and each
    // of those used to copy the name. Sixteen bytes that copy like an integer replace those copies, and the engine's
    // per-symbol maps key on `symbol.id()` instead of hashing the string.
    SymbolName symbol;
    double bid_price{};
    uint64_t bid_quantity{};
    double ask_price{};
    uint64_t ask_quantity{};
    uint64_t sequence{};
    std::string exchange;
};

struct DepthLevel
{
    double price{};
    uint64_t quantity{};
};

struct MarketDepthSnapshot
{
    uint64_t ts{};
    std::string symbol;
    uint64_t sequence{};
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
    std::string exchange;
};

struct IncrementalBookUpdate
{
    uint64_t exchange_ts{};
    uint64_t local_ts{};
    bool is_snapshot{false};
    Side side{Side::Unknown};
    double price{};
    uint64_t amount{};
};

/*
-------------------------------------------
 IncrementalUpdates
-------------------------------------------

 Storage for the updates of one incremental batch.

 A real incremental message carries one or two price level changes, so the first few updates are
 kept inside the event object and the batch needs no heap allocation at all. A batch that grows
 past the inline capacity moves its contents to a vector and stays there. On the Deribit
 incremental replay this removes one allocation and one free per batch, which measured at 161 ns
 per batch, about a quarter of the reader stage.

 begin() and end() always expose a single contiguous range, so range-for, std::any_of and
 operator[] behave exactly as they did when the storage was a plain vector.
-------------------------------------------
*/
class IncrementalUpdates
{
   public:
    static constexpr std::size_t kInlineCapacity = 4;

    using value_type = IncrementalBookUpdate;
    using iterator = IncrementalBookUpdate*;
    using const_iterator = const IncrementalBookUpdate*;

    IncrementalUpdates() = default;

    // Keeps brace initialisation working for callers that build a batch literal.
    IncrementalUpdates(std::initializer_list<IncrementalBookUpdate> updates)
    {
        for (const auto& update : updates) push_back(update);
    }

    std::size_t size() const
    {
        return overflow_.empty() ? count_ : overflow_.size();
    }

    bool empty() const
    {
        return size() == 0;
    }

    const_iterator begin() const
    {
        return data();
    }

    const_iterator end() const
    {
        return data() + size();
    }

    iterator begin()
    {
        return data();
    }

    iterator end()
    {
        return data() + size();
    }

    const IncrementalBookUpdate& operator[](std::size_t index) const
    {
        return data()[index];
    }

    IncrementalBookUpdate& operator[](std::size_t index)
    {
        return data()[index];
    }

    // A reservation within the inline capacity costs nothing.
    void reserve(std::size_t count)
    {
        if (count > kInlineCapacity) overflow_.reserve(count);
    }

    void clear()
    {
        count_ = 0;
        overflow_.clear();
    }

    void push_back(const IncrementalBookUpdate& update)
    {
        if (overflow_.empty())
        {
            if (count_ < kInlineCapacity)
            {
                inline_[count_++] = update;
                return;
            }
            spill_to_overflow();
        }
        overflow_.push_back(update);
    }

   private:
    IncrementalBookUpdate* data()
    {
        return overflow_.empty() ? inline_.data() : overflow_.data();
    }

    const IncrementalBookUpdate* data() const
    {
        return overflow_.empty() ? inline_.data() : overflow_.data();
    }

    void spill_to_overflow()
    {
        overflow_.reserve(kInlineCapacity * 2);
        overflow_.assign(inline_.begin(),
                         inline_.begin() + static_cast<std::ptrdiff_t>(count_));
        count_ = 0;
    }

    std::array<IncrementalBookUpdate, kInlineCapacity> inline_{};
    std::vector<IncrementalBookUpdate> overflow_;
    std::size_t count_{0};
};

struct IncrementalBookBatch
{
    uint64_t ts{};
    uint64_t exchange_ts{};
    uint64_t local_ts{};
    std::string symbol;
    IncrementalUpdates updates;
    std::string exchange;
    bool snapshot_metadata_valid{false};
    bool contains_snapshot{false};

    bool has_snapshot() const
    {
        if (snapshot_metadata_valid) return contains_snapshot;
        return std::any_of(updates.begin(), updates.end(),
                           [](const auto& update) { return update.is_snapshot; });
    }
};

using MarketEvent =
    std::variant<MarketTrade, BboQuote, MarketDepthSnapshot, IncrementalBookBatch>;

// The event's place on the venue's clock. A batch is stamped with the exchange's own microsecond and a book
// snapshot or a trade with its own, so every alternative answers the same question and a stream of different
// kinds can be ordered. It lives here rather than beside one caller because two of them need it: the replay
// feed merge, and the merge that turns several instruments' streams into one ordered stream.
inline uint64_t market_event_timestamp(const MarketEvent& event)
{
    return std::visit(
        [](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, IncrementalBookBatch>)
                return value.exchange_ts;
            else
                return value.ts;
        },
        event);
}

// The instrument the event is about, which is what a run carrying several books dispatches on. Every
// alternative carries one, including a batch: its updates do not, because one batch is one instrument.
// A `BboQuote` carries the name as an interned handle, whose `str()` is a reference into the intern table,
// so the reference this returns outlives the call in every case.
inline const std::string& market_event_symbol(const MarketEvent& event)
{
    return std::visit(
        [](const auto& value) -> const std::string&
        {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, BboQuote>)
                return value.symbol.str();
            else
                return value.symbol;
        },
        event);
}
