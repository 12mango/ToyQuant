// Step two's second half: the relay's order channel. `tcp_venue_transport_test` proves the adapter over a socket
// the test itself stands in front of, and `venue_relay.py --self-test` pins the translation on its own. Neither
// proves the two halves talk, which is what this file does: the real adapter, the real transport and the real
// relay process, with the relay answering as a sandbox venue -- no key, no endpoint and no market.
//
// What it pins is the round trip across two processes and one translation: an order becomes the venue's message
// and comes back as an acknowledgement and a fill, a size above the sandbox cap comes back as a refusal, and a
// cancel comes back as a cancel. The counters on the engine's side agree with what crossed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lines/live_venue.h"
#include "lines/tcp_venue_transport.h"

namespace
{
// The relay, as a child process. It prints the port it bound before it accepts anything, so the test never guesses
// which numbers are free on the host -- and a relay that failed to start arrives as a line naming python3 rather
// than as a connect that times out.
class RelayProcess
{
  public:
    RelayProcess()
    {
        // 2>&1 so a missing interpreter or a traceback is a line this class can name instead of silence.
        const std::string command = std::string("python3 ") + PROJECT_ROOT_DIR +
                                    "/tools/live/venue_relay.py --simulate-orders --order-listen "
                                    "127.0.0.1:0 2>&1";
        pipe_ = ::popen(command.c_str(), "r");
        if (pipe_ == nullptr)
            throw std::runtime_error("the relay's order channel could not be started: popen failed");

        char buffer[512];
        const char* line = ::fgets(buffer, sizeof(buffer), pipe_);
        const std::string printed = line != nullptr ? std::string(line) : std::string();
        const std::string marker = "order channel listening on 127.0.0.1:";
        const std::size_t at = printed.find(marker);
        if (at == std::string::npos)
        {
            const int status = ::pclose(pipe_);
            pipe_ = nullptr;
            throw std::runtime_error(
                "the relay's order channel did not start (exit " + std::to_string(status) +
                "); it needs python3 and tools/live/venue_relay.py, and it printed: " + printed);
        }
        port_ = static_cast<uint16_t>(std::atoi(printed.c_str() + at + marker.size()));
        if (port_ == 0) throw std::runtime_error("the relay printed no port: " + printed);
    }

    RelayProcess(const RelayProcess&) = delete;
    RelayProcess& operator=(const RelayProcess&) = delete;

    ~RelayProcess()
    {
        if (pipe_ != nullptr) ::pclose(pipe_);
    }

    [[nodiscard]] uint16_t port() const { return port_; }

    // Read once the engine has hung up: the relay leaves its loop on the end of the stream, and a non-zero exit
    // here is a relay that failed rather than one that finished.
    void wait()
    {
        // Drained to the end before the stream is closed: `pclose` closes the read end before it waits, so a
        // relay still printing its summary would meet a closed pipe and exit 120 rather than 0.
        char buffer[512];
        while (::fgets(buffer, sizeof(buffer), pipe_) != nullptr)
        {
        }
        const int status = ::pclose(pipe_);
        pipe_ = nullptr;
        if (status != 0) throw std::runtime_error("the relay's order channel exited " + std::to_string(status));
    }

  private:
    FILE* pipe_{nullptr};
    uint16_t port_{0};
};

template <typename Predicate>
bool wait_for(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

exchange::Order make_order(uint64_t id, exchange::Side side, double price, uint64_t quantity)
{
    exchange::Order order;
    order.id = id;
    order.symbol = "BTC-USDT-SWAP";
    order.side = side;
    order.type = exchange::OrderType::Limit;
    order.price = price;
    order.qty = quantity;
    order.remaining = quantity;
    order.ts = 1;
    return order;
}
}  // namespace

int main()
{
    RelayProcess relay;
    TcpVenueTransport transport("127.0.0.1", relay.port());
    if (!transport.connect())
        throw std::runtime_error("the relay is listening and the transport did not connect");

    LiveVenue venue(transport);
    std::vector<ExecutionReport> reports;
    venue.set_report_callback([&reports](const ExecutionReport& report) { reports.push_back(report); });
    transport.set_line_handler([&venue](std::string_view line) { venue.on_frame(line); });

    // An order: the venue's acknowledgement, then half of it filled at the order's own price.
    venue.send_order(make_order(7, exchange::Side::Buy, 6421.5, 10));
    if (!wait_for([&reports] { return reports.size() >= 2; }))
        throw std::runtime_error("the order channel never answered the order, so the relay and the adapter "
                                 "disagree about the protocol");
    if (reports[0].order_id != 7 || reports[0].exec_type != ExecType::Resting || reports[0].quantity != 10)
        throw std::runtime_error("the acknowledgement is not the one the sandbox venue sent");
    if (reports[1].exec_type != ExecType::Trade || reports[1].price != 6421.5 || reports[1].quantity != 5)
        throw std::runtime_error("the fill is not the one the sandbox venue sent");

    // Above the sandbox's cap: a refusal, which is a different report rather than a smaller fill.
    venue.send_order(make_order(8, exchange::Side::Buy, 6421.0, 1000));
    if (!wait_for([&reports] { return reports.size() >= 3; }))
        throw std::runtime_error("the sandbox venue never refused the oversized order");
    if (reports[2].order_id != 8 || reports[2].exec_type != ExecType::Rejected)
        throw std::runtime_error("an order above the sandbox cap was not refused");

    // A cancel of the order that is still resting.
    venue.cancel_order(7);
    if (!wait_for([&reports] { return reports.size() >= 4; }))
        throw std::runtime_error("the sandbox venue never answered the cancel");
    if (reports[3].order_id != 7 || reports[3].exec_type != ExecType::Cancelled ||
        reports[3].cancel_reason != CancelReason::User)
        throw std::runtime_error("the cancel did not come back as a user cancel");

    // What crossed: three frames out (two orders and a cancel) and four lines in. The adapter's counters and the
    // transport's have to agree, because a frame counted twice is a protocol that would double a position.
    if (transport.frames_sent() != 3) throw std::runtime_error("the transport sent a different number of frames");
    if (transport.frames_received() != 4)
        throw std::runtime_error("the transport received a different number of lines");
    if (venue.counters().orders_sent != 2 || venue.counters().cancels_sent != 1)
        throw std::runtime_error("the adapter counted a different order flow");
    if (venue.counters().reports_decoded != 4 || venue.counters().frames_dropped != 0)
        throw std::runtime_error("the adapter dropped or miscounted a reply");

    transport.disconnect();
    relay.wait();  // the relay leaves on the end of the stream; failing to exit is a failure of this test
    return 0;
}
