#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/*
-------------------------------------------
 SymbolName
-------------------------------------------

 The instrument's name, interned: a sixteen byte handle that copies like an integer.

 The L2 path used to copy and hash the name about six times per event -- once into the market view,
 once into the quote the pipeline builds from that view, twice inside the engine's cached quotes, and
 once per map lookup on each side of the hand-off. The name does not change during a run, so what
 travels between those copies should be an id.

 `id()` is assigned once per distinct name per process, so two handles compare as integers, and
 `str()` / `view()` hand back the name without a copy. The store is a `std::deque` of strings, whose
 elements keep their address for as long as the process lives, and it is deliberately never freed: a
 destructor of some other static could read a handle, and one leaked allocation per process is a
 smaller problem than an order of destruction to reason about.

 Interning takes a mutex, once per distinct name. That mutex is not on the hot path: the readers run
 on the feed thread while the strategy runs on another, so the table has to be shared safely, but a
 name that is already interned -- every read after the first -- never touches the lock, because
 `str()` only follows the pointer it was given.
-------------------------------------------
*/
class SymbolName
{
   public:
    // The empty name: id 0 and no string. A default-constructed event carries it, and the engine's maps use id 0 as
    // "no such symbol", which is why this case must not intern anything.
    SymbolName() = default;

    // Implicit on purpose. A literal, a `std::string` or a `string_view` standing where a symbol is expected is the
    // shape every call site already has, and requiring an explicit conversion would have turned a representation
    // change into an edit of every construction site. The cost is one hash the first time a name is seen.
    SymbolName(const char* name) : SymbolName(name == nullptr ? std::string_view{} : std::string_view{name}) {}
    SymbolName(const std::string& name) : SymbolName(std::string_view{name}) {}
    SymbolName(std::string_view name);

    // A lookup that does not intern, for the one caller that must not grow the table: the engine asking whether it has
    // seen a symbol it is about to refuse.
    static SymbolName find(std::string_view name);

    uint32_t id() const { return id_; }
    bool empty() const { return id_ == 0; }
    const std::string& str() const { return name_ == nullptr ? empty_name() : *name_; }
    std::string_view view() const { return str(); }

    friend bool operator==(const SymbolName& left, const SymbolName& right) = default;

   private:
    static const std::string& empty_name();

    uint32_t id_{0};
    // Points into the interned store, which outlives every other static; null for the empty name, so a default
    // constructed handle reads no store at all.
    const std::string* name_{nullptr};
};

