#include "lines/venue_order_wire.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "lines/venue_ws.h"

namespace venue_order_wire
{
namespace
{
// The id in `ORDER <id> ...` or `CANCEL <id> ...`: the token a refusal has to name. Anything unexpected becomes `0`
// rather than a token this side invented, because the engine answers an unknown id with nothing and a wrong one with a
// report about another order.
std::string token_of(std::string_view line)
{
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) return "0";
    std::size_t end = line.find(' ', space + 1);
    if (end == std::string_view::npos) end = line.size();
    const std::string_view token = line.substr(space + 1, end - space - 1);
    if (token.empty()) return "0";
    for (const char c : token)
        if (c < '0' || c > '9') return "0";
    return std::string(token);
}

// Points at the connection's current stream without owning it, so a session that throws on the way out clears the
// pointer as it unwinds rather than leaving a frame to be written into a closed socket.
class CurrentStream
{
   public:
    CurrentStream(std::mutex& mutex, venue_ws::Stream*& slot, venue_ws::Stream& stream)
        : mutex_(mutex), slot_(slot)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot_ = &stream;
    }
    ~CurrentStream()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot_ = nullptr;
    }
    CurrentStream(const CurrentStream&) = delete;
    CurrentStream& operator=(const CurrentStream&) = delete;

   private:
    std::mutex& mutex_;
    venue_ws::Stream*& slot_;
};
}  // namespace

std::string WireCounters::to_log_string() const
{
    return "orders=" + std::to_string(orders.load()) + " cancels=" + std::to_string(cancels.load()) +
           " replies=" + std::to_string(replies.load()) + " refused=" + std::to_string(refused.load()) +
           " write_failures=" + std::to_string(write_failures.load()) +
           " sessions=" + std::to_string(sessions.load());
}

OrderSession::Credentials credentials_from_environment()
{
    const auto read = [](const char* name) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            throw OrderError(std::string(name) +
                             " is not set: the private channel reads its three credentials from the environment, "
                             "never from the command line, which reaches a run's mode line and its recordings");
        return std::string(value);
    };
    OrderSession::Credentials credentials;
    credentials.api_key = read("TOYQUANT_API_KEY");
    credentials.secret = read("TOYQUANT_API_SECRET");
    credentials.passphrase = read("TOYQUANT_PASSPHRASE");
    return credentials;
}

VenueOrderConnection::VenueOrderConnection(std::string instrument, OrderSession::Credentials credentials,
                                           WireConfig config, EnginePort& engine,
                                           std::function<std::uint64_t()> clock)
    : config_(std::move(config)),
      session_(std::move(instrument), std::move(credentials), config_.inst_code, config_.mapping, *this),
      engine_(engine),
      clock_(clock ? std::move(clock)
                   : std::function<std::uint64_t()>([] { return venue_ws::now_seconds(); }))
{
}

void VenueOrderConnection::run()
{
    double backoff = config_.backoff_seconds;
    while (!stop_.load())
    {
        up_.store(false);
        try
        {
            session_once();
            backoff = config_.backoff_seconds;
        }
        catch (const std::exception& error)
        {
            if (stop_.load()) break;
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                last_error_ = error.what();
            }
            std::cerr << "private channel lost (" << error.what() << "); reconnecting in " << backoff << "s"
                      << std::endl;
        }
        if (stop_.load()) break;
        if (up_.load())
        {
            // The venue forgot that socket, so the next connection is a new session: the login and the subscription go
            // out again, and the counter is what says the reconnect path ran rather than the first connection lasting.
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_.on_disconnected();
        }
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_.wait_for(lock, std::chrono::duration<double>(backoff), [this] { return stop_.load(); });
        backoff = std::min(backoff * 2.0, 8.0);
    }
}

void VenueOrderConnection::stop()
{
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
    }
    wait_.notify_all();
}

void VenueOrderConnection::session_once()
{
    const venue_ws::Url url = venue_ws::parse_url(config_.venue_url);
    venue_ws::Stream stream;
    stream.connect(url);
    venue_ws::client_handshake(stream, url);
    CurrentStream current(write_mutex_, stream_, stream);
    up_.store(true);
    ++counters_.sessions;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.on_connected(clock_());
    }
    {
        // What was submitted between sessions goes out now, after the login and in the order it was submitted. The
        // venue's protocol is sequential, so a frame sent after the login frame is legal even before its answer arrives.
        std::lock_guard<std::mutex> lock(write_mutex_);
        for (const Pending& frame : pending_) venue_ws::send_frame(stream, frame.opcode, frame.payload, true);
        pending_.clear();
    }

    std::string payload;
    int opcode = 0;
    bool fin = true;
    while (!stop_.load())
    {
        const int status = venue_ws::read_frame(stream, payload, opcode, fin);
        if (status == -2)
        {
            // Quiet, not dead: the read timeout is what lets the session's keepalive be due on its own clock, and
            // `on_tick` is the only thing that decides whether a ping was sent.
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_.on_tick(clock_());
            continue;
        }
        if (status == 0) throw std::runtime_error("the venue closed the private connection");
        if (status != 1) throw std::runtime_error("the private connection could not be read");
        if (opcode == 0x8) throw std::runtime_error("the venue closed the private connection");
        if (opcode == 0x9)
        {
            // A control ping is owed a control pong with its own payload; the session never sees it, because a
            // heartbeat is the connection's business and not an order's.
            send(payload, 0xA);
            continue;
        }
        if (opcode != 0x1 && opcode != 0x0) continue;
        // This venue's heartbeat is a text frame rather than a control frame, and neither form is an order message.
        if (payload == "ping")
        {
            send("pong");
            continue;
        }
        if (payload == "pong") continue;
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.on_frame(payload);
    }
}


void VenueOrderConnection::on_engine_line(std::string_view line)
{
    const bool is_order = line.rfind("ORDER", 0) == 0;
    std::lock_guard<std::mutex> lock(session_mutex_);
    try
    {
        session_.on_engine_line(line);
    }
    catch (const OrderError& error)
    {
        // The translator refused the line before anything crossed -- a size below one contract, a field it cannot map --
        // and the engine is owed an answer rather than silence: a submission nobody answered is a strategy that waits
        // out its own timeout, which is the failure the relay's `refused` counter exists to make visible.
        ++counters_.refused;
        engine_.write_line("REJECT " + token_of(line) + " venue");
        std::cerr << "refused '" << line << "': " << error.what() << std::endl;
        return;
    }
    if (is_order)
        ++counters_.orders;
    else
        ++counters_.cancels;
}

void VenueOrderConnection::to_venue(std::string_view frame) { send(frame); }

void VenueOrderConnection::to_engine(std::string_view line)
{
    if (!engine_.write_line(line)) return;  // the engine went away; the next write says so again
    ++counters_.replies;
}

bool VenueOrderConnection::send(std::string_view payload, int opcode)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stream_ == nullptr)
    {
        // No socket yet: the request waits for one rather than vanishing (see `Pending`). The queue is what makes an
        // order submitted in the first milliseconds of a session reach the venue instead of being counted as a failure
        // the engine cannot see -- the relay accepted the engine's connection before the venue's login came back.
        if (pending_.size() >= kMaxPending)
        {
            ++counters_.write_failures;
            return false;
        }
        pending_.push_back(Pending{std::string(payload), opcode});
        return true;
    }
    if (!venue_ws::send_frame(*stream_, opcode, payload, true))
    {
        ++counters_.write_failures;
        return false;
    }
    return true;
}

std::string VenueOrderConnection::last_error() const
{
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}
}  // namespace venue_order_wire

