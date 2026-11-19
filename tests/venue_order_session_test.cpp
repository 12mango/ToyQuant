// The order channel's session, driven in one process, and then the same machine over a loopback socket: the login it
// emits is checked against the signature a stand-in venue computes from the same secret, the subscription follows the
// venue's answer rather than a timer, a drop forces a new login and a new subscription, and an account's channel is
// filtered to the instrument this session trades.
//
// The stand-in is the assertion, and it is there twice on purpose. Above, a `Sink` recorder checks what the session
// *builds*; below, a socket checks what a venue *reads*, through the relay's own frame helpers -- so a mask, a length or
// a handshake that only this client understands fails here rather than on the first sandbox order. A test that compared
// the session with itself would agree with any wrong preimage.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lines/venue_auth.h"
#include "lines/venue_json.h"
#include "lines/venue_order_session.h"
#include "lines/venue_order_wire.h"
#include "lines/venue_ws.h"

namespace
{
class RecordingSink : public OrderSession::Sink
{
   public:
    void to_venue(std::string_view frame) override { venue.emplace_back(frame); }
    void to_engine(std::string_view line) override { engine.emplace_back(line); }

    std::vector<std::string> venue;
    std::vector<std::string> engine;
};

Json parse(std::string_view text)
{
    Json value;
    std::string error;
    JsonParser parser(text);
    const bool ok = parser.parse(value, error);
    assert(ok);
    return value;
}

std::string field(const Json& object, std::string_view key)
{
    const Json* found = object.find(key);
    return found == nullptr ? std::string() : found->as_text();
}

// What a stand-in venue does with the login it received: the timestamp and the secret are its own, so the signature is
// recomputed here rather than read back from the frame.
bool login_is_signed(const Json& login, std::string_view secret)
{
    const Json* args = login.find("args");
    if (args == nullptr || args->items.empty()) return false;
    const Json& entry = args->items.front();
    const std::string timestamp = field(entry, "timestamp");
    const std::string sign = field(entry, "sign");
    return !timestamp.empty() && sign == venue_auth::login_signature(timestamp, secret);
}


// A stand-in venue on a loopback socket. It answers the WebSocket handshake, reads the login with the relay's own frame
// reader, and recomputes the signature from the secret it holds -- so the preimage, the encoding and the mask are all
// checked where a venue would check them rather than where the session builds them. It then acknowledges the login,
// acknowledges the `orders` subscription, pushes one fill, and can drop the connection on request: a drop that the client
// has to answer with a new login is the half of the session a `Sink` cannot see.
class StandInVenue
{
   public:
    explicit StandInVenue(std::string secret) : secret_(std::move(secret)), listener_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        if (listener_ < 0) throw std::runtime_error("the stand-in venue could not create a socket");
        int one = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;  // any free port: the test reads the port back rather than guessing one
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            throw std::runtime_error("the stand-in venue could not bind 127.0.0.1");
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        if (::listen(listener_, 4) < 0) throw std::runtime_error("the stand-in venue could not listen");
        thread_ = std::thread([this] { serve(); });
    }

    StandInVenue(const StandInVenue&) = delete;
    StandInVenue& operator=(const StandInVenue&) = delete;

    ~StandInVenue()
    {
        stop_.store(true);
        // Closing a listener under a blocked `accept` is not portable; shutting it down first is what ends that call.
        ::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        ::close(listener_);
    }

    [[nodiscard]] std::uint16_t port() const { return port_; }
    // The next connection ends once the session has answered its subscription: the client has to make a new one.
    void drop_next_session() { drop_.store(true); }

    [[nodiscard]] int logins() const { return logins_.load(); }
    [[nodiscard]] int subscribes() const { return subscribes_.load(); }
    [[nodiscard]] int fills() const { return fills_.load(); }
    [[nodiscard]] int orders() const { return orders_.load(); }
    [[nodiscard]] bool login_signature_matched() const { return signed_login_.load(); }
    [[nodiscard]] bool subscribed() const { return subscribed_.load(); }

    // The cancellation the channel reports, pushed on the next pass of the session loop: the REST answer to a cancel
    // is the request being accepted, and the venue's own state change is what ends the order.
    void push_cancel(const std::string& order_id)
    {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        cancel_push_ = order_id;
    }

   private:
    std::unique_ptr<venue_ws::Stream> accept()
    {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        const int fd = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &length);
        if (fd < 0) return nullptr;
        // The 200 ms timeout is what lets this thread notice `stop_` and a requested drop while the client is quiet.
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200'000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        auto stream = std::make_unique<venue_ws::Stream>();
        stream->adopt(fd);
        std::string request;
        char byte = 0;
        while (request.find("\r\n\r\n") == std::string::npos)
        {
            const int got = stream->read(&byte, 1);
            if (got == -2) continue;
            if (got != 1) return nullptr;
            request.push_back(byte);
            if (request.size() > 16 * 1024) return nullptr;
        }
        // Answered for its status line alone. The client reads no accept key, which is the one place this stand-in is
        // allowed to be simpler than a venue -- and the reason `client_handshake` checks the status and not the key.
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: toyquant-stand-in\r\n\r\n";
        if (!stream->write_all(response)) return nullptr;
        return stream;
    }

    void send(venue_ws::Stream& stream, std::string_view text) { venue_ws::send_frame(stream, 0x1, text, false); }

    // One connection, from the login to the close. The login and the subscription are acknowledged, a fill the channel
    // would push follows the subscription, and `orders` counts what the engine sent so the test can check the request
    // crossed rather than only that the socket came up.
    void serve_session(venue_ws::Stream& stream, bool may_drop)
    {
        std::string payload;
        int opcode = 0;
        bool fin = true;
        while (!stop_.load())
        {
            if (may_drop && drop_.load()) return;  // a drop the client has to answer with a new login
            std::string cancel_id;
            {
                std::lock_guard<std::mutex> lock(cancel_mutex_);
                cancel_id.swap(cancel_push_);
            }
            if (!cancel_id.empty())
            {
                send(stream, std::string(R"({"arg":{"channel":"orders"},"data":[{"clOrdId":")") + cancel_id +
                                  R"(","state":"canceled"}]})");
                continue;
            }
            const int status = venue_ws::read_frame(stream, payload, opcode, fin);
            if (status == -2) continue;
            if (status != 1 || opcode == 0x8) return;
            if (opcode == 0x9)
            {
                venue_ws::send_frame(stream, 0xA, payload, false);
                continue;
            }
            if (opcode != 0x1 && opcode != 0x0) continue;
            if (payload == "ping")
            {
                send(stream, "pong");
                continue;
            }
            const Json message = parse(payload);
            const std::string op = field(message, "op");
            if (op == "login")
            {
                signed_login_.store(login_is_signed(message, secret_));
                ++logins_;
                send(stream, R"({"event":"login","code":"0"})");
                continue;
            }
            if (op == "subscribe")
            {
                subscribed_.store(true);
                ++subscribes_;
                send(stream, R"({"event":"subscribe","arg":{"channel":"orders"}})");
                // The fill the channel pushes once the subscription is live: the engine's line is what says the
                // translation ran on a frame a venue would have sent, not on a string this test built.
                send(stream, R"({"arg":{"channel":"orders"},"data":[{"clOrdId":"7","instId":"BTC-USDT-SWAP",)"
                            R"("state":"partially_filled","fillPx":"6421.5","accFillSz":"1","fillFee":"-0.1"}]})");
                ++fills_;
                continue;
            }
            // `op: order` / `op: cancel-order`: the venue's own answer to a placement is its REST reply on this account,
            // so the channel only counts it. The state channel is where the fill came from.
            ++orders_;
        }
    }

    void serve()
    {
        // Every connection is answered until the test stops. A dropped session is a *new* session at the venue -- the
        // login and the subscription go out again -- and both the forced drop in the first half and the scheduled one in
        // the second make several, so the loop is the venue's shape rather than two counts of it.
        while (!stop_.load())
        {
            auto stream = accept();
            if (stream == nullptr) return;
            serve_session(*stream, true);
            drop_.store(false);
        }
    }

    std::string secret_;
    int listener_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> drop_{false};
    std::atomic<bool> signed_login_{false};
    std::atomic<bool> subscribed_{false};
    std::atomic<int> logins_{0};
    std::atomic<int> subscribes_{0};
    std::atomic<int> fills_{0};
    std::atomic<int> orders_{0};
    std::mutex cancel_mutex_;
    std::string cancel_push_;
    std::thread thread_;
};

// Where the session's engine-side lines go in the transport half: the relay passes the socket it accepted from the
// engine, and the test passes this.
class RecordingPort : public venue_order_wire::EnginePort
{
   public:
    bool write_line(std::string_view line) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines.emplace_back(line);
        return true;
    }

    bool contains(std::string_view prefix)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& line : lines)
            if (line.rfind(prefix, 0) == 0) return true;
        return false;
    }

    std::vector<std::string> lines;

   private:
    std::mutex mutex_;
};

// A deadline rather than a sleep: the test's thread waits for the socket's thread to reach a point, and an implementation
// that never gets there fails the assertion instead of hanging the gate.
template <typename Predicate>
bool wait_for(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

// A whole file, for the assertions that read what a run left behind rather than what it printed.
std::string read_file(const std::string& path)
{
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}



// The placement surface on a loopback socket: one HTTP/1.1 request at a time, the venue's own answer shape. It recomputes
// the signature from the secret it holds -- over the bytes it received and the timestamp header it received -- which is
// the one check a wrong preimage or a wrong millisecond format cannot pass.
class StandInRest
{
   public:
    explicit StandInRest(std::string secret) : secret_(std::move(secret)), listener_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        if (listener_ < 0) throw std::runtime_error("the stand-in REST endpoint could not create a socket");
        int one = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            throw std::runtime_error("the stand-in REST endpoint could not bind 127.0.0.1");
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        if (::listen(listener_, 4) < 0) throw std::runtime_error("the stand-in REST endpoint could not listen");
        thread_ = std::thread([this] { serve(); });
    }

    StandInRest(const StandInRest&) = delete;
    StandInRest& operator=(const StandInRest&) = delete;

    ~StandInRest()
    {
        stop_.store(true);
        ::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        ::close(listener_);
    }

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] bool signature_matched() const { return signed_.load(); }
    [[nodiscard]] bool carried_the_demo_header() const { return simulated_.load(); }
    [[nodiscard]] int requests() const { return requests_.load(); }
    [[nodiscard]] int rate_limit_hits() const { return rate_limit_hits_.load(); }
    std::string last_body()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return body_;
    }

   private:
    static std::string header_of(const std::string& request, std::string_view name)
    {
        std::string lower_request;
        lower_request.reserve(request.size());
        for (const char c : request)
            lower_request.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        std::string at = "\r\n";
        at += name;
        at += ':';
        const std::size_t found = lower_request.find(at);
        if (found == std::string::npos) return {};
        const std::size_t start = found + at.size();
        const std::size_t end = request.find("\r\n", start);
        std::string value = request.substr(start, end - start);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        return value;
    }


    void serve()
    {
        while (!stop_.load())
        {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            const int fd = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &length);
            if (fd < 0) return;
            handle(fd);
            ::close(fd);
        }
    }

    void handle(int fd)
    {
        std::string request;
        char byte = 0;
        while (request.find("\r\n\r\n") == std::string::npos)
        {
            const ssize_t got = ::recv(fd, &byte, 1, 0);
            if (got <= 0) return;
            request.push_back(byte);
            if (request.size() > 64 * 1024) return;
        }
        // An HTTP body is not line-delimited, so the length header is the only place its edge can be found.
        const std::size_t length = static_cast<std::size_t>(
            std::strtoul(header_of(request, "content-length").c_str(), nullptr, 10));
        std::string body = request.substr(request.find("\r\n\r\n") + 4);
        while (body.size() < length)
        {
            char buffer[1024];
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            body.append(buffer, static_cast<std::size_t>(got));
        }
        const std::size_t first = request.find(' ');
        const std::size_t second = request.find(' ', first + 1);
        const std::string method = request.substr(0, first);
        const std::string path = request.substr(first + 1, second - first - 1);
        const std::string timestamp = header_of(request, "ok-access-timestamp");
        // The method and the path as sent are part of the preimage: a GET's query string is signed with it, and a client
        // that signed a bare path would be refused by the venue rather than by this stand-in.
        signed_.store(!timestamp.empty() &&
                      header_of(request, "ok-access-sign") ==
                          venue_auth::rest_signature(timestamp, secret_, method, path, body));
        simulated_.store(header_of(request, "x-simulated-trading") == "1");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = body;
        }
        ++requests_;
        // The two queries, answered from the shapes the venue's own documentation gives: a pending list keyed by
        // `clOrdId`, and a balance whose `totalEq` is the account's equity in USD.
        if (method == "GET")
        {
            std::string answer;
            if (path.rfind("/api/v5/trade/orders-pending", 0) == 0)
                answer = R"({"code":"0","msg":"","data":[{"clOrdId":"11","instId":"BTC-USDT-SWAP",)"
                         R"("state":"live"},{"clOrdId":"12","instId":"BTC-USDT-SWAP","state":"live"}]})";
            else if (path.rfind("/api/v5/account/balance", 0) == 0)
                answer = R"({"code":"0","msg":"","data":[{"totalEq":"80086.85",)"
                         R"("details":[{"ccy":"USDT","eq":"4986.73"},{"ccy":"BTC","eq":"1.0"}]}]})";
            else if (path.rfind("/api/v5/account/positions", 0) == 0)
                // One contract short. The engine's units are the contract size taken apart: 0.01 of the coin at a
                // one-millionth scale is ten thousand units, and the sign is the position's.
                answer = R"({"code":"0","msg":"","data":[{"instId":"BTC-USDT-SWAP","pos":"-1","avgPx":"6421.5"}]})";
            else
                answer = R"({"code":"404","msg":"unknown path","data":[]})";
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                         std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer;
            ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
            return;
        }
        // One rate-limited order, so the client's retry path is exercised rather than assumed: two 429s and then the
        // ordinary acknowledgement.
        if (body.find("\"clOrdId\":\"24\"") != std::string::npos && rate_limit_hits_.load() < 2)
        {
            ++rate_limit_hits_;
            const std::string answer = R"({"code":"50011","msg":"Rate limit reached","data":[]})";
            const std::string response =
                "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer;
            ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
            return;
        }
        // Three answer shapes a loopback with one fixed reply never produces, chosen by the order id so one stand-in can
        // pin all of the client's paths: a chunked body, a body that is not JSON, and a connection closed with no answer.
        // A placement whose reply is the venue's real shape: an order id and a status, with no `sz`. The client has
        // to acknowledge the size it sent rather than a missing field, which is what the live route did before this
        // branch existed.
        if (body.find("\"clOrdId\":\"25\"") != std::string::npos)
        {
            const std::string answer =
                R"({"code":"0","msg":"","data":[{"clOrdId":"25","ordId":"3","sCode":"0","sMsg":"Order placed"}]})";
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                         std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer;
            ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
            return;
        }
        if (body.find("\"clOrdId\":\"21\"") != std::string::npos)
        {
            const std::string answer = R"({"code":"0","msg":"","data":[{"clOrdId":"21","ordId":"2",)"
                                       R"("sCode":"0","sMsg":"Order placed","sz":"10"}]})";
            const std::size_t split = answer.size() / 3;
            std::string chunked = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\n";
            for (const std::string_view part : {answer.substr(0, split),
                                                answer.substr(split, split),
                                                answer.substr(2 * split)})
            {
                char size[32];  // wider than %zx needs for a size_t, because the compiler's bound is not
                std::snprintf(size, sizeof(size), "%zx", part.size());
                chunked += size;
                chunked += "\r\n";
                chunked += part;
                chunked += "\r\n";
            }
            chunked += "0\r\n\r\n";
            ::send(fd, chunked.data(), chunked.size(), MSG_NOSIGNAL);
            return;
        }
        if (body.find("\"clOrdId\":\"22\"") != std::string::npos)
        {
            const std::string answer = "not JSON, because a proxy can answer instead of the venue";
            const std::string response = "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nContent-Length: " +
                                         std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer;
            ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
            return;
        }
        if (body.find("\"clOrdId\":\"23\"") != std::string::npos) return;  // closed with no answer at all
        // The demo venue's two answers, which are the two this route has to read: an accepted placement whose `sz` is in
        // contracts, and a refusal that is a *sub*-code with the top-level code saying the call itself failed.
        std::string answer;
        if (body.find("\"sz\":\"1000\"") != std::string::npos)
            answer = R"({"code":"1","msg":"All operations failed","data":[{"clOrdId":"12","sCode":"51008",)"
                     R"("sMsg":"Insufficient balance"}]})";
        else if (path == "/api/v5/trade/cancel-order")
            answer = R"({"code":"0","msg":"","data":[{"clOrdId":"11","ordId":"1","sCode":"0","sMsg":""}]})";
        else
            answer = R"({"code":"0","msg":"","data":[{"clOrdId":"11","ordId":"1","sCode":"0",)"
                     R"("sMsg":"Order placed","sz":"10"}]})";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                     std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer;
        ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
    }

    std::string secret_;
    int listener_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> signed_{false};
    std::atomic<bool> simulated_{false};
    std::atomic<int> requests_{0};
    std::atomic<int> rate_limit_hits_{0};
    std::mutex mutex_;
    std::string body_;
    std::thread thread_;
};

}  // namespace

int main()
{
    const std::string secret = "toyquant-relay-self-test-secret";
    RecordingSink sink;
    OrderSession session("BTC-USDT-SWAP", OrderSession::Credentials{.api_key = "key-1",
                                                                    .secret = secret,
                                                                    .passphrase = "pass-1"},
                         10459, OrderMapping{}, sink);

    // The connection comes up on the venue's clock, which is the string the signature is over.
    session.on_connected(1794528000);
    assert(sink.venue.size() == 1);
    const Json login = parse(sink.venue[0]);
    assert(field(login, "op") == "login");
    assert(login_is_signed(login, secret));
    // And on this timestamp the signature is the relay's pinned vector, which is the same value its Python self-test
    // checks: two implementations agreeing on one preimage is the only thing that makes a login a login.
    assert(field(login.find("args")->items.front(), "sign") == "WIUsY7IKdr05lNBq2VgPKZu5ME3ZgT1i6w4ixCiJ9W8=");
    assert(session.counters().logins == 1);

    // The subscription goes out when the venue answers the login, not before: a subscribe on an unauthenticated socket
    // is refused, which is why the state machine waits for the event rather than a timer.
    session.on_frame(R"({"event":"login","code":"0"})");
    assert(sink.venue.size() == 2);
    assert(sink.venue[1] == R"({"op":"subscribe","args":[{"channel":"orders","instType":"ANY"}]})");
    assert(session.counters().subscribes == 1);

    // An order line from the engine becomes the venue's request on the same frame the translator produces, so the
    // session adds no second translation.
    session.on_engine_line("ORDER 7 BTC-PERPETUAL B 6421.5 100000 Gtc");
    assert(sink.venue.size() == 3);
    assert(sink.venue[2] == R"({"id":"7","op":"order","args":[{"instId":"BTC-USDT-SWAP","tdMode":"cross",)"
                          R"("side":"buy","ordType":"limit","px":"6421.5","sz":"10","clOrdId":"7","instIdCode":10459}]})");

    // An account's `orders` channel carries every instrument it trades. The session answers the engine for its own and
    // counts the rest, because a fill booked against the wrong symbol is a position nobody can explain.
    session.on_frame(R"({"arg":{"channel":"orders"},"data":[)"
                     R"({"clOrdId":"14","instId":"BTC-USDT-SWAP","state":"partially_filled",)"
                     R"("fillPx":"66664.04","accFillSz":"0.45","fillFee":"-0.14999409"},)"
                     R"({"clOrdId":"15","instId":"ETH-USDT-SWAP","state":"filled","fillPx":"3000","accFillSz":"1"}]})");
    assert(sink.engine.size() == 1);
    assert(sink.engine[0] == "FILL 14 66664.04 4500 0.14999409");
    assert(session.counters().other_instruments == 1);

    // A drop is a new session at the venue: the next connection logs in again and subscribes again, and the counters
    // are what say the reconnect path ran rather than the first connection lasting.
    session.on_disconnected();
    assert(session.counters().reconnects == 1);
    session.on_connected(1794528100);
    assert(sink.venue.size() == 4);
    const Json second_login = parse(sink.venue[3]);
    assert(login_is_signed(second_login, secret));
    assert(field(second_login.find("args")->items.front(), "timestamp") == "1794528100");
    assert(field(second_login.find("args")->items.front(), "sign") !=
           "WIUsY7IKdr05lNBq2VgPKZu5ME3ZgT1i6w4ixCiJ9W8=");
    session.on_frame(R"({"event":"login","code":"0"})");
    assert(sink.venue.size() == 5);
    assert(sink.venue[4] == R"({"op":"subscribe","args":[{"channel":"orders","instType":"ANY"}]})");
    assert((session.counters().logins == 2 && session.counters().subscribes == 2));

    // The keepalive is a cadence, not a frame per event: nothing at nineteen seconds, the channel's own text ping at
    // twenty -- the literal `ping`, because the object form is refused by the venue as `60012`.
    assert(!session.on_tick(1794528119));
    assert(session.on_tick(1794528120));
    assert(sink.venue.back() == "ping");
    assert(!session.on_tick(1794528120));

    // A message the session cannot read is counted rather than answered, and the summary names every counter the relay
    // prints when the engine hangs up.
    session.on_frame("{not json");
    assert(session.counters().unreadable == 1);
    const std::string summary = session.summary();
    assert(summary.find("logins=2") != std::string::npos);
    assert(summary.find("subscribes=2") != std::string::npos);
    assert(summary.find("reconnects=1") != std::string::npos);
    assert(summary.find("other_instruments=1") != std::string::npos);
    assert(summary.find("unreadable=1") != std::string::npos);

    // ---- the transport: the same machine on a loopback socket, through the relay's own frames
    //
    // Everything above is the state machine. This half is what ADR 0006's third slice adds beside it: a connection that
    // comes up, is read on a timeout, answers a heartbeat, and reconnects. The venue is this test's own stand-in, which
    // is why the login's signature is checked by a peer that holds the secret rather than by the code that built it.
    StandInVenue venue(secret);
    RecordingPort port;
    venue_order_wire::WireConfig wire;
    wire.venue_url = "ws://127.0.0.1:" + std::to_string(venue.port());
    wire.inst_code = 10459;
    wire.mapping = OrderMapping{};
    // A drop has to be answered inside a test rather than after the first second of the production schedule.
    wire.backoff_seconds = 0.05;
    // The frame dump, which the assertions below read: both directions recorded and the login's key, passphrase and
    // signature masked, so a file that explains a silent session is never the copy of the key that leaked.
    wire.dump_path = "/tmp/toyquant_order_dump_test.txt";
    std::remove(wire.dump_path.c_str());
    venue_order_wire::VenueOrderConnection connection(
        "BTC-USDT-SWAP", OrderSession::Credentials{.api_key = "key-1", .secret = secret, .passphrase = "pass-1"},
        wire, port);
    std::thread runner([&connection] { connection.run(); });

    // The order is submitted before the venue's login has come back, which is the race the relay's own route meets: the
    // engine's connection is accepted first. The connection has to hold the request rather than drop it, so this line is
    // also the assertion that `write_failures` is zero at the end.
    connection.on_engine_line("ORDER 7 BTC-USDT-SWAP B 6421.5 100000 Gtc");

    // The login crosses as a masked frame and the stand-in recomputes its signature from the secret it holds, so this is
    // `venue_auth` checked by the peer that would refuse a wrong preimage -- the same check as above, one layer down.
    assert(wait_for([&venue] { return venue.logins() >= 1; }));
    assert(venue.login_signature_matched());

    // An order from the engine becomes the venue's request, and the fill the channel pushes comes back as the
    // translator's line. The size is 100,000 engine units, which is ten contracts of 0.01 at a scale of a million, and
    // the reply is in engine units again: the same conversion the sandbox run measured, now across two threads.
    assert(wait_for([&venue] { return venue.orders() >= 1; }));
    assert(wait_for([&port] { return port.contains("FILL 7 "); }));
    assert(port.contains("FILL 7 6421.5 10000 0.1"));
    assert(venue.subscribed());
    assert(venue.subscribes() == 1);

    // A line the translator refuses is answered rather than dropped, because a submission nobody answered is a strategy
    // that waits out its own timeout. One engine unit is a ten-thousandth of a contract.
    connection.on_engine_line("ORDER 9 BTC-USDT-SWAP B 6421.5 1 Gtc");
    assert(wait_for([&port] { return port.contains("REJECT 9 "); }));
    assert(connection.counters().refused.load() == 1);

    // The venue drops the socket. The client has to treat that as a new session -- a login and a subscription on a new
    // connection -- and the stand-in only counts a second login if it arrives on one.
    venue.drop_next_session();
    assert(wait_for([&venue] { return venue.logins() >= 2; }));
    assert(venue.subscribes() >= 2);
    assert(venue.login_signature_matched());
    // The second session's subscription pushed its own fill as well, and the translator deduplicated it: the assertion is
    // that it was pushed, not that a second FILL line reached the engine.
    assert(wait_for([&venue] { return venue.fills() >= 2; }));

    connection.stop();
    runner.join();
    assert(connection.session().counters().logins == 2);
    assert(connection.session().counters().reconnects == 1);
    assert(connection.counters().sessions.load() == 2);
    assert(connection.counters().write_failures.load() == 0);

    // What the dump holds: both directions, and no credential. The negative assertion is the one that matters -- the api
    // key this test sends must appear nowhere in the file, because the file is what a reader opens to find out why a
    // session heard nothing.
    const std::string dumped = read_file(wire.dump_path);
    assert(dumped.find("\"dir\": \"request\"") != std::string::npos);
    assert(dumped.find("\"dir\": \"venue\"") != std::string::npos);
    assert(dumped.find("apiKey") != std::string::npos);
    assert(dumped.find("***") != std::string::npos);
    assert(dumped.find("key-1") == std::string::npos);
    assert(dumped.find("pass-1") == std::string::npos);

    // ---- slice four: placement over REST, state over the same private channel
    //
    // This venue answers `50026` to its websocket trade operation and places over REST, so the order leg is two
    // connections: this placement surface, and the `orders` subscription above it. The body is the translator's own
    // `args[0]`, which is what makes the two transports one translation rather than two.
    StandInVenue state(secret);
    StandInRest placement(secret);
    RecordingPort rest_port;
    venue_order_wire::WireConfig rest_wire;
    rest_wire.venue_url = "ws://127.0.0.1:" + std::to_string(state.port());
    rest_wire.rest_url = "http://127.0.0.1:" + std::to_string(placement.port());
    rest_wire.inst_code = 10459;
    rest_wire.backoff_seconds = 0.05;
    // The scheduled drop is the hook a live run uses to exercise the reconnect path the venue has no reason to exercise:
    // a fifth of a second into each private session, so the REST assertions below run beside a channel that keeps coming
    // back. The stand-in counts the logins, and that counter is atomic, so the wait does not read a session mid-write.
    rest_wire.drop_after_seconds = 0.2;
    venue_order_wire::VenueOrderConnection resting(
        "BTC-USDT-SWAP", OrderSession::Credentials{.api_key = "key-1", .secret = secret, .passphrase = "pass-1"},
        rest_wire, rest_port);
    std::thread rest_runner([&resting] { resting.run(); });

    // The acknowledgement is the venue's own reply, and its `sz` is in contracts: ten of them are 100,000 engine units.
    resting.on_engine_line("ORDER 11 BTC-USDT-SWAP B 6421.5 100000 Gtc");
    assert(wait_for([&rest_port] { return rest_port.contains("ACK 11 100000"); }));
    assert(placement.signature_matched());  // recomputed, over that body and that ISO-millisecond timestamp
    assert(placement.carried_the_demo_header());
    assert(placement.last_body() ==
           R"({"instId":"BTC-USDT-SWAP","tdMode":"cross","side":"buy","ordType":"limit","px":"6421.5",)"
           R"("sz":"10","clOrdId":"11","instIdCode":10459})");
    assert(resting.counters().placed.load() == 1);

    // A close goes over the same placement route as a market reduce-only request, and its acknowledgement is the
    // units the engine asked to close -- 9,500, the fractional position -- not the venue's rounded-up contract.
    resting.on_engine_line("CLOSE 26 BTC-USDT-SWAP S 9500");
    assert(wait_for([&rest_port] { return rest_port.contains("ACK 26 9500"); }));
    assert(placement.last_body() ==
           R"({"instId":"BTC-USDT-SWAP","tdMode":"cross","side":"sell","ordType":"market","sz":"1","reduceOnly":true,"clOrdId":"26","instIdCode":10459})");

    // A refusal is a sub-code with the call itself failed, answered as a REJECT rather than as a silence -- and it is the
    // venue's refusal, not a transport failure.
    resting.on_engine_line("ORDER 12 BTC-USDT-SWAP B 6421.5 10000000 Gtc");
    assert(wait_for([&rest_port] { return rest_port.contains("REJECT 12 "); }));
    assert(resting.counters().venue_refusals.load() == 1);
    assert(resting.counters().transport_failures.load() == 0);

    // A cancel is the other request the route serves, and its REST answer is the request being accepted rather than a
    // second ending: the line reaches the engine from the venue's own state change on the channel, and the route
    // writes nothing. Measured 2026-11-18: 31 of a 60 s session's 37 cancels arrived twice before this.
    resting.on_engine_line("CANCEL 11");
    assert(wait_for([&placement]
                    { return placement.last_body().find("\"clOrdId\":\"11\"") != std::string::npos; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!rest_port.contains("CANCELED 11 user"));
    state.push_cancel("11");
    assert(wait_for([&rest_port] { return rest_port.contains("CANCELED 11 user"); }));

    // Three answer shapes only a venue or a proxy in front of one produces. A chunked body has to be read as the
    // acknowledgement it is -- the path exists and no test reached it before this one -- and a body that is not JSON or a
    // connection closed with no answer are `REJECT`s *counted as transport failures*, not silences and not venue refusals.
    resting.on_engine_line("ORDER 21 BTC-USDT-SWAP B 6421.5 100000 Gtc");
    assert(wait_for([&rest_port] { return rest_port.contains("ACK 21 100000"); }));
    resting.on_engine_line("ORDER 22 BTC-USDT-SWAP B 6421.5 100000 Gtc");
    assert(wait_for([&rest_port] { return rest_port.contains("REJECT 22 venue"); }));
    resting.on_engine_line("ORDER 23 BTC-USDT-SWAP B 6421.5 100000 Gtc");
    assert(wait_for([&rest_port] { return rest_port.contains("REJECT 23 venue"); }));
    assert(resting.counters().transport_failures.load() == 2);
    assert(resting.counters().placed.load() == 3);  // the two accepted placements and the close

    // The two queries a long-lived session needs: which orders the venue still holds, and what it says the account is
    // worth. Both are GETs on a signed path, and the stand-in recomputes the signature over the method and the query
    // string it received -- so `signature_matched` after each one is the check that the preimage is what the venue
    // verifies, not what this client hoped it was.
    resting.on_engine_line("PENDING");
    assert(wait_for([&rest_port] { return rest_port.contains("PENDING 2 11 12"); }));
    assert(placement.signature_matched());
    resting.on_engine_line("EQUITY");
    // The USDT equity, not the account-wide totalEq: the stand-in's answer carries 80,086.85 there and 4,986.73 for
    // USDT, which is the currency the strategy trades. Reading `totalEq` would pass 80,086.85 through and hide the
    // spot holdings the ledger does not own.
    assert(wait_for([&rest_port] { return rest_port.contains("EQUITY 4986.73"); }));
    assert(placement.signature_matched());
    // The quantity, on the same signed REST surface: one contract short is -10,000 engine units, and the sign is what
    // makes the number comparable with the ledger's position rather than with a contract count. It is stated with the
    // convention that produced it -- 1,000,000 engine units per coin, of BTC -- so the engine can check it against its
    // own instrument instead of assuming the two processes count the same unit.
    resting.on_engine_line("POSITION");
    assert(wait_for([&rest_port] { return rest_port.contains("POSITION -10000 1000000 BTC"); }));
    assert(placement.signature_matched());
    assert(resting.counters().pending_queries.load() == 1);
    assert(resting.counters().equity_queries.load() == 1);
    assert(resting.counters().query_failures.load() == 0);

    // A 429 is this client's budget rather than the venue's judgment of the order, so it is retried and counted
    // separately: two refusals, then the same acknowledgement the other orders got.
    resting.on_engine_line("ORDER 24 BTC-USDT-SWAP B 6421.5 100000 Gtc");
    assert(wait_for([&rest_port] { return rest_port.contains("ACK 24 100000"); }));
    assert(placement.rate_limit_hits() == 2);
    assert(resting.counters().rest_429.load() == 2);

    // A venue's placement reply carries no `sz` (the demo's is `ordId`, `clOrdId`, `sCode`, `sMsg`), so the
    // acknowledgement has to be the size this client sent. Reading the missing field gave `ACK 25 0` against the live
    // venue; the stand-in's answer here is that shape.
    resting.on_engine_line("ORDER 25 BTC-USDT-SWAP B 6421.5 100000 Gtc");
    assert(wait_for([&rest_port] { return rest_port.contains("ACK 25 100000"); }));

    // The state half is still the socket: the subscription pushed a fill, which is the half REST cannot carry.
    assert(wait_for([&state] { return state.fills() >= 1; }));
    assert(wait_for([&rest_port] { return rest_port.contains("FILL 7 "); }));
    // ... and the private channel has come back more than once meanwhile: the scheduled drop closed each session after a
    // fifth of a second, and each one had to log in again (`logins` counts what the stand-in received).
    assert(wait_for([&state] { return state.logins() >= 3; }));

    resting.stop();
    rest_runner.join();
    // Ten POSTs (the rate-limited one is three) and three GETs: the pending list, the balance and the position.
    assert(placement.requests() == 14);  // the close's placement is one of them
    assert(resting.counters().write_failures.load() == 0);
    // The scheduled drop ran: every session it closed was a new login at the venue, which is the reconnect path a live
    // run takes when the venue drops the private channel.
    assert(resting.session().counters().reconnects >= 1);
    assert(resting.session().counters().logins >= 2);

    // The cost counters a kept-alive connection would have to beat: one call per placement, and the handshake inside the
    // call. The timing itself cannot be asserted here -- the stand-in answers in microseconds -- but the relationship can:
    // a placement contains its connection, and the summary states both averages beside the count.
    const std::string wire_summary = resting.counters().to_log_string();
    assert(wire_summary.find("rest_calls=14") != std::string::npos);
    assert(wire_summary.find("rest_429=2") != std::string::npos);
    assert(wire_summary.find("pending_queries=1") != std::string::npos);
    assert(wire_summary.find("equity_queries=1") != std::string::npos);
    assert(wire_summary.find("rest_connect_ms_avg=") != std::string::npos);
    assert(resting.counters().rest_place_ms.load() >= resting.counters().rest_connect_ms.load());

    // The per-endpoint budget: one request per hundred milliseconds, so the second placement waits for a token rather
    // than reaching the venue. The bucket is per path, which is why this is one endpoint and not a global rate.
    {
        StandInRest limited(secret);
        RecordingPort limited_port;
        venue_order_wire::WireConfig limited_wire;
        limited_wire.venue_url = "ws://127.0.0.1:" + std::to_string(state.port());
        limited_wire.rest_url = "http://127.0.0.1:" + std::to_string(limited.port());
        limited_wire.inst_code = 10459;
        limited_wire.backoff_seconds = 0.05;
        limited_wire.rest_limits = venue_order_wire::RestLimits{.requests_per_window = 1, .window_ms = 100};
        venue_order_wire::VenueOrderConnection throttled(
            "BTC-USDT-SWAP", OrderSession::Credentials{.api_key = "key-1", .secret = secret, .passphrase = "pass-1"},
            limited_wire, limited_port);
        std::thread throttled_runner([&throttled] { throttled.run(); });
        const auto started = std::chrono::steady_clock::now();
        throttled.on_engine_line("ORDER 31 BTC-USDT-SWAP B 6421.5 100000 Gtc");
        assert(wait_for([&limited_port] { return limited_port.contains("ACK 31 "); }));
        throttled.on_engine_line("ORDER 32 BTC-USDT-SWAP B 6421.5 100000 Gtc");
        assert(wait_for([&limited_port] { return limited_port.contains("ACK 32 "); }));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        assert(elapsed >= 80);
        assert(limited.requests() == 2);  // the venue never saw the wait
        throttled.stop();
        throttled_runner.join();
    }

    return 0;
}
