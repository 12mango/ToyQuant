// Step two of ADR 0005: the same adapter, over a real socket. `TcpVenueTransport` is the only thing here that
// knows a socket exists; everything below runs on loopback with a twenty-line server standing in for a venue, so
// the test proves the round trip without needing an endpoint, a credential or a market.
//
// What it pins: a frame written by the adapter arrives as one line, a line written back reaches the adapter's
// decoder and becomes a report, the counters agree with what crossed, and a connection that has gone away is
// reported rather than silently swallowed.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lines/live_venue.h"
#include "lines/tcp_venue_transport.h"

namespace
{
// A listening socket on an ephemeral loopback port, so the test never depends on a fixed port being free.
class LoopbackVenue
{
  public:
    LoopbackVenue()
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listener_ >= 0);
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        assert(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        assert(::listen(listener_, 1) == 0);
    }

    ~LoopbackVenue() { stop(); }

    [[nodiscard]] uint16_t port() const { return port_; }

    // Accept one client, read its lines, and answer the first two: an acknowledgement and then a fill.
    void serve()
    {
        thread_ = std::thread([this] {
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd < 0) return;
            client_.store(fd);
            std::string pending;
            char buffer[512];
            while (true)
            {
                const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                pending.append(buffer, static_cast<std::size_t>(got));
                std::size_t newline = pending.find('\n');
                while (newline != std::string::npos)
                {
                    received.push_back(pending.substr(0, newline));
                    pending.erase(0, newline + 1);
                    if (received.size() == 1) ::send(fd, "ACK 7 10\n", 9, MSG_NOSIGNAL);
                    if (received.size() == 2) ::send(fd, "FILL 7 6421.5 4\n", 17, MSG_NOSIGNAL);
                    newline = pending.find('\n');
                }
            }
            client_.store(-1);
            ::close(fd);
        });
    }

    // The venue hangs up on the client, which is how a session that lost its endpoint looks from the transport's
    // side. Joining first would deadlock: the serve thread is blocked reading the very socket being closed.
    void hang_up()
    {
        const int fd = client_.load();
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    }

    void stop()
    {
        hang_up();
        if (thread_.joinable()) thread_.join();
        if (listener_ >= 0) ::close(listener_);
        listener_ = -1;
    }

    std::vector<std::string> received;

  private:
    int listener_{-1};
    std::atomic<int> client_{-1};
    uint16_t port_{0};
    std::thread thread_;
};

// A bounded wait, because a socket has no notion of "by now": a test that slept a fixed time would be slow on a
// good day and flaky on a bad one.
bool wait_for(const std::function<bool()>& condition)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

exchange::Order make_order(uint64_t id, double price, uint64_t qty)
{
    exchange::Order order;
    order.id = id;
    order.symbol = "BTC-PERPETUAL";
    order.side = exchange::Side::Buy;
    order.type = exchange::OrderType::Limit;
    order.price = price;
    order.qty = qty;
    order.remaining = qty;
    order.ts = 1;
    return order;
}

}  // namespace

int main()
{
    // The round trip: what the adapter writes crosses as one line, and what the venue writes back becomes the
    // reports the ledger consumes.
    {
        LoopbackVenue venue_server;
        venue_server.serve();

        TcpVenueTransport transport("127.0.0.1", venue_server.port());
        assert(transport.connect());
        assert(transport.connected());

        LiveVenue venue(transport);
        std::vector<ExecutionReport> reports;
        venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
        transport.set_line_handler([&venue](std::string_view line) { venue.on_frame(line); });

        venue.send_order(make_order(7, 6421.5, 10));
        assert(wait_for([&venue_server] { return venue_server.received.size() >= 1; }));
        assert(venue_server.received[0] == "ORDER 7 BTC-PERPETUAL B 6421.5 10 Gtc");

        // The acknowledgement arrives only after it crossed the socket, so the assertion is about the wire and
        // not about the decoder being handed a string by the test.
        assert(wait_for([&reports] { return reports.size() >= 1; }));
        assert(reports[0].order_id == 7 && reports[0].exec_type == ExecType::Resting);
        assert(reports[0].quantity == 10);

        venue.send_order(make_order(8, 6421.0, 4));
        assert(wait_for([&venue_server] { return venue_server.received.size() >= 2; }));
        assert(wait_for([&reports] { return reports.size() >= 2; }));
        assert(reports[1].exec_type == ExecType::Trade);
        assert(reports[1].price == 6421.5 && reports[1].quantity == 4);

        // The counters on both ends agree with what crossed: two frames out, two lines in.
        assert(transport.frames_sent() == 2);
        assert(transport.frames_received() == 2);
        assert(venue.counters().orders_sent == 2);
        assert(venue.counters().reports_decoded == 2);
    }

    // The venue closes the connection. The transport must say so, and an order sent afterwards must be counted as
    // a failure rather than as a frame that went somewhere.
    {
        LoopbackVenue venue_server;
        venue_server.serve();

        TcpVenueTransport transport("127.0.0.1", venue_server.port());
        assert(transport.connect());
        LiveVenue venue(transport);
        transport.set_line_handler([&venue](std::string_view line) { venue.on_frame(line); });

        venue.send_order(make_order(1, 10.0, 1));
        assert(wait_for([&venue_server] { return venue_server.received.size() >= 1; }));

        venue_server.hang_up();  // the accepted socket is closed, so the next read sees the end of the stream
        assert(wait_for([&transport] { return !transport.connected(); }));
        assert(!transport.send("ORDER 2 BTC-PERPETUAL B 10 1 Gtc"));
    }

    // An endpoint nobody is listening on is reported as a failed connection, not as a connected transport that
    // silently sends nothing.
    {
        uint16_t free_port = 0;
        {
            LoopbackVenue throwaway;
            free_port = throwaway.port();
        }
        TcpVenueTransport transport("127.0.0.1", free_port);
        assert(!transport.connect());
        assert(!transport.connected());
    }

    return 0;
}
