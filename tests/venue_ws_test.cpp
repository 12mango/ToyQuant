// The connect's own bound. `venue_ws::Stream` carries three deadlines -- 200 ms on a read, fifteen seconds on the TLS
// handshake, and ten on the TCP connect -- and the third is the one this file pins, because it was the one missing: a
// peer that blackholes a SYN leaves `connect(2)` retrying for minutes, and every caller sits inside it. The Python relay
// has always bounded this call (`socket.create_connection(..., timeout=15)`); the C++ port dropped the bound.
//
// The blackhole is manufactured rather than found: a listener with a backlog of one, filled with connections nobody
// accepts, leaves further SYNs unanswered -- the same failure a filtered address produces, with no network to arrange.
//
// What it pins: a connect that cannot complete throws once its budget is spent (and says so, with the budget in the
// message), a connect that is refused fails at once rather than by deadline, and a connect that succeeds leaves a
// blocking socket a read can use -- the last one is the non-blocking flag being put back, without which every read would
// return a timeout the caller never asked for.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lines/venue_ws.h"

namespace
{
// A loopback listener. `listen(1)` is what makes the blackhole: the accept queue holds one connection, and everything
// after it is dropped on the floor until somebody accepts.
class LoopbackListener
{
  public:
    LoopbackListener()
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ >= 0);
        int reuse = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        address_.sin_family = AF_INET;
        address_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_.sin_port = 0;
        assert(::bind(fd_, reinterpret_cast<sockaddr*>(&address_), sizeof(address_)) == 0);
        assert(::listen(fd_, 1) == 0);
        socklen_t length = sizeof(address_);
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address_), &length) == 0);
    }

    ~LoopbackListener()
    {
        for (const int fd : held_) ::close(fd);
        ::close(fd_);
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    [[nodiscard]] sockaddr_in address() const { return address_; }
    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] std::string url() const
    {
        return "ws://127.0.0.1:" + std::to_string(ntohs(address_.sin_port));
    }

    // Fill the accept queue with connections this listener will never accept. The count and the caller's pause are what
    // make the blackhole: with a backlog of one, a handful of pending handshakes leaves further SYNs unanswered only
    // after the kernel's own queues have filled, which takes dozens rather than a few.
    void clog()
    {
        for (int i = 0; i < 64; ++i)
        {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            assert(fd >= 0);
            const int flags = ::fcntl(fd, F_GETFL, 0);
            assert(flags >= 0);
            assert(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0);
            ::connect(fd, reinterpret_cast<sockaddr*>(&address_), sizeof(address_));
            held_.push_back(fd);
        }
    }

  private:
    int fd_{0};
    sockaddr_in address_{};
    std::vector<int> held_;
};
// A stand-in proxy: it answers `CONNECT` the way a real one does and then speaks for the far end, so the tunnel is proved
// on loopback without a venue, a certificate or a network.
class StandInProxy
{
  public:
    explicit StandInProxy(std::string status) : status_(std::move(status))
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ >= 0);
        int reuse = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        assert(::listen(fd_, 1) == 0);
        socklen_t length = sizeof(address);
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~StandInProxy()
    {
        if (thread_.joinable()) thread_.join();
        ::close(fd_);
    }

    StandInProxy(const StandInProxy&) = delete;
    StandInProxy& operator=(const StandInProxy&) = delete;

    [[nodiscard]] uint16_t port() const { return port_; }

    // The request line, once the connection is over: joining here keeps the read free of a race, because the thread
    // writes it before it answers and the answer is what the caller waited for.
    std::string request()
    {
        if (thread_.joinable()) thread_.join();
        return request_;
    }

  private:
    void serve()
    {
        const int client = ::accept(fd_, nullptr, nullptr);
        if (client < 0) return;
        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos)
        {
            char buffer[256];
            const ssize_t got = ::recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            head.append(buffer, static_cast<std::size_t>(got));
        }
        request_ = head.substr(0, head.find("\r\n"));
        const std::string answer = status_ + "\r\n\r\n";
        ::send(client, answer.data(), answer.size(), MSG_NOSIGNAL);
        if (status_.find(" 200") != std::string::npos)
        {
            const std::string far_end = "tunnelled\n";
            ::send(client, far_end.data(), far_end.size(), MSG_NOSIGNAL);
        }
        ::close(client);
    }

    std::string status_;
    int fd_{0};
    uint16_t port_{0};
    std::string request_;
    std::thread thread_;
};
}  // namespace


int main()
{
    {
        // A full accept queue answers nothing. The premise is checked with a budget of one millisecond: if a connection
        // went through, the queue was not full and the assertion below would be measuring an empty stage.
        LoopbackListener blackhole;
        blackhole.clog();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        venue_ws::Stream probe;
        bool connected = true;
        try
        {
            probe.connect(venue_ws::parse_url(blackhole.url()), 1);
        }
        catch (const std::runtime_error&)
        {
            connected = false;
        }
        assert(!connected);

        // The bound, with a budget the test can afford. The floor is what separates a deadline from a refusal: a refused
        // connection comes back in microseconds, and a deadline spends its whole budget.
        venue_ws::Stream stream;
        const std::int64_t started = venue_ws::now_ms();
        std::string message;
        bool threw = false;
        try
        {
            stream.connect(venue_ws::parse_url(blackhole.url()), 600);
        }
        catch (const std::runtime_error& error)
        {
            threw = true;
            message = error.what();
        }
        const std::int64_t elapsed = venue_ws::now_ms() - started;
        assert(threw);
        assert(message.find("could not connect to 127.0.0.1") != std::string::npos);
        assert(message.find("within 600 ms") != std::string::npos);
        assert(elapsed >= 500);
        assert(elapsed < 2'000);
    }

    {
        // A port that is bound and not listening refuses a connection at once. The socket that holds the port stays open
        // while the connection is attempted, so nothing else can take it in between: a port that was bound and released
        // is a race, and this test lost that race once -- it connected to somebody else's port and called the answer a
        // success.
        const int held = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(held >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(::bind(held, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        assert(::getsockname(held, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        const uint16_t closed_port = ntohs(address.sin_port);
        venue_ws::Stream stream;
        const std::int64_t started = venue_ws::now_ms();
        bool threw = false;
        std::string message;
        try
        {
            stream.connect(venue_ws::parse_url("ws://127.0.0.1:" + std::to_string(closed_port)), 5'000);
        }
        catch (const std::runtime_error& error)
        {
            threw = true;
            message = error.what();
        }
        const std::int64_t elapsed = venue_ws::now_ms() - started;
        assert(threw);
        assert(message.find("could not connect to 127.0.0.1") != std::string::npos);
        assert(message.find("within") == std::string::npos);
        assert(elapsed < 1'000);
        ::close(held);
    }

    {
        // A connection that comes up is left blocking: the listener answers one line, and the read returns it. A socket
        // still in non-blocking mode would answer `EAGAIN` here and the read would time out instead.
        LoopbackListener listener;
        std::thread answering([&listener] {
            const int client = ::accept(listener.fd(), nullptr, nullptr);
            if (client < 0) return;
            const std::string line = "hello\n";
            ::send(client, line.data(), line.size(), 0);
            ::close(client);
        });
        venue_ws::Stream stream;
        stream.connect(venue_ws::parse_url(listener.url()), 2'000);
        std::string received;
        const std::int64_t deadline = venue_ws::now_ms() + 5'000;
        while (received.find("hello\n") == std::string::npos && venue_ws::now_ms() < deadline)
        {
            char buffer[64];
            const int got = stream.read(buffer, sizeof(buffer));
            if (got > 0) received.append(buffer, static_cast<std::size_t>(got));
        }
        answering.join();
        assert(received == "hello\n");
    }

    {
        // Through a proxy: the tunnel is asked for by the venue's own name, and the bytes on the connection are the far
        // end's. This is the path a venue behind a proxy needs and the one a direct dial never exercises.
        StandInProxy proxy("HTTP/1.1 200 Connection established");
        venue_ws::Stream stream;
        stream.connect(venue_ws::parse_url("ws://venue.example:8443"), 2'000,
                       venue_ws::parse_proxy("127.0.0.1:" + std::to_string(proxy.port())));
        std::string received;
        const std::int64_t deadline = venue_ws::now_ms() + 5'000;
        while (received.find("tunnelled\n") == std::string::npos && venue_ws::now_ms() < deadline)
        {
            char buffer[64];
            const int got = stream.read(buffer, sizeof(buffer));
            if (got > 0) received.append(buffer, static_cast<std::size_t>(got));
        }
        assert(received == "tunnelled\n");
        assert(proxy.request() == "CONNECT venue.example:8443 HTTP/1.1");
    }

    {
        // A proxy that refuses is named with its status: a reader told only "could not connect" would go and check the
        // venue instead of the tunnel.
        StandInProxy proxy("HTTP/1.1 403 Forbidden");
        venue_ws::Stream stream;
        std::string message;
        bool threw = false;
        try
        {
            stream.connect(venue_ws::parse_url("ws://venue.example:8443"), 2'000,
                           venue_ws::parse_proxy("127.0.0.1:" + std::to_string(proxy.port())));
        }
        catch (const std::runtime_error& error)
        {
            threw = true;
            message = error.what();
        }
        assert(threw);
        assert(message == "the proxy refused the tunnel: HTTP/1.1 403 Forbidden");
    }

    {
        // The flag's own parsing: empty is a direct connection, `host:port` is a tunnel, and anything else is named
        // rather than ignored.
        assert(!venue_ws::parse_proxy("").enabled());
        const venue_ws::Proxy parsed = venue_ws::parse_proxy("127.0.0.1:7890");
        assert(parsed.enabled());
        assert(parsed.host == "127.0.0.1");
        assert(parsed.port == 7890);
        bool threw = false;
        try
        {
            venue_ws::parse_proxy("7890");
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        assert(threw);
    }

    {
        // A peer that has gone is a failed write, not the death of the process. This is the pin for `ignore_sigpipe`:
        // the same `send` without the ignore kills the test with SIGPIPE, which is how the relay died (exit code 141)
        // while answering a balance query on 2026-11-21.
        venue_ws::ignore_sigpipe();
        int pair[2] = {-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        ::close(pair[1]);
        const ssize_t sent = ::send(pair[0], "x", 1, 0);
        assert(sent < 0);
        ::close(pair[0]);
    }

    return 0;
}
