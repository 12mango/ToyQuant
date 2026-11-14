#include "lines/live_venue.h"

#include <charconv>
#include <sstream>
#include <vector>

namespace
{
// The frames are space separated and small; a split on one space is the whole parser. Kept here rather than
// in a shared helper because nothing else in the tree speaks this protocol.
std::vector<std::string_view> fields_of(std::string_view frame)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= frame.size())
    {
        const std::size_t space = frame.find(' ', start);
        if (space == std::string_view::npos)
        {
            fields.push_back(frame.substr(start));
            break;
        }
        fields.push_back(frame.substr(start, space - start));
        start = space + 1;
    }
    return fields;
}

bool parse_u64(std::string_view text, uint64_t& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_price(std::string_view text, double& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

const char* side_token(exchange::Side side)
{
    return side == exchange::Side::Buy ? "B" : "S";
}

const char* tif_token(TimeInForce tif)
{
    switch (tif)
    {
        case TimeInForce::Ioc:
            return "Ioc";
        case TimeInForce::Fok:
            return "Fok";
        case TimeInForce::Gtc:
            break;
    }
    return "Gtc";
}

CancelReason cancel_reason_of(std::string_view text)
{
    if (text == "user") return CancelReason::User;
    if (text == "ioc") return CancelReason::Ioc;
    if (text == "fok") return CancelReason::Fok;
    if (text == "self_trade") return CancelReason::SelfTrade;
    if (text == "timeout") return CancelReason::Timeout;
    if (text == "post_only") return CancelReason::PostOnly;
    return CancelReason::Venue;
}
}  // namespace

void LiveVenue::send_order(const exchange::Order& order)
{
    // The adapter's own bound, checked before anything is written. The pipeline's caps are about what the
    // strategy may ask for; this one is about what this adapter is willing to put on a real wire.
    if (config_.max_orders != 0 && counters_.orders_sent >= config_.max_orders)
    {
        ++counters_.refused_by_adapter;
        return;
    }
    if (config_.max_order_quantity != 0 && order.qty > config_.max_order_quantity)
    {
        ++counters_.refused_by_adapter;
        return;
    }
    // Priced with the accounting's own helper rather than `price * qty`: the engine's quantity is in scaled
    // units and a contract can carry a face value, so the product would be off by `quantity_scale` on one
    // instrument and by the multiplier on the next.
    if (config_.max_order_notional_usd > 0.0 &&
        fill_notional_usd(config_.unit_notional_usd, config_.multiplier, order.price, order.qty,
                          config_.quantity_scale) > config_.max_order_notional_usd)
    {
        ++counters_.refused_by_adapter;
        return;
    }

    std::ostringstream frame;
    frame << "ORDER " << order.id << ' ' << order.symbol << ' ' << side_token(order.side) << ' '
          << order.price << ' ' << order.qty << ' ' << tif_token(order.time_in_force);
    if (!transport_.send(frame.str()))
    {
        ++counters_.transport_failures;
        ++counters_.refused_by_adapter;
        return;
    }
    ++counters_.orders_sent;
    // Kept until the order ends, because the venue's frames carry the id and nothing else.
    sent_[order.id] = SentOrder{order.owner, order.symbol, order.side, order.time_in_force};
}

void LiveVenue::cancel_order(uint64_t order_id)
{
    std::string frame = "CANCEL " + std::to_string(order_id);
    if (!transport_.send(frame))
    {
        ++counters_.transport_failures;
        return;
    }
    ++counters_.cancels_sent;
}

bool LiveVenue::on_frame(std::string_view frame)
{
    const std::vector<std::string_view> fields = fields_of(frame);
    if (fields.size() < 2)
    {
        ++counters_.frames_dropped;
        return false;
    }

    ExecutionReport report;
    uint64_t order_id = 0;
    if (!parse_u64(fields[1], order_id))
    {
        ++counters_.frames_dropped;
        return false;
    }
    report.order_id = order_id;
    // Owner, side, the symbol and the role are the sending order's, not the frame's: the pipeline books a fill
    // against its own owner, in the side the order was for, under the symbol the position is keyed by, and counts
    // maker and taker fills separately -- and the report's defaults are none of those. A missing symbol is not a
    // cosmetic gap: the position lands under the empty string, no mid is ever found for it, and the account
    // reports its cash as equity while the position is worth nothing -- which is what a live run did.
    const auto sent = sent_.find(order_id);
    if (sent != sent_.end())
    {
        report.owner = sent->second.owner;
        report.side = sent->second.side;
        report.symbol = sent->second.symbol;
    }

    const std::string_view kind = fields[0];
    if (kind == "ACK" && fields.size() >= 3)
    {
        uint64_t remaining = 0;
        if (!parse_u64(fields[2], remaining))
        {
            ++counters_.frames_dropped;
            return false;
        }
        report.exec_type = ExecType::Resting;
        report.quantity = remaining;
    }
    else if (kind == "FILL" && fields.size() >= 4)
    {
        double price = 0.0;
        uint64_t quantity = 0;
        if (!parse_price(fields[2], price) || !parse_u64(fields[3], quantity))
        {
            ++counters_.frames_dropped;
            return false;
        }
        report.exec_type = ExecType::Trade;
        report.price = price;
        report.quantity = quantity;
        // The venue's own fee, when the frame carries one: the fifth field is the cost the venue booked, in the
        // quote currency, positive for a charge and negative for a rebate. It is the venue's number rather than a
        // rate this adapter applies, because the schedule is the venue's and the invoice is right there. A frame
        // with four fields keeps zero, which is what the simulated venue charges.
        if (fields.size() >= 5 && !parse_price(fields[4], report.fee))
        {
            ++counters_.frames_dropped;
            return false;
        }
        // A Gtc order rests, so a venue filling it is filling a maker; an Ioc or Fok order is a taker by
        // definition. The frame carries no role, so this is the adapter's rule rather than the venue's word.
        report.liquidity_role = sent != sent_.end() && sent->second.time_in_force != TimeInForce::Gtc
                                    ? LiquidityRole::Taker
                                    : LiquidityRole::Maker;
    }
    else if (kind == "CANCELED")
    {
        report.exec_type = ExecType::Cancelled;
        report.cancel_reason = fields.size() >= 3 ? cancel_reason_of(fields[2]) : CancelReason::Venue;
    }
    else if (kind == "REJECT")
    {
        report.exec_type = ExecType::Rejected;
        report.reject_reason = RejectReason::Venue;
    }
    else
    {
        ++counters_.frames_dropped;
        return false;
    }

    // An order that has ended will not be reported again, so its owner is no longer needed; a fill keeps it,
    // because an order can fill more than once.
    if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Rejected)
        sent_.erase(order_id);
    // The frame carries no time, so the report is stamped here with the clock the run is on. Without it a
    // recorded live session's answers are all due at zero: `--replay-orders` then releases every one of them
    // at the first event, and a session that had an answer between two quotes replays as one that did not --
    // measured as a replay that diverges at the third order through a venue that refused every one.
    report.ts = report_clock_ ? report_clock_() : 0;
    ++counters_.reports_decoded;
    if (callback_) callback_(report);
    return true;
}
