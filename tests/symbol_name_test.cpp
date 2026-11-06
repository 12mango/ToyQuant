#include "common/symbol.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
void same_name_is_the_same_handle()
{
    const SymbolName first{"BTC-PERPETUAL"};
    const SymbolName second{std::string{"BTC-PERPETUAL"}};
    const SymbolName third{std::string_view{"BTC-PERPETUAL"}};
    assert(!first.empty());
    assert(first == second && second == third);
    assert(first.id() == second.id() && second.id() == third.id());
    // The name itself is not copied into the handle: both read the interned string, which is one object.
    assert(&first.str() == &second.str());
    assert(first.str() == "BTC-PERPETUAL");
    assert(first.view() == "BTC-PERPETUAL");
}

void different_names_are_different_handles()
{
    const SymbolName btc{"BTC-PERPETUAL"};
    const SymbolName eth{"ETH-PERPETUAL"};
    assert(btc != eth);
    assert(btc.id() != eth.id());
    // A prefix is not a symbol: the table keys on the whole name.
    const SymbolName prefix{"BTC"};
    assert(prefix != btc && prefix.id() != btc.id());
}

void the_empty_name_is_id_zero()
{
    const SymbolName empty;
    assert(empty.empty());
    assert(empty.id() == 0);
    assert(empty.str().empty());
    // An empty handle never interns anything, which is what lets a default-constructed event be free.
    const SymbolName also_empty{""};
    assert(also_empty.empty() && also_empty.id() == 0);
    assert(also_empty == empty);
}

void the_pointer_survives_later_names()
{
    // This is the invariant the store is built on: a deque hands out addresses that stay valid while new names are
    // appended, so a handle taken before a thousand others were interned still points at its own name.
    const SymbolName early{"SURVIVOR"};
    const std::string* address = &early.str();
    for (int index = 0; index < 1000; ++index) (void)SymbolName{"FILLER-" + std::to_string(index)};
    assert(&early.str() == address);
    assert(early.str() == "SURVIVOR");
    assert(SymbolName{"SURVIVOR"} == early);
}

void find_does_not_intern()
{
    const SymbolName fresh{"NEVER-SEEN-BEFORE"};
    assert(fresh.id() != 0);
    // The name is in the table now, so a lookup finds it...
    assert(SymbolName::find("NEVER-SEEN-BEFORE") == fresh);
    // ...and a name that is not there comes back empty rather than being added.
    const SymbolName missing = SymbolName::find("NOT-IN-THE-TABLE");
    assert(missing.empty());
    assert(SymbolName::find("NOT-IN-THE-TABLE").empty());
}

void two_threads_agree_on_the_id()
{
    // The readers run on the feed thread and the strategy on another, so the table is reached from both. The same name
    // interned concurrently must come out one id, which is the property the whole representation rests on.
    std::vector<std::uint32_t> ids(8, 0);
    std::vector<std::thread> threads;
    threads.reserve(ids.size());
    for (auto& id : ids) threads.emplace_back([&id] { id = SymbolName{"CONCURRENT"}.id(); });
    for (auto& thread : threads) thread.join();
    for (const std::uint32_t id : ids)
    {
        assert(id != 0);
        assert(id == ids.front());
    }
}
}  // namespace

int main()
{
    same_name_is_the_same_handle();
    different_names_are_different_handles();
    the_empty_name_is_id_zero();
    the_pointer_survives_later_names();
    find_does_not_intern();
    two_threads_agree_on_the_id();
    std::cout << "symbol_name_test OK\n";
    return 0;
}
