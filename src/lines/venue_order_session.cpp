#include "lines/venue_order_session.h"

#include <sstream>

#include "lines/venue_auth.h"
#include "lines/venue_json.h"

std::string OrderSession::Counters::to_log_string() const
{
    return "logins=" + std::to_string(logins) + " subscribes=" + std::to_string(subscribes) +
           " reconnects=" + std::to_string(reconnects) + " messages=" + std::to_string(messages) +
           " other_instruments=" + std::to_string(other_instruments) +
           " unreadable=" + std::to_string(unreadable);
}

OrderSession::OrderSession(std::string instrument, Credentials credentials, std::int64_t inst_code,
                           OrderMapping mapping, Sink& sink)
    : instrument_(std::move(instrument)),
      credentials_(std::move(credentials)),
      translator_(instrument_, inst_code, mapping),
      sink_(sink)
{
}

void OrderSession::on_connected(std::uint64_t now_seconds)
{
    // The login is the first frame on every connection, including a reconnection: the venue's session is its own, and
    // a socket that comes back is a client it has never seen. The timestamp is the one the signature is over.
    const std::string timestamp = std::to_string(now_seconds);
    const std::string sign = venue_auth::login_signature(timestamp, credentials_.secret);
    std::ostringstream frame;
    frame << R"({"op":"login","args":[{"apiKey":")" << credentials_.api_key << R"(","passphrase":")"
          << credentials_.passphrase << R"(","timestamp":")" << timestamp << R"(","sign":")" << sign
          << R"("}]})";
    ++counters_.logins;
    last_ping_seconds_ = now_seconds;
    sink_.to_venue(frame.str());
}

void OrderSession::subscribe()
{
    // The venue takes an instrument *type* on this channel, not a symbol, so one subscription covers the account and
    // the filter is on this side -- which is what `other_instruments` counts.
    ++counters_.subscribes;
    sink_.to_venue(R"({"op":"subscribe","args":[{"channel":"orders","instType":"ANY"}]})");
}

void OrderSession::on_frame(std::string_view frame)
{
    ++counters_.messages;
    Json message;
    std::string error;
    JsonParser parser(frame);
    if (!parser.parse(message, error))
    {
        ++counters_.unreadable;
        return;
    }

    const Json* event = message.find("event");
    if (event != nullptr && event->as_text() == "login")
    {
        // The venue answers the login before anything else is legal on the socket, so the subscription goes out here
        // rather than on a timer: a subscribe sent earlier is refused.
        subscribe();
        return;
    }
    if (event != nullptr && event->as_text() == "error")
    {
        ++counters_.unreadable;
        return;
    }

    const Json* arg = message.find("arg");
    const Json* channel = arg != nullptr ? arg->find("channel") : nullptr;
    if (channel != nullptr && channel->as_text() == "orders")
    {
        // The filter is here rather than in the translator: the translator speaks the venue's shapes and does not know
        // which instrument this relay is for, while an account's channel carries every instrument it trades.
        const Json* data = message.find("data");
        if (data != nullptr)
        {
            for (const Json& entry : data->items)
            {
                const Json* instrument = entry.find("instId");
                if (instrument != nullptr && instrument->as_text() != instrument_)
                {
                    ++counters_.other_instruments;
                    continue;
                }
                for (const std::string& line : translator_.decode_orders_entry(entry)) sink_.to_engine(line);
            }
        }
        return;
    }
    ++counters_.unreadable;
}

void OrderSession::on_engine_line(std::string_view line)
{
    sink_.to_venue(translator_.encode(line));
}

bool OrderSession::on_tick(std::uint64_t now_seconds)
{
    // A ping every twenty seconds, whether or not data flowed: the venue answers it, which is what makes the
    // keepalive a round trip rather than a write into a socket that has already gone.
    if (now_seconds < last_ping_seconds_ + 20) return false;
    last_ping_seconds_ = now_seconds;
    sink_.to_venue(R"({"op":"ping"})");
    return true;
}
