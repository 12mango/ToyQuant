#include "lines/tcp_venue_transport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
// One attempt at an address, so a host that resolves to several is tried in order rather than abandoned.
int connect_to(const addrinfo* address)
{
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) return -1;
    if (::connect(fd, address->ai_addr, address->ai_addrlen) != 0)
    {
        ::close(fd);
        return -1;
    }
    int one = 1;
    // A frame is a request, not a stream: waiting for the previous one to be acknowledged would put a round
    // trip between two orders the strategy decided in the same event.
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}
}  // namespace

TcpVenueTransport::TcpVenueTransport(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port)
{
}

TcpVenueTransport::~TcpVenueTransport()
{
    disconnect();
}

bool TcpVenueTransport::connect()
{
    if (socket_.load() >= 0) return true;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port_);
    if (::getaddrinfo(host_.c_str(), service.c_str(), &hints, &resolved) != 0) return false;

    int fd = -1;
    for (const addrinfo* address = resolved; address != nullptr && fd < 0; address = address->ai_next)
        fd = connect_to(address);
    ::freeaddrinfo(resolved);
    if (fd < 0) return false;

    socket_.store(fd);
    running_.store(true);
    reader_ = std::thread(&TcpVenueTransport::receive_loop, this);
    return true;
}

void TcpVenueTransport::disconnect()
{
    // Joined whenever the thread exists, not only when it is still running: a reader that already saw the end of
    // the stream has exited but is still joinable, and destroying a joinable std::thread terminates the process.
    running_.store(false);
    const int fd = socket_.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (reader_.joinable()) reader_.join();
    if (fd >= 0) ::close(fd);
}

bool TcpVenueTransport::send(std::string_view frame)
{
    if (!running_.load(std::memory_order_relaxed)) return false;
    const int fd = socket_.load(std::memory_order_relaxed);
    if (fd < 0) return false;

    std::string line(frame);
    line.push_back('\n');
    std::size_t written = 0;
    while (written < line.size())
    {
        const ssize_t sent = ::send(fd, line.data() + written, line.size() - written, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            if (sent < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            running_.store(false);
            return false;
        }
        written += static_cast<std::size_t>(sent);
    }
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TcpVenueTransport::receive_loop()
{
    std::vector<char> buffer(4096);
    std::string pending;
    while (running_.load(std::memory_order_relaxed))
    {
        const int fd = socket_.load(std::memory_order_relaxed);
        if (fd < 0) break;
        const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (got == 0)
        {
            // The venue closed the connection. The adapter is told by `running_` going false, which is what the
            // demo reports rather than pretending the socket is still there.
            running_.store(false);
            break;
        }
        if (got < 0)
        {
            if (errno == EINTR) continue;
            running_.store(false);
            break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(got));
        std::size_t newline = pending.find('\n');
        while (newline != std::string::npos)
        {
            std::string_view line(pending.data(), newline);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            frames_received_.fetch_add(1, std::memory_order_relaxed);
            if (handler_) handler_(line);
            pending.erase(0, newline + 1);
            newline = pending.find('\n');
        }
    }
}
