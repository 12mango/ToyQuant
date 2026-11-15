#include "lines/venue_relay.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "market/ws_feed.h"
#include "lines/venue_json.h"
#include "lines/venue_order_wire.h"
#include "lines/venue_ws.h"

namespace
{
// The URL, the TLS stream, the frames and the opening handshake are `venue_ws`'s, because the order leg needs the same
// four and a second copy would be a client that disagrees with this one about a mask or a timeout. The names are used
// unqualified here for the same reason `Json` is: this file is a client of that header, not a second implementation of it.
using namespace venue_ws;

constexpr int kPingAfterSeconds = 25;


// ------------------------------------------------------------------ the venue decoder

struct RelayStats
{
    uint64_t trades{0};
    uint64_t book_messages{0};
    uint64_t depth_rows{0};
    uint64_t gaps{0};
    uint64_t reconnects{0};
    uint64_t parse_errors{0};
    // Backpressure and heartbeat, both of which a six second run never exercises: the high-water mark
    // is how deep the hand-off to the engine got, the waits are how often it was full, and the two
    // heartbeat counters say the keepalive path ran at all.
    uint64_t queue_high_water{0};
    uint64_t queue_waits{0};
    uint64_t pings_sent{0};
    uint64_t pongs_sent{0};
    // A pong the venue sent back, which is what makes a keepalive a round trip rather than a write.
    uint64_t pongs_received{0};

    std::string to_log_string() const
    {
        return "trades=" + std::to_string(trades) + " book_messages=" +
               std::to_string(book_messages) + " depth_rows=" + std::to_string(depth_rows) +
               " gaps=" + std::to_string(gaps) + " reconnects=" + std::to_string(reconnects) +
               " parse_errors=" + std::to_string(parse_errors) +
               " queue_high_water=" + std::to_string(queue_high_water) +
               " queue_waits=" + std::to_string(queue_waits) +
               " pings_sent=" + std::to_string(pings_sent) +
               " pongs_sent=" + std::to_string(pongs_sent) +
               " pongs_received=" + std::to_string(pongs_received);
    }
};

int64_t json_int(const Json* value)
{
    return value == nullptr ? 0 : static_cast<int64_t>(std::llround(value->as_double()));
}

// The OKX public channels, decoded into the replay rows this repository already reads. One decoder
// serves all three roles -- relay, direct record, and capture replay -- so a row means the same thing
// wherever it is produced.
class VenueDecoder
{
   public:
    explicit VenueDecoder(const RelayConfig& config)
        : instrument_(config.instrument),
          contract_size_(config.contract_size),
          quantity_scale_(config.quantity_scale),
          max_levels_(config.levels)
    {
    }

    void decode(std::string_view message, std::vector<std::string>& rows, RelayStats& stats)
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

   private:
    uint64_t amount_of(const Json* size) const
    {
        const double value = size == nullptr ? 0.0 : size->as_double();
        return static_cast<uint64_t>(std::llround(value * contract_size_ *
                                                   static_cast<double>(quantity_scale_)));
    }

    void decode_trades(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
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

    void decode_books(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
                      RelayStats& stats);


    std::string instrument_;
    double contract_size_;
    uint64_t quantity_scale_;
    std::size_t max_levels_;
    int64_t last_seq_{0};
    bool has_last_seq_{false};
};

void VenueDecoder::decode_books(const Json& root, const std::string& symbol,
                                std::vector<std::string>& rows, RelayStats& stats)
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



std::string header_value(const std::string& request, const std::string& name)
{
    std::size_t start = request.find("\r\n");
    if (start == std::string::npos) return {};
    start += 2;
    while (start < request.size())
    {
        const std::size_t end = request.find("\r\n", start);
        if (end == std::string::npos) break;
        const std::string line = request.substr(start, end - start);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string key = line.substr(0, colon);
            for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key == name)
            {
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                return value;
            }
        }
        start = end + 2;
    }
    return {};
}

// Blocks until the engine connects and completes the handshake, then adopts the connection into `stream`. `role` names the
// channel in the line it prints, because a session with both legs runs two of these and the reader has to know which one
// came up first.
void accept_engine(const std::string& listen, Stream& stream, std::string_view role = "relay")
{
    const std::size_t colon = listen.find(':');
    if (colon == std::string::npos || colon == 0)
        throw std::runtime_error("--listen must be host:port, not " + listen);
    const std::string host = listen.substr(0, colon);
    const int port = std::stoi(listen.substr(colon + 1));

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("could not create the listening socket");
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "0.0.0.0")
        address.sin_addr.s_addr = INADDR_ANY;
    else if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("--listen needs a literal IPv4 address");
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        ::close(listener);
        throw std::runtime_error("could not bind " + listen);
    }
    if (::listen(listener, 1) < 0)
    {
        ::close(listener);
        throw std::runtime_error("could not listen on " + listen);
    }
    // The line prints the port the kernel gave, not the one that was asked for: `--listen=127.0.0.1:0` is how a test or a
    // wrapper asks for any free port, and a relay that echoed "0" back would leave the caller guessing.
    sockaddr_in bound{};
    socklen_t bound_length = sizeof(bound);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length);
    std::cout << role << " listening on ws://" << (host.empty() ? "0.0.0.0" : host) << ":"
              << ntohs(bound.sin_port) << ", waiting for the engine ..." << std::endl;

    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    const int conn = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
    ::close(listener);
    if (conn < 0) throw std::runtime_error("the engine connection was not accepted");

    std::string request;
    char byte = 0;
    while (request.find("\r\n\r\n") == std::string::npos)
    {
        const ssize_t got = ::recv(conn, &byte, 1, 0);
        if (got <= 0)
        {
            ::close(conn);
            throw std::runtime_error("the engine closed during the handshake");
        }
        request.push_back(byte);
        if (request.size() > 16 * 1024)
        {
            ::close(conn);
            throw std::runtime_error("the engine handshake is too large");
        }
    }
    const std::string key = header_value(request, "sec-websocket-key");
    if (key.empty())
    {
        ::close(conn);
        throw std::runtime_error("the engine handshake carries no Sec-WebSocket-Key");
    }
    const std::string accept = WebSocketFeed::accept_for_key(key);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                 accept + "\r\n\r\n";
    std::size_t done = 0;
    while (done < response.size())
    {
        const ssize_t sent = ::send(conn, response.data() + done, response.size() - done, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            ::close(conn);
            throw std::runtime_error("could not answer the engine handshake");
        }
        done += static_cast<std::size_t>(sent);
    }
    std::cout << "engine connected" << std::endl;
    stream.adopt(conn);
}


// ------------------------------------------------------------------ rows, recording, and the venue client

enum class Pop
{
    Row,
    Timeout,
    Closed
};

class RowQueue
{
   public:
    // Rows in flight between the venue thread and the thread serving the engine. Bounded for a live
    // relay, because an engine that falls behind must slow the venue down rather than grow this process
    // without limit; a replay reads a finite file, so it keeps the unbounded queue.
    static constexpr std::size_t kCapacity = 200000;

    explicit RowQueue(bool bounded) : bounded_(bounded) {}

    // Waits for room rather than dropping the row. Returns false once the queue is closed, so a stop
    // does not leave the venue thread blocked in a push nobody will consume.
    bool push(std::string row, RelayStats& stats)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (bounded_ && rows_.size() >= kCapacity)
        {
            ++stats.queue_waits;
            room_.wait(lock, [this] { return rows_.size() < kCapacity || closed_; });
            if (closed_) return false;
        }
        rows_.push_back(std::move(row));
        stats.queue_high_water = std::max<uint64_t>(stats.queue_high_water, rows_.size());
        ready_.notify_one();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        room_.notify_all();
        ready_.notify_all();
    }

    Pop pop(std::string& row, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return !rows_.empty() || closed_; }))
            return Pop::Timeout;
        if (rows_.empty()) return Pop::Closed;
        row = std::move(rows_.front());
        rows_.pop_front();
        room_.notify_one();
        return Pop::Row;
    }

   private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable room_;
    std::deque<std::string> rows_;
    bool closed_{false};
    bool bounded_{false};
};

class Recorder
{
   public:
    explicit Recorder(const std::string& prefix)
        : trades_(prefix + "_trades.csv", std::ios::out | std::ios::trunc),
          depth_(prefix + "_depth.csv", std::ios::out | std::ios::trunc)
    {
        if (!trades_.is_open() || !depth_.is_open())
            throw std::runtime_error("could not open the recording files under " + prefix);
        trades_ << "exchange,symbol,timestamp,id,side,price,amount\n";
        depth_ << "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n";
    }

    void write(const std::string& row)
    {
        // A trade row has seven fields and a depth row eight, which is how the reader tells them apart.
        std::size_t commas = 0;
        for (const char c : row)
            if (c == ',') ++commas;
        (commas == 6 ? trades_ : depth_) << row;
        // Flushed as it goes, so a session that is stopped rather than closed loses at most this many
        // rows instead of whatever the stream was holding.
        if (++rows_ % 4096 == 0) flush();
    }

    void flush()
    {
        trades_.flush();
        depth_.flush();
    }

   private:
    std::ofstream trades_;
    std::ofstream depth_;
    std::size_t rows_{0};
};

class VenueClient
{
   public:
    VenueClient(const RelayConfig& config, VenueDecoder& decoder, RelayStats& stats, RowQueue& rows,
                Recorder* recorder, bool enqueue)
        : config_(config), decoder_(decoder), stats_(stats), rows_(rows), recorder_(recorder),
          enqueue_(enqueue)
    {
        if (!config_.capture.empty())
        {
            capture_.open(config_.capture, std::ios::out | std::ios::app);
            if (!capture_.is_open())
                throw std::runtime_error("could not open " + config_.capture);
        }
    }

    void run()
    {
        const int64_t deadline = config_.max_seconds > 0.0
                                     ? now_ms() + static_cast<int64_t>(config_.max_seconds * 1000.0)
                                     : 0;
        int backoff_ms = 1000;
        while (!stop_.load())
        {
            if (deadline > 0 && now_ms() >= deadline) break;
            const int64_t started_ms = now_ms();
            try
            {
                session(deadline);
                backoff_ms = 1000;
            }
            catch (const std::exception& error)
            {
                if (stop_.load()) break;
                ++stats_.reconnects;
                // A long session that drops starts the backoff over; only a venue that keeps refusing
                // a connection should walk it up to eight seconds.
                if (now_ms() - started_ms >= 30000) backoff_ms = 1000;
                std::cerr << "venue connection lost (" << error.what() << "); reconnecting in "
                          << backoff_ms / 1000 << "s" << std::endl;
            }
            if (stop_.load()) break;
            if (deadline > 0 && now_ms() >= deadline) break;
            // Sleep in slices so stop() does not wait out the whole backoff.
            for (int waited = 0; waited < backoff_ms && !stop_.load(); waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff_ms = static_cast<int>(std::min<int64_t>(backoff_ms * 2LL, 8000LL));
        }
        if (enqueue_) rows_.close();
    }

    void stop() { stop_.store(true); }

   private:
    void session(int64_t deadline)
    {
        const Url url = parse_url(config_.venue);
        Stream stream;
        stream.connect(url);
        client_handshake(stream, url);
        const std::string subscription =
            "{\"op\":\"subscribe\",\"args\":[{\"channel\":\"" + config_.books_channel +
            "\",\"instId\":\"" + config_.instrument + "\"},{\"channel\":\"trades\",\"instId\":\"" +
            config_.instrument + "\"}]}";
        if (!send_frame(stream, 0x1, subscription, true))
            throw std::runtime_error("could not subscribe");

        const int64_t session_start_ms = now_ms();
        int64_t last_ping_ms = session_start_ms;
        const int64_t ping_interval_ms =
            config_.ping_seconds > 0.0 ? static_cast<int64_t>(config_.ping_seconds * 1000.0)
                                       : 25000;
        while (!stop_.load())
        {
            if (deadline > 0 && now_ms() >= deadline) return;
            if (config_.drop_after_seconds > 0.0 &&
                now_ms() - session_start_ms >=
                    static_cast<int64_t>(config_.drop_after_seconds * 1000.0))
                throw std::runtime_error("dropped for the reconnect test");
            std::string payload;
            int opcode = 0;
            bool fin = true;
            const int status = read_frame(stream, payload, opcode, fin);
            if (status == -2)
            {
                const int64_t now = now_ms();
                if (now - last_ping_ms >= ping_interval_ms)
                {
                    send_frame(stream, 0x9, "ping", true);  // keep an idle public connection alive
                    ++stats_.pings_sent;
                    last_ping_ms = now;
                }
                continue;
            }
            if (status != 1) return;  // closed or broken
            if (opcode == 0x8) return;
            if (opcode == 0x9)
            {
                // A control ping is owed a control pong with the same payload. A session that answers
                // neither this nor the text form is dropped by the venue once its heartbeat expires.
                send_frame(stream, 0xA, payload, true);
                ++stats_.pongs_sent;
                continue;
            }
            if (opcode == 0xA)
            {
                ++stats_.pongs_received;
                continue;
            }
            if (opcode != 0x1 && opcode != 0x0) continue;
            // OKX's heartbeat is a text frame rather than a control frame, and neither it nor its
            // answer is a market message to capture or decode.
            if (payload == "ping")
            {
                send_frame(stream, 0x1, "pong", true);
                ++stats_.pongs_sent;
                continue;
            }
            if (payload == "pong")
            {
                ++stats_.pongs_received;
                continue;
            }
            // A subscription acknowledgement is protocol, not market data.
            if (payload.find("\"event\"") != std::string::npos) continue;
            if (capture_.is_open())
            {
                capture_ << payload << "\n";
                capture_.flush();
            }
            std::vector<std::string> decoded;
            decoder_.decode(payload, decoded, stats_);
            for (std::string& row : decoded)
            {
                if (recorder_ != nullptr) recorder_->write(row);
                if (enqueue_ && !rows_.push(std::move(row), stats_)) return;
            }
        }
    }

    RelayConfig config_;
    VenueDecoder& decoder_;
    RelayStats& stats_;
    RowQueue& rows_;
    Recorder* recorder_;
    bool enqueue_;
    std::ofstream capture_;
    std::atomic<bool> stop_{false};
};

// ------------------------------------------------------------------ serving the engine, and the two entry points

void stream_engine(Stream& engine, RowQueue& rows)
{
    send_frame(engine, 0x1, "exchange,symbol,timestamp,id,side,price,amount\n", false);
    send_frame(engine, 0x1,
               "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n", false);
    std::string row;
    while (true)
    {
        const Pop result = rows.pop(row, 200);
        if (result == Pop::Closed) break;
        if (result == Pop::Timeout) continue;
        if (!send_frame(engine, 0x1, row, false)) break;
    }
    send_frame(engine, 0x8, "", false);
}

// ------------------------------------------------------------------ the order leg

// The engine's end of the order channel: `LiveVenue`'s lines out as text frames and the session's answers back in. The
// engine is a WebSocket client of this relay exactly as it is of the market leg, so `TcpVenueTransport` cannot tell this
// route from the simulated one -- which is the property ADR 0005 decision 1 asked for.
class EngineStreamPort : public venue_order_wire::EnginePort
{
   public:
    explicit EngineStreamPort(Stream& engine) : engine_(engine) {}

    bool write_line(std::string_view line) override
    {
        // Written by the venue's reader while the engine's own reader answers a control ping on the same socket, so the
        // two writes are serialised. A mutex is enough because a frame is written whole or not at all.
        std::lock_guard<std::mutex> lock(mutex_);
        return send_frame(engine_, 0x1, line, false);
    }

   private:
    Stream& engine_;
    std::mutex mutex_;
};

// Serves one engine connection for the order channel: the venue's private session on one side and the engine's line
// socket on the other. It returns when the engine hangs up -- the Python relay's shape as well -- and the summary is the
// one that route prints: what crossed, what was refused, and how many times the session had to log in again.
void serve_order_leg(const RelayConfig& config)
{
    // The credentials are read before the socket is accepted: an engine that connected only to be dropped for a missing
    // variable would meet the failure as a dead wire rather than as the name of the variable to set.
    const OrderSession::Credentials credentials = venue_order_wire::credentials_from_environment();
    // The URL is checked here, before the socket is accepted: a typo is an error naming it rather than a listener that
    // says it is ready and then a loop of failed connections behind it.
    venue_ws::parse_url(config.order_venue);

    venue_order_wire::WireConfig wire;
    wire.venue_url = config.order_venue;
    wire.inst_code = config.order_inst_code;
    wire.mapping = OrderMapping{.contract_size = config.contract_size,
                                .quantity_scale = config.quantity_scale,
                                .tick_size = config.tick_size,
                                .lot_size = config.lot_size};

    Stream engine;
    accept_engine(config.order_listen, engine, "order channel");
    EngineStreamPort port(engine);
    venue_order_wire::VenueOrderConnection connection(config.instrument, credentials, wire, port);
    std::thread thread([&connection] { connection.run(); });

    std::string payload;
    int opcode = 0;
    bool fin = true;
    while (true)
    {
        const int status = read_frame(engine, payload, opcode, fin);
        if (status == -2) continue;  // the read timeout is the loop's own clock, not an error
        if (status != 1) break;
        if (opcode == 0x8) break;
        if (opcode == 0x9)
        {
            send_frame(engine, 0xA, payload, false);
            continue;
        }
        if (opcode != 0x1 && opcode != 0x0) continue;
        connection.on_engine_line(payload);
    }

    connection.stop();
    thread.join();
    std::cout << "order channel done: " << connection.counters().to_log_string() << ' '
              << connection.session().summary();
    const std::string lost = connection.last_error();
    if (!lost.empty()) std::cout << " last_error=\"" << lost << '"';
    std::cout << std::endl;
}

int run_relay_impl(const RelayConfig& config)
{
    if (!config.order_listen.empty())
    {
        // The order leg is its own route and returns when the engine hangs up, which is what the Python relay does: a
        // live session that needs the market leg too runs the other process.
        serve_order_leg(config);
        return 0;
    }
    VenueDecoder decoder(config);
    RelayStats stats;

    if (!config.replay.empty())
    {
        std::ifstream capture(config.replay);
        if (!capture.is_open()) throw std::runtime_error("could not open " + config.replay);
        std::unique_ptr<Recorder> recorder;
        if (!config.record.empty()) recorder = std::make_unique<Recorder>(config.record);
        RowQueue rows(false);  // a replay reads a finite file, so it needs no bound
        std::string line;
        while (std::getline(capture, line))
        {
            if (line.empty()) continue;
            std::vector<std::string> decoded;
            decoder.decode(line, decoded, stats);
            for (std::string& row : decoded)
            {
                if (recorder != nullptr) recorder->write(row);
                rows.push(std::move(row), stats);
            }
        }
        if (recorder != nullptr) recorder->flush();
        rows.close();
        if (!config.listen.empty())
        {
            Stream engine;
            accept_engine(config.listen, engine);
            stream_engine(engine, rows);
        }
        std::cout << stats.to_log_string() << std::endl;
        return 0;
    }

#ifndef TOYQUANT_HAVE_OPENSSL
    // A build without OpenSSL cannot reach a real venue; fail by name instead of retrying a wall.
    if (parse_url(config.venue).secure)
        throw std::runtime_error(
            "this build has no TLS: pass a ws:// venue, or build with OpenSSL's headers");
#endif

    std::unique_ptr<Recorder> recorder;
    if (!config.record.empty()) recorder = std::make_unique<Recorder>(config.record);
    RowQueue rows(true);  // a live relay bounds the hand-off, so a stalled engine slows the venue down
    const bool serve = !config.listen.empty();
    Stream engine;
    if (serve) accept_engine(config.listen, engine);

    VenueClient client(config, decoder, stats, rows, recorder.get(), serve);
    std::thread thread([&client] { client.run(); });
    if (serve)
    {
        stream_engine(engine, rows);
        client.stop();
    }
    thread.join();
    if (recorder != nullptr) recorder->flush();
    std::cout << stats.to_log_string() << std::endl;
    return 0;
}

bool check(bool condition, const char* what)
{
    if (!condition) std::cerr << "self-test FAILED: " << what << "\n";
    return condition;
}

int self_test_impl()
{
    RelayConfig config;
    config.levels = 1;  // so the snapshot cap is visible
    VenueDecoder decoder(config);
    RelayStats stats;
    std::vector<std::string> rows;

    decoder.decode(
        R"({"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":[{"instId":"BTC-USDT-SWAP","tradeId":"7","px":"100.5","sz":"2.5","side":"buy","ts":"1000","seqId":5}]})",
        rows, stats);
    bool ok = check(rows.size() == 1 && rows[0] == "okx,BTC-USDT-SWAP,1000,7,buy,100.5,25000\n",
                    "a trade row");

    rows.clear();
    decoder.decode(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":[{"asks":[["100.6","3","0","1"],["100.7","1","0","1"]],"bids":[["100.4","5","0","2"],["100.3","2","0","2"]],"ts":"1000","seqId":10,"prevSeqId":-1}]})",
        rows, stats);
    ok = check(rows.size() == 2 && rows[0] == "okx,BTC-USDT-SWAP,1000,1000,1,bid,100.4,50000\n" &&
                   rows[1] == "okx,BTC-USDT-SWAP,1000,1000,1,ask,100.6,30000\n",
               "a snapshot capped to one level a side") && ok;

    rows.clear();
    decoder.decode(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":[{"asks":[["100.6","0","0","0"]],"bids":[],"ts":"1001","seqId":11,"prevSeqId":10}]})",
        rows, stats);
    ok = check(rows.size() == 1 && rows[0] == "okx,BTC-USDT-SWAP,1001,1001,0,ask,100.6,0\n",
               "a removal") && ok;
    ok = check(stats.gaps == 0, "no gap when the sequence continues") && ok;

    rows.clear();
    decoder.decode(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":[{"asks":[],"bids":[],"ts":"1002","seqId":99,"prevSeqId":42}]})",
        rows, stats);
    ok = check(stats.gaps == 1, "a gap when the sequence breaks") && ok;

    rows.clear();
    decoder.decode(R"({"event":"subscribe","arg":{"channel":"books"},"connId":"x"})", rows, stats);
    ok = check(rows.empty(), "an event is ignored") && ok;
    decoder.decode("pong", rows, stats);
    ok = check(stats.parse_errors == 1, "a non-JSON line is counted, not crashed") && ok;

    if (ok) std::cout << "self-test OK" << std::endl;
    return ok ? 0 : 1;
}
}  // namespace

int run_relay(const RelayConfig& config)
{
    return run_relay_impl(config);
}

int relay_self_test()
{
    return self_test_impl();
}

