#include "lines/venue_order_wire.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "lines/venue_auth.h"
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
    // The placement route's two averages are the counting measurement a kept-alive connection would have to beat; both
    // are summed across every call, including the ones whose answer never came back, because those paid for a handshake
    // too. Zero calls reports zero rather than a division by nothing.
    const std::uint64_t calls = rest_calls.load();
    const std::string connect_avg = std::to_string(calls == 0 ? 0 : rest_connect_ms.load() / calls);
    const std::string place_avg = std::to_string(calls == 0 ? 0 : rest_place_ms.load() / calls);
    return "orders=" + std::to_string(orders.load()) + " cancels=" + std::to_string(cancels.load()) +
           " replies=" + std::to_string(replies.load()) + " refused=" + std::to_string(refused.load()) +
           " write_failures=" + std::to_string(write_failures.load()) +
           " sessions=" + std::to_string(sessions.load()) + " placed=" + std::to_string(placed.load()) +
           " venue_refusals=" + std::to_string(venue_refusals.load()) +
           " transport_failures=" + std::to_string(transport_failures.load()) +
           " rest_calls=" + std::to_string(calls) + " rest_connect_ms_avg=" + connect_avg +
           " rest_place_ms_avg=" + place_avg + " rest_429=" + std::to_string(rest_429.load()) +
           " pending_queries=" + std::to_string(pending_queries.load()) +
           " equity_queries=" + std::to_string(equity_queries.load()) +
           " query_failures=" + std::to_string(query_failures.load());
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

namespace
{
// A REST timestamp: ISO 8601 with milliseconds, which is this venue's format for a *request* signature. The login's is
// Unix epoch seconds, and a client that shares one function across the two is one error code away from looking like a key
// with no permission -- measured rather than read off a page: `60004` for the one, `50112` for the other.
std::string rest_timestamp()
{
    using namespace std::chrono;
    const auto elapsed = system_clock::now().time_since_epoch();
    const auto whole = duration_cast<seconds>(elapsed);
    const auto millis = duration_cast<milliseconds>(elapsed) - duration_cast<milliseconds>(whole);
    const std::time_t stamp = static_cast<std::time_t>(whole.count());
    std::tm utc{};
    gmtime_r(&stamp, &utc);
    char text[96];  // wider than the format needs, because the compiler's bound on %04d is eleven characters
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(millis.count()));
    return text;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A chunked body, as the transfer encoding spells it: a hex length, the bytes, a CRLF, and `0\r\n\r\n` at the end.
std::string dechunk(std::string_view text)
{
    std::string out;
    std::size_t at = 0;
    while (at < text.size())
    {
        const std::size_t end = text.find("\r\n", at);
        if (end == std::string_view::npos) break;
        const std::size_t size = std::strtoul(std::string(text.substr(at, end - at)).c_str(), nullptr, 16);
        at = end + 2;
        if (size == 0) break;
        out.append(text.substr(at, std::min(size, text.size() - at)));
        at += size + 2;  // the chunk's own CRLF
    }
    return out;
}

bool answered(std::string_view answer)
{
    const std::size_t split = answer.find("\r\n\r\n");
    if (split == std::string_view::npos) return false;
    const std::string head = lower(answer.substr(0, split));
    const std::string_view body = answer.substr(split + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos)
        return body.find("\r\n0\r\n") != std::string_view::npos || body.rfind("0\r\n\r\n") == body.size() - 5;
    const std::size_t at = head.find("content-length:");
    if (at == std::string::npos) return false;  // no length: the venue's close is what ends it
    const std::size_t end = head.find("\r\n", at);
    const std::string number(head.substr(at + 15, end - at - 15));
    return body.size() >= std::stoull(number);
}

HttpResponse parse_answer(const std::string& answer)
{
    HttpResponse parsed;
    const std::size_t split = answer.find("\r\n\r\n");
    if (split == std::string::npos)
        throw std::runtime_error("the venue's answer has no headers: " + answer.substr(0, 80));
    const std::string head = lower(answer.substr(0, split));
    const std::string_view body(answer.data() + split + 4, answer.size() - split - 4);
    const std::size_t space = head.find(' ');
    parsed.status = space == std::string::npos ? 0 : std::atoi(head.c_str() + space + 1);
    parsed.body = head.find("transfer-encoding: chunked") != std::string::npos ? dechunk(body) : std::string(body);
    return parsed;
}

// One call on its own connection, which is what the venue's endpoint expects: the Python relay's placer makes one call
// per order too, and a kept-alive connection would need a pool this line has no reason to own yet -- `connect_ms` is the
// number that decides whether it should, filled with the time spent in connect plus the TLS handshake. `method` is POST
// for the order endpoints and GET for the two queries; a GET carries no body and the same `Content-Length: 0`, so the
// one reader below ends either answer on a header rather than on the venue closing the socket.
HttpResponse http_json(std::string_view method, const venue_ws::Url& url, std::string_view path,
                       std::string_view headers, std::string_view body, std::int64_t& connect_ms)
{
    venue_ws::Stream stream;
    const std::int64_t started = venue_ws::now_ms();
    stream.connect(url);
    connect_ms = venue_ws::now_ms() - started;
    std::string request;
    request += method;
    request += ' ';
    request += path;
    request += " HTTP/1.1\r\nHost: ";
    request += url.host;
    request += "\r\n";
    request += headers;
    request += "Content-Length: ";
    request += std::to_string(body.size());
    request += "\r\nConnection: close\r\n\r\n";
    request += body;
    if (!stream.write_all(request)) throw std::runtime_error("the request could not be written");

    std::string answer;
    const std::int64_t deadline = venue_ws::now_ms() + 10'000;
    while (true)
    {
        char buffer[4096];
        const int got = stream.read(buffer, sizeof(buffer));
        if (got > 0)
        {
            answer.append(buffer, static_cast<std::size_t>(got));
            if (answered(answer)) break;
            continue;
        }
        if (got == -2)
        {
            // The stream's 200 ms read timeout, not the venue's silence: the deadline is what decides this.
            if (venue_ws::now_ms() >= deadline)
                throw std::runtime_error("the venue did not answer within ten seconds");
            continue;
        }
        if (got == 0) break;  // the venue closed, so the answer is what arrived
        throw std::runtime_error("the answer could not be read");
    }
    return parse_answer(answer);
}
}  // namespace

namespace
{
// A JSON string body, as the dump's line needs it: the frames are JSON themselves, so a quote inside one would end the
// line's string early and make the file unreadable at exactly the frame a reader wants.
std::string json_escaped(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

// The login is the only frame that carries the account's key, passphrase and signature, and the dump is written to be
// read by whoever is debugging a silent session. Masking is done here rather than by the session because the session has
// no reason to know a file exists -- and this is the one place in the tree that could copy a key into one.
std::string masked_credentials(std::string_view frame)
{
    std::string text(frame);
    for (const std::string_view field : {"\"apiKey\":\"", "\"passphrase\":\"", "\"sign\":\""})
    {
        const std::size_t at = text.find(field);
        if (at == std::string::npos) continue;
        const std::size_t from = at + field.size();
        const std::size_t to = text.find('"', from);
        if (to == std::string::npos) continue;
        text.replace(from, to - from, "***");
    }
    return text;
}
}  // namespace

VenueOrderConnection::VenueOrderConnection(std::string instrument, OrderSession::Credentials credentials,
                                           WireConfig config, EnginePort& engine,
                                           std::function<std::uint64_t()> clock)
    : config_(std::move(config)),
      session_(std::move(instrument), std::move(credentials), config_.inst_code, config_.mapping, *this),
      engine_(engine),
      clock_(clock ? std::move(clock)
                   : std::function<std::uint64_t()>([] { return venue_ws::now_seconds(); }))
{
    if (!config_.rest_url.empty())
        placer_ = std::make_unique<RestPlacer>(config_.rest_url, session_.credentials(), config_.rest_limits);
    if (!config_.dump_path.empty())
    {
        // Appended rather than truncated: a run that reconnects writes several sessions into one file, and the order they
        // crossed in is what a reader follows. A file that cannot be opened is an error naming it, not a silent no-op.
        dump_.open(config_.dump_path, std::ios::out | std::ios::app);
        if (!dump_.is_open())
            throw std::runtime_error("could not open the order dump " + config_.dump_path);
    }
}

void VenueOrderConnection::dump_frame(std::string_view direction, std::string_view frame)
{
    if (!dump_.is_open()) return;
    std::lock_guard<std::mutex> lock(dump_mutex_);
    dump_ << "{\"dir\": \"" << direction << "\", \"frame\": \"" << json_escaped(masked_credentials(frame)) << "\"}\n";
    dump_.flush();
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
    const std::int64_t drop_at =
        config_.drop_after_seconds > 0.0
            ? venue_ws::now_ms() + static_cast<std::int64_t>(config_.drop_after_seconds * 1000.0)
            : 0;
    while (!stop_.load())
    {
        if (drop_at > 0 && venue_ws::now_ms() >= drop_at)
            throw std::runtime_error("dropped for the reconnect test");
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
        dump_frame("venue", payload);
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.on_frame(payload);
    }
}


void VenueOrderConnection::on_engine_line(std::string_view line)
{
    // The two queries are not orders and carry no id: they are read first so `place` never has to know them.
    if (line == "PENDING")
    {
        answer_pending();
        return;
    }
    if (line == "EQUITY")
    {
        answer_equity();
        return;
    }
    const bool is_order = line.rfind("ORDER", 0) == 0 || line.rfind("CLOSE", 0) == 0;
    std::lock_guard<std::mutex> lock(session_mutex_);
    try
    {
        place(line);
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

void VenueOrderConnection::place(std::string_view line)
{
    if (placer_ == nullptr)
    {
        // The websocket route: the session joins the request into the venue's frame and writes it to the socket.
        session_.on_engine_line(line);
        return;
    }
    // The REST route: the same translation, and its `args[0]` is the body. The venue's answer is the only report this
    // surface gives -- the socket's `orders` channel is what carries the state that follows.
    for (const std::string& reply : placer_->place(session_.request_of(line), session_.translator()))
    {
        if (engine_.write_line(reply)) ++counters_.replies;
    }
    publish_rest_counters();
}

void VenueOrderConnection::publish_rest_counters()
{
    if (placer_ == nullptr) return;
    counters_.placed.store(placer_->placed());
    counters_.venue_refusals.store(placer_->refused());
    counters_.transport_failures.store(placer_->transport_failures());
    counters_.rest_calls.store(placer_->calls());
    counters_.rest_connect_ms.store(placer_->connect_ms());
    counters_.rest_place_ms.store(placer_->place_ms());
    counters_.rest_429.store(placer_->rate_limited());
}

void VenueOrderConnection::answer_pending()
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    ++counters_.pending_queries;
    if (placer_ == nullptr)
    {
        // The socket route has no query endpoint in this client: saying so is the answer, because an empty list would
        // tell a recovery that no order is working and leave every inherited one alive.
        engine_.write_line("PENDING unavailable");
        ++counters_.query_failures;
        return;
    }
    const std::optional<std::vector<std::uint64_t>> ids = placer_->pending_orders();
    publish_rest_counters();
    if (!ids)
    {
        engine_.write_line("PENDING unavailable");
        ++counters_.query_failures;
        return;
    }
    std::string answer = "PENDING " + std::to_string(ids->size());
    for (const std::uint64_t id : *ids) answer += ' ' + std::to_string(id);
    if (engine_.write_line(answer)) ++counters_.replies;
}

void VenueOrderConnection::answer_equity()
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    ++counters_.equity_queries;
    if (placer_ == nullptr)
    {
        engine_.write_line("EQUITY unavailable");
        ++counters_.query_failures;
        return;
    }
    const std::optional<double> equity = placer_->balance_usd();
    publish_rest_counters();
    if (!equity)
    {
        engine_.write_line("EQUITY unavailable");
        ++counters_.query_failures;
        return;
    }
    std::ostringstream answer;
    answer.precision(10);
    answer << "EQUITY " << *equity;
    if (engine_.write_line(answer.str())) ++counters_.replies;
}

void VenueOrderConnection::to_venue(std::string_view frame)
{
    dump_frame("request", frame);
    send(frame);
}

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

// ------------------------------------------------------------------ the placement route

RestPlacer::RestPlacer(std::string base_url, OrderSession::Credentials credentials, RestLimits limits)
    : url_(venue_ws::parse_url(base_url)), credentials_(std::move(credentials)), limits_(limits)
{
    // The path is the venue's, not the base url's, so a base url carrying one would be silently dropped: refused instead.
    if (url_.target != "/")
        throw std::runtime_error("the REST placement's base url is a host and a port, not a path: " + base_url);
}

void RestPlacer::acquire(std::string_view path)
{
    if (limits_.requests_per_window == 0 || limits_.window_ms <= 0) return;
    const double capacity = static_cast<double>(limits_.requests_per_window);
    const double rate_per_ms = capacity / static_cast<double>(limits_.window_ms);
    while (true)
    {
        std::int64_t wait_ms = 0;
        {
            std::lock_guard<std::mutex> lock(bucket_mutex_);
            const std::int64_t now = venue_ws::now_ms();
            Bucket& bucket = buckets_[std::string(path)];
            if (!bucket.started)
            {
                bucket.started = true;
                bucket.last_ms = now;
                bucket.tokens = capacity;
            }
            else
            {
                bucket.tokens = std::min(capacity, bucket.tokens +
                                                        static_cast<double>(now - bucket.last_ms) * rate_per_ms);
                bucket.last_ms = now;
            }
            if (bucket.tokens >= 1.0)
            {
                bucket.tokens -= 1.0;
                return;
            }
            // Rounded up so a wake-up never arrives a fraction of a millisecond early and loops.
            wait_ms = static_cast<std::int64_t>(std::ceil((1.0 - bucket.tokens) / rate_per_ms));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<std::int64_t>(wait_ms, 1)));
    }
}

HttpResponse RestPlacer::request(std::string_view method, std::string_view path, std::string_view body,
                                 std::string& failure, std::int64_t& connect_ms)
{
    // The timestamp and the signature are one decision -- the signature is over this exact string -- so they are made
    // here together rather than by two calls that could straddle a millisecond.
    const std::string timestamp = rest_timestamp();
    std::string headers;
    headers += "Content-Type: application/json\r\n";
    // The proxy in front of the venue answers a default agent with a 403 whose body is `error code: 1010`, which reads
    // like a venue refusal and is not one.
    headers += "User-Agent: curl/8.5.0\r\n";
    headers += "OK-ACCESS-KEY: " + credentials_.api_key + "\r\n";
    headers += "OK-ACCESS-SIGN: " +
               venue_auth::rest_signature(timestamp, credentials_.secret, method, path, body) + "\r\n";
    headers += "OK-ACCESS-TIMESTAMP: " + timestamp + "\r\n";
    headers += "OK-ACCESS-PASSPHRASE: " + credentials_.passphrase + "\r\n";
    // The demo environment is the only one this tree has a key for, and it refuses a placement without this header; a
    // live placement is refused with it, which is the direction a tree with no live key should fail in.
    headers += "x-simulated-trading: 1\r\n";

    // Two retries, each after a longer wait. The bucket is what keeps a run under the venue's quota; this is what keeps
    // one 429 from becoming an order the strategy never hears about.
    static constexpr int kRateLimitRetries = 2;
    HttpResponse response;
    connect_ms = 0;
    for (int attempt = 0; attempt <= kRateLimitRetries; ++attempt)
    {
        acquire(path);
        const std::int64_t started = venue_ws::now_ms();
        ++calls_;
        std::int64_t call_connect_ms = 0;
        try
        {
            response = http_json(method, url_, path, headers, body, call_connect_ms);
        }
        catch (const std::exception& error_text)
        {
            ++failures_;
            connect_ms += call_connect_ms;
            place_ms_ += static_cast<std::uint64_t>(venue_ws::now_ms() - started);
            failure = std::string("the REST call could not reach the venue (") + error_text.what() + ")";
            return response;
        }
        // Counted on every path, because a call whose answer never arrived paid for its handshake too -- and those are
        // exactly the calls a kept-alive connection would have saved.
        connect_ms += call_connect_ms;
        place_ms_ += static_cast<std::uint64_t>(venue_ws::now_ms() - started);
        if (response.status != 429) return response;
        ++rate_limited_;
        if (attempt == kRateLimitRetries) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
    }
    return response;
}

std::vector<std::string> RestPlacer::place(const OrderTranslator::Request& request,
                                           const OrderTranslator& translator)
{
    std::string_view path;
    if (request.op == "order")
        path = "/api/v5/trade/order";
    else if (request.op == "cancel-order")
        path = "/api/v5/trade/cancel-order";
    else
        throw OrderError("'" + request.op + "' is not a request the placement route sends");

    std::string failure;
    std::int64_t connect_ms = 0;
    const HttpResponse response = this->request("POST", path, request.args_object, failure, connect_ms);
    if (!failure.empty())
    {
        std::cerr << failure << std::endl;
        return {"REJECT " + request.id + " venue"};
    }
    Json answer;
    std::string error;
    if (!JsonParser(response.body).parse(answer, error))
    {
        ++failures_;
        std::cerr << "the venue answered a placement with something that is not JSON (" << error << "): "
                  << response.body.substr(0, 160) << std::endl;
        return {"REJECT " + request.id + " venue"};
    }
    return lines_for(request.op, request.id, answer, translator, request.contracts,
                     request.reduce_only ? request.units : 0);
}

std::optional<std::vector<std::uint64_t>> RestPlacer::pending_orders()
{
    // `instType=SWAP` is part of the signed path, not a filter applied here: the venue signs the request path as sent,
    // and a query string added after signing is a different request to it.
    static constexpr std::string_view kPath = "/api/v5/trade/orders-pending?instType=SWAP";
    std::string failure;
    std::int64_t connect_ms = 0;
    const HttpResponse response = request("GET", kPath, "", failure, connect_ms);
    if (!failure.empty())
    {
        std::cerr << failure << std::endl;
        return std::nullopt;
    }
    Json answer;
    std::string error;
    if (!JsonParser(response.body).parse(answer, error))
    {
        ++failures_;
        std::cerr << "the venue answered the pending-orders query with something that is not JSON (" << error
                  << "): " << response.body.substr(0, 160) << std::endl;
        return std::nullopt;
    }
    const Json* code = answer.find("code");
    if (code == nullptr || code->as_text() != "0")
    {
        ++refused_;
        const Json* message = answer.find("msg");
        std::cerr << "the venue refused the pending-orders query: code="
                  << (code != nullptr ? code->as_text() : std::string()) << ' '
                  << (message != nullptr ? message->as_text() : std::string()) << std::endl;
        return std::nullopt;
    }
    const Json* data = answer.find("data");
    if (data == nullptr) return std::nullopt;
    std::vector<std::uint64_t> ids;
    for (const Json& item : data->items)
    {
        const Json* id = item.find("clOrdId");
        if (id == nullptr) continue;
        const std::string text = id->as_text();
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        // A client id that is not this client's numbering belongs to another tool on the same account: it is left
        // out rather than guessed at, and the engine can only query ids it wrote down anyway.
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0)
            ids.push_back(value);
    }
    return ids;
}

std::optional<double> RestPlacer::balance_usd()
{
    static constexpr std::string_view kPath = "/api/v5/account/balance";
    std::string failure;
    std::int64_t connect_ms = 0;
    const HttpResponse response = request("GET", kPath, "", failure, connect_ms);
    if (!failure.empty())
    {
        std::cerr << failure << std::endl;
        return std::nullopt;
    }
    Json answer;
    std::string error;
    if (!JsonParser(response.body).parse(answer, error))
    {
        ++failures_;
        std::cerr << "the venue answered the balance query with something that is not JSON (" << error << "): "
                  << response.body.substr(0, 160) << std::endl;
        return std::nullopt;
    }
    const Json* code = answer.find("code");
    const Json* data = answer.find("data");
    const Json* first = data != nullptr && !data->items.empty() ? &data->items.front() : nullptr;
    // The USDT equity, not the account-wide `totalEq`. A USD-margined strategy's ledger holds one currency, and an
    // account can carry spot holdings in others: the demo account's 1 BTC and 100 OKB are 73,000 of its 80,000
    // totalEq, and their prices move a number the strategy never traded. Measured 2026-11-17: totalEq 80,086.85
    // against a USDT equity of 4,986.73. `eq` includes the currency's unrealized PnL, which is what an equity is;
    // `cashBal` is the fallback when `eq` is empty.
    const Json* details = first != nullptr ? first->find("details") : nullptr;
    if (code != nullptr && code->as_text() == "0" && details != nullptr)
    {
        for (const Json& detail : details->items)
        {
            const Json* ccy = detail.find("ccy");
            if (ccy == nullptr || ccy->as_text() != "USDT") continue;
            const Json* eq = detail.find("eq");
            const Json* cash = detail.find("cashBal");
            const Json* value = nullptr;
            if (eq != nullptr && !eq->as_text().empty()) value = eq;
            else if (cash != nullptr) value = cash;
            if (value == nullptr) continue;
            try
            {
                return value->as_double();
            }
            catch (const std::exception&)
            {
                // A number the reader cannot convert is not an equity, and zero would read as a flat account.
            }
        }
    }
    ++refused_;
    const Json* message = first != nullptr ? first->find("sMsg") : nullptr;
    if (message == nullptr) message = answer.find("msg");
    std::cerr << "the venue did not report a USDT equity: code="
              << (code != nullptr ? code->as_text() : std::string()) << ' '
              << (message != nullptr ? message->as_text() : std::string()) << std::endl;
    return std::nullopt;
}

std::vector<std::string> RestPlacer::lines_for(std::string_view op, const std::string& order_id,
                                               const Json& answer, const OrderTranslator& translator,
                                               double sent_contracts, std::uint64_t sent_units)
{
    const Json* code = answer.find("code");
    const Json* data = answer.find("data");
    const Json* first = data != nullptr && !data->items.empty() ? &data->items.front() : nullptr;
    const Json* sub = first != nullptr ? first->find("sCode") : nullptr;
    const std::string code_text = code != nullptr ? code->as_text() : std::string();
    const std::string sub_text = sub != nullptr ? sub->as_text() : std::string();
    if (code_text == "0" && sub_text == "0")
    {
        if (op == "order")
        {
            ++placed_;
            // The reply names the order (`ordId`, `clOrdId`, `sCode`) and carries no `sz`, so the size in the
            // acknowledgement is the one this client sent; reading a missing field gave every live placement
            // `ACK <id> 0`, measured against the demo venue on 2026-11-17. When a venue does report `sz`, its number
            // wins, because that is what the venue actually accepted.
            const Json* sz = first != nullptr ? first->find("sz") : nullptr;
            double contracts = sz != nullptr ? sz->as_double() : 0.0;
            if (!(contracts > 0.0)) contracts = sent_contracts;
            // A close's acknowledgement is the units the engine asked to close: the venue's size was rounded up to
            // its grid, so `units_of(contracts)` would report a size the engine never sent.
            const std::uint64_t units = sent_units != 0 ? sent_units : translator.units_of(contracts);
            return {"ACK " + order_id + " " + std::to_string(units)};
        }
        return {"CANCELED " + order_id + " user"};
    }
    ++refused_;
    // The venue's own words, printed rather than swallowed: `sMsg` is the sub-code's reason and `msg` the top-level one.
    const Json* own = first != nullptr ? first->find("sMsg") : nullptr;
    const Json* top = answer.find("msg");
    std::cerr << "the venue refused request " << order_id << ": code=" << code_text << " sCode=" << sub_text << ' '
              << (own != nullptr ? own->as_text() : top != nullptr ? top->as_text() : std::string()) << std::endl;
    return {"REJECT " + order_id + " venue"};
}

}  // namespace venue_order_wire

