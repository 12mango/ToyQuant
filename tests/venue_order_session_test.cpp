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
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
        // Two connections, because the drop the test asks for is a *new session* at the venue: the first one is answered
        // and closed, and the second has to be logged in and subscribed again.
        if (auto first = accept()) serve_session(*first, true);
        drop_.store(false);
        if (auto second = accept()) serve_session(*second, false);
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

    // The keepalive is a cadence, not a frame per event: nothing at nineteen seconds, a ping at twenty.
    assert(!session.on_tick(1794528119));
    assert(session.on_tick(1794528120));
    assert(sink.venue.back() == R"({"op":"ping"})");
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



    return 0;
}
