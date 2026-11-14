#include "lines/venue_order_translator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace
{
std::vector<std::string_view> split_fields(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size())
    {
        const std::size_t space = line.find(' ', start);
        if (space == std::string_view::npos)
        {
            if (start < line.size()) fields.push_back(line.substr(start));
            break;
        }
        if (space > start) fields.push_back(line.substr(start, space - start));
        start = space + 1;
    }
    return fields;
}

bool all_digits(std::string_view text)
{
    if (text.empty()) return false;
    for (const char c : text)
        if (c < '0' || c > '9') return false;
    return true;
}

double positive(std::string_view text, const char* what)
{
    std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0')
        throw OrderError(std::string(what) + " '" + copy + "' is not a number");
    if (!(value > 0.0)) throw OrderError(std::string(what) + " '" + copy + "' is not positive");
    return value;
}
}  // namespace

std::uint64_t OrderTranslator::units_of(double contracts) const
{
    // The venue's contracts as engine units -- the market decoder's amount read the other way.
    return static_cast<std::uint64_t>(
        std::llround(contracts * mapping_.contract_size * static_cast<double>(mapping_.quantity_scale)));
}

std::uint64_t OrderTranslator::contracts_for(std::uint64_t units) const
{
    // Rounded down rather than to nearest: a size above what the strategy asked for is the direction that turns a
    // demonstration into a position. A size that rounds to nothing is refused here, where both numbers can be named.
    const double per_contract = mapping_.contract_size * static_cast<double>(mapping_.quantity_scale);
    auto contracts = static_cast<std::int64_t>(std::floor(static_cast<double>(units) / per_contract));
    if (mapping_.lot_size > 1) contracts -= contracts % mapping_.lot_size;
    if (contracts <= 0)
    {
        std::ostringstream message;
        message << "quantity " << units << " engine units is "
                << (static_cast<double>(units) / static_cast<double>(mapping_.quantity_scale))
                << " of the coin, below one contract of " << mapping_.contract_size << " (" << per_contract
                << " units), so there is nothing to place";
        throw OrderError(message.str());
    }
    return static_cast<std::uint64_t>(contracts);
}

std::string OrderTranslator::price_on_tick(std::string_view text) const
{
    // The engine's double can carry digits the venue's grid does not have, and a price one step off it is refused
    // rather than rounded by the venue. Nearest step, which is the smallest change the venue can represent.
    const double step = mapping_.tick_size;
    const double price = positive(text, "price");
    const double steps = std::round(price / step);
    int decimals = 0;
    if (step < 1.0) decimals = std::max(0, -static_cast<int>(std::floor(std::log10(step))));
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(decimals);
    out << steps * step;
    return out.str();
}

std::string OrderTranslator::encode(std::string_view line)
{
    const std::vector<std::string_view> fields = split_fields(line);
    if (fields.empty()) throw OrderError("the line is empty");

    if (fields[0] == "ORDER")
    {
        if (fields.size() != 7)
            throw OrderError("an order line has seven fields and this one has " +
                             std::to_string(fields.size()));
        const std::string_view cl_ord_id = fields[1];
        const std::string_view side = fields[3];
        const std::string_view quantity = fields[5];
        const std::string_view tif = fields[6];
        if (!all_digits(cl_ord_id))
            throw OrderError("the order id '" + std::string(cl_ord_id) + "' is not a number");
        if (side != "B" && side != "S")
            throw OrderError("side '" + std::string(side) + "' is not B or S");
        const double price = positive(fields[4], "price");
        (void)price;
        if (!all_digits(quantity) || std::strtoull(std::string(quantity).c_str(), nullptr, 10) == 0)
            throw OrderError("quantity '" + std::string(quantity) + "' is not a positive integer");
        if (tif != "Gtc" && tif != "Ioc" && tif != "Fok")
            throw OrderError("time in force '" + std::string(tif) + "' is not Gtc, Ioc or Fok");

        const auto units = static_cast<std::uint64_t>(std::strtoull(std::string(quantity).c_str(), nullptr, 10));
        const std::uint64_t contracts = contracts_for(units);
        orders_[std::string(cl_ord_id)] =
            Known{.symbol = std::string(fields[2]), .units = units, .contracts = contracts};
        // OKX's shape for a limit order, with both numbers on the venue's grid. The instrument is the relay's, not the
        // engine's: the replay runs on whatever its book came from, and the venue only knows its own name for the thing.
        std::ostringstream out;
        out << R"({"id":")" << cl_ord_id << R"(","op":"order","args":[{"instId":")" << instrument_
            << R"(","tdMode":"cross","side":")" << (side == "B" ? "buy" : "sell")
            << R"(","ordType":"limit","px":")" << price_on_tick(fields[4]) << R"(","sz":")" << contracts
            << R"(","clOrdId":")" << cl_ord_id << R"(")";
        if (inst_code_ >= 0) out << R"(,"instIdCode":)" << inst_code_;
        out << "}]}";
        return out.str();
    }

    if (fields[0] == "CANCEL")
    {
        if (fields.size() != 2)
            throw OrderError("a cancel line has two fields and this one has " +
                             std::to_string(fields.size()));
        const std::string_view cl_ord_id = fields[1];
        if (!all_digits(cl_ord_id))
            throw OrderError("the order id '" + std::string(cl_ord_id) + "' is not a number");
        std::ostringstream out;
        out << R"({"id":")" << cl_ord_id << R"(","op":"cancel-order","args":[{"instId":")" << instrument_
            << R"(","clOrdId":")" << cl_ord_id << R"(")";
        if (inst_code_ >= 0) out << R"(,"instIdCode":)" << inst_code_;
        out << "}]}";
        return out.str();
    }

    throw OrderError("'" + std::string(fields[0]) + "' is not an order or a cancel");
}

std::vector<std::string> OrderTranslator::decode(const Json& message)
{
    if (message.type != Json::Type::Object) throw OrderError("a venue message is not an object");
    const Json* op = message.find("op");
    const std::string operation = op != nullptr ? op->as_text() : std::string();

    // A request the venue refused answers at the top level with an empty `data` list, so a decoder that only read the
    // entries would turn "parameter id can not be empty" into a silence the engine reads as a venue that ignored it.
    if (operation == "order" || operation == "cancel-order")
    {
        const Json* code = message.find("code");
        const std::string code_text = code != nullptr ? code->as_text() : std::string("0");
        if (code_text != "0")
        {
            const Json* id = message.find("id");
            const std::string order_id = id != nullptr ? id->as_text() : std::string();
            if (!all_digits(order_id))
                throw OrderError("the venue refused a request without an id the engine knows");
            return {"REJECT " + order_id + " venue"};
        }
    }

    const Json* arg = message.find("arg");
    const Json* channel_field = arg != nullptr ? arg->find("channel") : nullptr;
    const std::string channel = channel_field != nullptr ? channel_field->as_text() : std::string();

    std::vector<std::string> lines;
    const Json* data = message.find("data");
    if (data == nullptr) return lines;
    for (const Json& entry : data->items)
    {
        const Json* id_field = entry.find("clOrdId");
        const std::string cl_ord_id = id_field != nullptr ? id_field->as_text() : std::string();
        if (!all_digits(cl_ord_id))
            throw OrderError("the venue's message carries no order id the engine knows");

        if (operation == "order")
        {
            const Json* s_code = entry.find("sCode");
            if (s_code != nullptr && s_code->as_text() != "0")
            {
                lines.push_back("REJECT " + cl_ord_id + " venue");
                continue;
            }
            Known& known = orders_[cl_ord_id];
            const Json* sz = entry.find("sz");
            const double placed = sz != nullptr ? sz->as_double() : static_cast<double>(known.contracts);
            known.contracts = static_cast<std::uint64_t>(std::llround(placed));
            // An acknowledgement says what is still working. The cumulative it carries is the venue's count, not one
            // the engine has booked -- an ACK is not a fill -- so it is not recorded as delivered: the `orders` push
            // that follows reports the same counter, and that is where the fill is booked, once.
            const Json* already = entry.find("accFillSz");
            const double filled = already != nullptr ? already->as_double() : 0.0;
            lines.push_back("ACK " + cl_ord_id + " " +
                            std::to_string(units_of(std::max(placed - filled, 0.0))));
            continue;
        }

        if (operation == "cancel-order")
        {
            // The acknowledgement of a cancel is the venue saying it will act; the state message that follows is the
            // order ending, and that is the one that becomes CANCELED.
            const Json* s_code = entry.find("sCode");
            if (s_code != nullptr && s_code->as_text() != "0")
                lines.push_back("REJECT " + cl_ord_id + " venue");
            continue;
        }

        if (channel != "orders")
            throw OrderError("the venue's message is not one this channel knows");

        // A stream of states, not of events: the venue re-sends an order's current state on every change and again
        // after a reconnect, and `accFillSz` is cumulative over the order's life. One fill per increment of it, so a
        // repeated state is not a second fill and a reconnect is not a second session.
        Known& known = orders_[cl_ord_id];
        const double previous = known.filled;
        const Json* reported = entry.find("accFillSz");
        double cumulative = previous;
        if (reported != nullptr && !(reported->is_string() && reported->text.empty()))
            cumulative = reported->as_double();
        // A tolerance rather than an exact compare: the venue formats these itself, and a re-sent "0.45" must read as
        // the repeat it is rather than as a count that went backwards.
        if (cumulative < previous - 1e-9)
            throw OrderError("the venue's fill count for order " + cl_ord_id + " went backwards");
        const double increment = cumulative - previous;
        const Json* fill_price = entry.find("fillPx");
        bool emitted = false;
        if (increment > 0.0 && fill_price != nullptr && !fill_price->as_text().empty())
        {
            std::string line = "FILL " + cl_ord_id + " " + fill_price->as_text() + " " +
                               std::to_string(units_of(increment));
            // The venue's own charge: `fillFee` is this fill's and `fee` the order's cumulative, both negative when
            // the venue takes money, and the engine books a cost as positive. A rebate then arrives negative.
            const Json* fee = entry.find("fillFee");
            if (fee == nullptr) fee = entry.find("fee");
            if (fee != nullptr && !(fee->is_string() && fee->text.empty()))
            {
                char cost[40];
                std::snprintf(cost, sizeof(cost), " %.10g", -fee->as_double());
                line += cost;
            }
            lines.push_back(line);
            known.filled = cumulative;
            emitted = true;
        }
        const Json* state = entry.find("state");
        const std::string state_text = state != nullptr ? state->as_text() : std::string();
        if (state_text == "canceled" && !known.terminal)
        {
            lines.push_back("CANCELED " + cl_ord_id + " user");
            known.terminal = true;
            emitted = true;
        }
        else if (state_text == "rejected" && !known.terminal)
        {
            lines.push_back("REJECT " + cl_ord_id + " venue");
            known.terminal = true;
            emitted = true;
        }
        else if (state_text == "filled")
        {
            known.terminal = true;
        }
        if (!emitted) ++fills_deduplicated_;
    }
    return lines;
}
