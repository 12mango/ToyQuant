#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "lines/live_venue.h"

// The wire, for real this time: a TCP connection to a venue endpoint, one frame per line, one receive thread.
// This is step two of ADR 0005 and the only file in that work that knows a socket exists -- the adapter above
// takes `IVenueTransport` and stays ignorant, which is what made step one testable without one.
//
// POSIX, like the legacy UDP feed in this directory: the demo runs on the desk, and a second portable socket
// layer would be work with no measurement behind it.
class TcpVenueTransport : public IVenueTransport
{
  public:
    TcpVenueTransport(std::string host, uint16_t port);
    ~TcpVenueTransport() override;

    TcpVenueTransport(const TcpVenueTransport&) = delete;
    TcpVenueTransport& operator=(const TcpVenueTransport&) = delete;

    // Connects and starts the receive thread. False when the endpoint could not be reached: a caller that
    // reported success here and then sent nothing would look exactly like a venue that ignored every order.
    bool connect();
    void disconnect();

    bool send(std::string_view frame) override;

    // One line of the venue's reply, called on the receive thread. A line is the unit because the protocol is
    // line-delimited: a partial line is held until its newline arrives rather than handed on in pieces.
    using LineHandler = std::function<void(std::string_view)>;
    void set_line_handler(LineHandler handler) { handler_ = std::move(handler); }

    [[nodiscard]] uint64_t frames_sent() const { return frames_sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t frames_received() const { return frames_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool connected() const { return socket_ >= 0 && running_.load(std::memory_order_relaxed); }

  private:
    void receive_loop();

    std::string host_;
    uint16_t port_;
    std::atomic<int> socket_{-1};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> frames_received_{0};
    std::thread reader_;
    LineHandler handler_;
};
