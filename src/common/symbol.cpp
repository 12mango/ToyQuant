#include "common/symbol.h"

#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
// One table per process. `names` holds the strings and hands out stable addresses; `by_name` indexes them by the
// contents of a view, so a lookup can be made from a `string_view` that points anywhere.
//
// It lives here rather than in the header on purpose: a table needs a mutex, a deque and a hash map, and the handle is
// included by nearly every translation unit in the program through `common/market_event.h`. Keeping those three
// headers out of that one is what stops a representation change from moving the layout of code that does not use it.
struct Store
{
    std::mutex mutex;
    std::deque<std::string> names;
    std::unordered_map<std::string_view, SymbolName> by_name;
    std::uint32_t next_id{1};
};

Store& store()
{
    // Deliberately leaked, so that no static destructor can run before a handle is read.
    static Store* instance = new Store();
    return *instance;
}
}  // namespace

const std::string& SymbolName::empty_name()
{
    static const std::string empty;
    return empty;
}

SymbolName::SymbolName(std::string_view name)
{
    if (name.empty()) return;
    Store& table = store();
    const std::lock_guard<std::mutex> lock(table.mutex);
    if (const auto found = table.by_name.find(name); found != table.by_name.end())
    {
        id_ = found->second.id_;
        name_ = found->second.name_;
        return;
    }
    // A deque never moves an element it already holds, so the pointer below stays valid while later names are added.
    table.names.emplace_back(name);
    name_ = &table.names.back();
    id_ = table.next_id++;
    table.by_name.emplace(std::string_view{*name_}, *this);
}

SymbolName SymbolName::find(std::string_view name)
{
    if (name.empty()) return {};
    Store& table = store();
    const std::lock_guard<std::mutex> lock(table.mutex);
    const auto found = table.by_name.find(name);
    return found == table.by_name.end() ? SymbolName{} : found->second;
}
