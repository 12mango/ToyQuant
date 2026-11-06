#pragma once

#include <cstdint>

#include "common/symbol.h"
#include "orderbook/top_of_book.h"

struct L2MarketView
{
    // An interned handle rather than a `std::string`: the book sets it once and the pipeline reads it on every event,
    // so what travels is sixteen bytes instead of a copy of the name.
    SymbolName symbol;
    uint64_t ts{0};
    uint64_t local_ts{0};
    TopOfBook top;
    double micro_price{0.0};
    double depth_imbalance{0.0};
    uint64_t bid_depth{0};
    uint64_t ask_depth{0};
};
