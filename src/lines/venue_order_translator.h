#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lines/venue_json.h"

// The order leg's one translation each way: the engine's `ORDER`/`CANCEL` lines become the venue's request object,
// and the venue's replies become the `ACK`/`FILL`/`CANCELED`/`REJECT` lines the adapter decodes. Both directions run
// on the same object because the state they share is the order's: a cancel needs the instrument an order was placed
// on, and the `orders` channel repeats a cumulative fill count that has to be booked once.
//
// This is the second slice of ADR 0006. The rules are the ones `tools/live/venue_relay.py` states and its self-test
// pins, ported rather than reinvented: a size below one contract is refused here, where the reason can name both
// numbers, rather than at the venue as a rejection; a price is rounded onto the venue's tick because one step off its
// grid is refused rather than corrected; and the venue's sizes are decimal strings, so a contract that fills in parts
// must not be read with an integer type.
class OrderError : public std::runtime_error
{
   public:
    using std::runtime_error::runtime_error;
};

// The three venue facts and the engine's scale that turn one into the other: one contract is `contract_size` of the
// coin, `quantity_scale` engine units are one of the coin, a price sits on `tick_size` and a size on `lot_size`.
struct OrderMapping
{
    double contract_size{0.01};
    std::uint64_t quantity_scale{1000000};
    double tick_size{0.1};
    std::int64_t lot_size{1};
};

class OrderTranslator
{
   public:
    // `inst_code` is the venue's internal code for the instrument (-1 when it is not sent). It is an operator's
    // reading, not something derived here: a wrong guess would be an order on the wrong contract.
    explicit OrderTranslator(std::string instrument, std::int64_t inst_code = -1, OrderMapping mapping = {})
        : instrument_(std::move(instrument)), inst_code_(inst_code), mapping_(mapping)
    {
    }

    // The engine's line as the JSON text the venue's `op: order` / `op: cancel-order` carries. Throws `OrderError`
    // naming the field it refused.
    std::string encode(std::string_view line);

    // The venue's message as the adapter's lines. Throws `OrderError` for a message it cannot attribute to an order.
    std::vector<std::string> decode(const Json& message);

    std::uint64_t units_of(double contracts) const;
    std::uint64_t contracts_for(std::uint64_t units) const;
    std::string price_on_tick(std::string_view text) const;

    // Messages that repeated a fill the engine had already been told about. A reconnect re-sends every live order's
    // state, so a session with a drop that reports zero here is the surprising case.
    std::uint64_t fills_deduplicated() const { return fills_deduplicated_; }
    const std::string& instrument() const { return instrument_; }

   private:
    struct Known
    {
        std::string symbol;
        std::uint64_t units{0};
        std::uint64_t contracts{0};
        double filled{0.0};
        bool terminal{false};
    };

    std::string instrument_;
    std::int64_t inst_code_{-1};
    OrderMapping mapping_;
    std::unordered_map<std::string, Known> orders_;
    std::uint64_t fills_deduplicated_{0};
};
