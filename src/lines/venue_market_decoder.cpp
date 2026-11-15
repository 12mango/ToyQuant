#include "lines/venue_market_decoder.h"

#include <cmath>

std::string RelayStats::to_log_string() const
{
    return "trades=" + std::to_string(trades) + " book_messages=" + std::to_string(book_messages) +
           " depth_rows=" + std::to_string(depth_rows) + " gaps=" + std::to_string(gaps) +
           " reconnects=" + std::to_string(reconnects) + " parse_errors=" + std::to_string(parse_errors) +
           " queue_high_water=" + std::to_string(queue_high_water) +
           " queue_waits=" + std::to_string(queue_waits) + " pings_sent=" + std::to_string(pings_sent) +
           " pongs_sent=" + std::to_string(pongs_sent) + " pongs_received=" + std::to_string(pongs_received);
}

namespace
{
int64_t json_int(const Json* value)
{
    return value == nullptr ? 0 : static_cast<int64_t>(std::llround(value->as_double()));
}
}  // namespace

VenueDecoder::VenueDecoder(const RelayConfig& config)
    : instrument_(config.instrument),
      contract_size_(config.contract_size),
      quantity_scale_(config.quantity_scale),
      max_levels_(config.levels)
{
}

void VenueDecoder::decode(std::string_view message, std::vector<std::string>& rows, RelayStats& stats)
{
    Json root;
    std::string error;
    JsonParser parser(message);
    if (!parser.parse(root, error))
    {
        ++stats.parse_errors;
        return;
    }
    const Json* arg = root.find("arg");
    if (arg == nullptr) return;  // an event or a pong, not data
    const Json* channel = arg->find("channel");
    const Json* inst = arg->find("instId");
    const std::string symbol = inst != nullptr ? inst->as_text() : instrument_;
    if (channel == nullptr) return;
    const std::string name = channel->as_text();
    if (name == "trades")
        decode_trades(root, symbol, rows, stats);
    else if (name == "books" || name == "books5" || name == "bbo-tbt")
        decode_books(root, symbol, rows, stats);
}

uint64_t VenueDecoder::amount_of(const Json* size) const
{
    const double value = size == nullptr ? 0.0 : size->as_double();
    return static_cast<uint64_t>(std::llround(value * contract_size_ * static_cast<double>(quantity_scale_)));
}

void VenueDecoder::decode_trades(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
                                 RelayStats& stats)
{
    const Json* data = root.find("data");
    if (data == nullptr || !data->is_array()) return;
    for (const Json& trade : data->items)
    {
        const Json* ts = trade.find("ts");
        if (ts == nullptr) continue;
        const Json* id = trade.find("tradeId");
        const Json* side = trade.find("side");
        const Json* price = trade.find("px");
        const Json* size = trade.find("sz");
        std::string row = "okx,";
        row += symbol;
        row += ',';
        row += ts->as_text();
        row += ',';
        row += id != nullptr ? id->as_text() : std::string();
        row += ',';
        row += side != nullptr ? side->as_text() : std::string();
        row += ',';
        row += price != nullptr ? price->as_text() : std::string();
        row += ',';
        row += std::to_string(amount_of(size));
        row += '\n';
        rows.push_back(std::move(row));
        ++stats.trades;
    }
}

void VenueDecoder::decode_books(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
                                RelayStats& stats)
{
    const Json* action = root.find("action");
    const bool snapshot = action == nullptr || action->as_text() == "snapshot";
    const Json* data = root.find("data");
    if (data == nullptr || !data->is_array()) return;
    for (const Json& book : data->items)
    {
        const int64_t seq = json_int(book.find("seqId"));
        const int64_t previous = json_int(book.find("prevSeqId"));
        if (snapshot)
        {
            has_last_seq_ = true;
            last_seq_ = seq;
        }
        else
        {
            // A break between the venue's `prevSeqId` and the `seqId` we last saw is a lost update;
            // counting it is what "recorded for hours without a drop" is measured on.
            if (has_last_seq_ && previous != last_seq_) ++stats.gaps;
            has_last_seq_ = true;
            last_seq_ = seq;
        }
        const Json* ts = book.find("ts");
        const std::string stamp = ts != nullptr ? ts->as_text() : std::string("0");
        const Json* sides[2] = {book.find("bids"), book.find("asks")};
        const char* names[2] = {"bid", "ask"};
        for (int side = 0; side < 2; ++side)
        {
            const Json* levels = sides[side];
            if (levels == nullptr || !levels->is_array()) continue;
            std::size_t count = levels->items.size();
            if (snapshot && max_levels_ > 0 && count > max_levels_) count = max_levels_;
            for (std::size_t index = 0; index < count; ++index)
            {
                const Json& level = levels->items[index];
                if (level.items.size() < 2) continue;
                std::string row = "okx,";
                row += symbol;
                row += ',';
                row += stamp;
                row += ',';
                row += stamp;
                row += snapshot ? ",1," : ",0,";
                row += names[side];
                row += ',';
                row += level.items[0].as_text();
                row += ',';
                row += std::to_string(amount_of(&level.items[1]));
                row += '\n';
                rows.push_back(std::move(row));
                ++stats.depth_rows;
            }
        }
        ++stats.book_messages;
    }
}
