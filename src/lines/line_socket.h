#pragma once
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>

// The order leg's wire, in one place: a line is a message, and both of its ends are in this tree -- `TcpVenueTransport` on
// the engine's side and the relay's order channel on the other. Three copies of "write every byte" and two of "split on the
// newline" is how one end starts accepting a `\r` the other never sends, so the two primitives live here and both callers
// use them. The market leg is deliberately not a client: its messages are WebSocket frames, which is `venue_ws.h`.
namespace line_socket
{
// Every byte or false. `MSG_NOSIGNAL` because a peer that has gone must be a failed write rather than a signal that kills
// the process, and the two retryable errnos rather than a short write counted as a broken connection.
inline bool write_all(int fd, std::string_view bytes)
{
    std::size_t done = 0;
    while (done < bytes.size())
    {
        const ssize_t sent = ::send(fd, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (sent <= 0) return false;
        done += static_cast<std::size_t>(sent);
    }
    return true;
}

// One frame, which is one line: the newline is the adapter's message edge (`src/lines/live_venue.h`).
inline bool write_line(int fd, std::string_view line)
{
    std::string text(line);
    text.push_back('\n');
    return write_all(fd, text);
}

// Hands every complete line in `pending` to `on_line` and leaves the partial tail behind. A trailing '\r' is stripped,
// because a peer that writes CRLF is one this side has to read. The bytes are not copied, so `on_line` may read the view
// but must not keep it -- the buffer is compacted as soon as the line is handled.
template <typename OnLine>
void dispatch_lines(std::string& pending, OnLine&& on_line)
{
    std::size_t newline = pending.find('\n');
    while (newline != std::string::npos)
    {
        std::string_view line(pending.data(), newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        on_line(line);
        pending.erase(0, newline + 1);
        newline = pending.find('\n');
    }
}
}  // namespace line_socket
