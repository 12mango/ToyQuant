#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef TOYQUANT_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

// The socket half of a venue session: one URL, one plaintext-or-TLS connection, and the WebSocket frames that cross it.
// It is a header with two users -- the market leg in `venue_relay.cpp` and the order leg's session in
// `venue_order_wire.cpp` -- because a second copy would be a handshake that disagrees with the first one about a mask
// or a timeout. `src/lines/venue_json.h` is the same move for the reader, and ADR 0006 asks for it in one sentence:
// the order session runs *beside* the TLS/WebSocket code the market leg already uses.
//
// TLS is a compile-time option. With OpenSSL's headers present CMake defines TOYQUANT_HAVE_OPENSSL and a `wss://`
// venue is reached directly; without them only `ws://` is spoken and the Python relay stays the TLS terminator. Every
// target that compiles a user of this header must carry the same definition, which is why it is set on a target rather
// than on a file: the class below has OpenSSL members only in that build, and two translation units disagreeing about
// it would be an ODR violation rather than a missing symbol.
namespace venue_ws
{
// Monotonic milliseconds, for a deadline or a backoff. A venue's clock is not this one: the session's login takes
// `now_seconds()`, which is the wall clock its signature is over.
inline int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Unix epoch seconds, which is the string a login's signature is computed over.
inline std::uint64_t now_seconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

struct Url
{
    bool secure{false};
    std::string host;
    uint16_t port{0};
    std::string target{"/"};
};

inline Url parse_url(const std::string& url)
{
    Url parsed;
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        throw std::runtime_error("a venue url needs a scheme: " + url);
    // The scheme says whether the connection is TLS, and both spellings of each are accepted because the market leg
    // addresses a websocket and the order leg's placement route addresses an HTTP endpoint: `wss`/`https` are secure,
    // `ws`/`http` are not, and anything else is not a scheme this client speaks.
    const std::string name = url.substr(0, scheme);
    if (name != "ws" && name != "wss" && name != "http" && name != "https")
        throw std::runtime_error("a venue url's scheme is ws, wss, http or https, not " + name + ": " + url);
    parsed.secure = name == "wss" || name == "https";
    const std::string rest = url.substr(scheme + 3);
    const std::size_t slash = rest.find('/');
    const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    parsed.target = slash == std::string::npos ? "/" : rest.substr(slash);
    const std::size_t colon = host_port.find(':');
    parsed.host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    parsed.port = static_cast<uint16_t>(
        colon == std::string::npos ? (parsed.secure ? 443 : 80) : std::stoi(host_port.substr(colon + 1)));
    if (parsed.host.empty() || parsed.port == 0)
        throw std::runtime_error("a venue url needs a host and a port: " + url);
    return parsed;
}

// An HTTP proxy that speaks `CONNECT`. That is how a venue is reached from behind one: the client asks the proxy for a
// tunnel to `host:port` and then speaks its own protocol through it, so the proxy never sees a key and the venue's
// certificate is still checked against the venue's own name. Measured need: `www.okx.com` resolves to a link-local
// address on this host and only the operator's proxy reaches it, so a direct connection is not merely slower, it is
// nothing at all.
struct Proxy
{
    std::string host;
    uint16_t port{0};
    [[nodiscard]] bool enabled() const { return port != 0; }
};

// `host:port`, or empty for a direct connection. Named rather than guessed: a malformed value is an error, because the
// alternative is a setting the operator gave and this client silently ignored.
inline Proxy parse_proxy(const std::string& text)
{
    if (text.empty()) return Proxy{};
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("a proxy is host:port, not " + text);
    Proxy proxy;
    proxy.host = text.substr(0, colon);
    int port = 0;
    try
    {
        port = std::stoi(text.substr(colon + 1));
    }
    catch (const std::exception&)
    {
        throw std::runtime_error("a proxy is host:port, not " + text);
    }
    if (proxy.host.empty() || port <= 0 || port > 65535)
        throw std::runtime_error("a proxy is host:port, not " + text);
    proxy.port = static_cast<uint16_t>(port);
    return proxy;
}

// A peer that has gone must be a failed write rather than a signal that kills the process. The plaintext path asks for
// that per write (`MSG_NOSIGNAL`), but `SSL_write` has no such flag -- OpenSSL writes with `write(2)` -- so a connection
// the venue resets raises SIGPIPE and ends the process: measured 2026-11-21 as the relay's exit code 141 in the middle
// of answering a balance query. Ignored once, the same write returns an error the caller already handles.
inline void ignore_sigpipe()
{
    static std::once_flag once;
    std::call_once(once, [] { ::signal(SIGPIPE, SIG_IGN); });
}

// The TCP connect's own bound. The Python relay has always had one (`socket.create_connection(..., timeout=15)`) and this
// port did not: `connect(2)` against a peer that blackholes a SYN retries for minutes, and every caller sits inside it --
// a placement, a query, or a session's read loop, which is the one that must keep its clock. Ten seconds is the REST
// route's answer budget, so a venue that cannot be reached costs one deadline rather than two.
inline constexpr std::int64_t kConnectTimeoutMs = 10'000;

// True when the connection came up before the deadline. The socket is left blocking again, because the read side carries
// its own 200 ms timeout and a non-blocking socket would turn every read into a verdict the caller never asked for.
inline bool connect_within(int fd, const sockaddr* address, socklen_t length, std::int64_t deadline_ms)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    bool connected = ::connect(fd, address, length) == 0;
    if (!connected && errno == EINPROGRESS)
    {
        while (true)
        {
            const std::int64_t remaining = deadline_ms - now_ms();
            if (remaining <= 0) break;
            pollfd watched{};
            watched.fd = fd;
            watched.events = POLLOUT;
            // A wake-up every 200 ms rather than one wait for the whole budget: the loop re-reads the clock, so a
            // deadline crossed while poll sleeps is noticed on the next round instead of being rounded to it.
            const int ready = ::poll(&watched, 1, static_cast<int>(std::min<std::int64_t>(remaining, 200)));
            if (ready < 0)
            {
                if (errno == EINTR) continue;
                break;
            }
            if (ready == 0) continue;
            int error = 0;
            socklen_t size = sizeof(error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) break;
            connected = error == 0;
            break;
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    return connected;
}

// One connection, plaintext or TLS. The TLS branch is compiled only when OpenSSL's headers were found, which is the one
// thing this tree cannot assume; without it a `wss://` url is refused by name rather than half-connected. The read side
// times out after 200 ms rather than blocking, so the caller's loop keeps its clock: without that a keepalive could not
// be sent on a socket that has gone quiet, and a stop could not be noticed.
class Stream
{
   public:
    Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream() { close(); }

    // `timeout_ms` is the TCP connect's budget, and it is a parameter because a test proves the bound with 600 ms rather
    // than with the ten seconds a live session is allowed. `proxy` is where the connection is dialled when the venue is
    // only reachable through one; the name in `url` is still what the tunnel asks for and what TLS verifies.
    void connect(const Url& url, std::int64_t timeout_ms = kConnectTimeoutMs, const Proxy& proxy = Proxy{})
    {
        if (url.secure)
        {
#ifdef TOYQUANT_HAVE_OPENSSL
            OPENSSL_init_ssl(0, nullptr);
#else
            throw std::runtime_error(
                "this build has no TLS: pass a ws:// url, or build with OpenSSL's headers");
#endif
        }
        // A proxy is dialled instead of the venue; the venue's own name is still what the tunnel asks for and what TLS
        // verifies, so a proxy never becomes the authority on where the connection went.
        const std::string dial_host = proxy.enabled() ? proxy.host : url.host;
        const std::string port = std::to_string(proxy.enabled() ? proxy.port : url.port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(dial_host.c_str(), port.c_str(), &hints, &result) != 0)
            throw std::runtime_error("could not resolve " + dial_host);
        // One deadline for the whole address list: a host with four records that all blackhole would otherwise cost four
        // budgets, and the caller's own timeout is stated in seconds, not in addresses.
        const std::int64_t deadline = now_ms() + timeout_ms;
        bool expired = false;
        for (addrinfo* address = result; address != nullptr; address = address->ai_next)
        {
            fd_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd_ < 0) continue;
            if (connect_within(fd_, address->ai_addr, address->ai_addrlen, deadline)) break;
            ::close(fd_);
            fd_ = -1;
            if (now_ms() >= deadline)
            {
                expired = true;
                break;
            }
        }
        freeaddrinfo(result);
        if (fd_ < 0)
            throw std::runtime_error("could not connect to " + dial_host + ":" + port +
                                     (expired ? " within " + std::to_string(timeout_ms) + " ms" : ""));

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200'000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // On the plain connection and before TLS: the proxy is asked for a tunnel to the venue's name.
        if (proxy.enabled()) tunnel_through(url);

#ifdef TOYQUANT_HAVE_OPENSSL
        if (url.secure)
        {
            ctx_ = SSL_CTX_new(TLS_client_method());
            if (ctx_ == nullptr) throw std::runtime_error("could not create a TLS context");
            SSL_CTX_set_default_verify_paths(ctx_);
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
            ssl_ = SSL_new(ctx_);
            SSL_set_tlsext_host_name(ssl_, url.host.c_str());
            SSL_set_fd(ssl_, fd_);
            // The socket's 200 ms receive timeout is what lets a caller's loop keep its clock, and a TLS handshake that
            // reads through it returns `WANT_READ` rather than failing: retried against a deadline of its own, the same
            // budget the WebSocket handshake uses.
            const std::int64_t deadline = now_ms() + 15'000;
            while (true)
            {
                const int result = SSL_connect(ssl_);
                if (result == 1) break;
                const int error = SSL_get_error(ssl_, result);
                if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) && now_ms() < deadline) continue;
                const unsigned long code = ERR_get_error();
                throw std::runtime_error(
                    std::string("TLS handshake failed: ") +
                    (code != 0 ? ERR_error_string(code, nullptr)
                               : error == SSL_ERROR_SYSCALL ? "the connection was closed during the handshake"
                                                            : "the handshake did not complete in fifteen seconds"));
            }
        }
#endif
    }

    // >0 bytes, 0 closed, -1 error, -2 a receive timeout.
    int read(void* buffer, std::size_t count)
    {
        if (!pushed_back_.empty())
        {
            // Bytes the tunnel's answer carried past its header: they are the far end's, not the proxy's.
            const std::size_t taken = std::min(count, pushed_back_.size());
            std::memcpy(buffer, pushed_back_.data(), taken);
            pushed_back_.erase(0, taken);
            return static_cast<int>(taken);
        }
#ifdef TOYQUANT_HAVE_OPENSSL
        if (ssl_ != nullptr)
        {
            const int got = SSL_read(ssl_, buffer, static_cast<int>(count));
            if (got > 0) return got;
            const int error = SSL_get_error(ssl_, got);
            if (error == SSL_ERROR_ZERO_RETURN) return 0;
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return -2;
            return -1;
        }
#endif
        const ssize_t got = ::recv(fd_, buffer, count, 0);
        if (got > 0) return static_cast<int>(got);
        if (got == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        return -1;
    }

    bool write_all(std::string_view data)
    {
        std::size_t done = 0;
        while (done < data.size())
        {
#ifdef TOYQUANT_HAVE_OPENSSL
            if (ssl_ != nullptr)
            {
                const int sent = SSL_write(ssl_, data.data() + done,
                                           static_cast<int>(data.size() - done));
                if (sent <= 0) return false;
                done += static_cast<std::size_t>(sent);
                continue;
            }
#endif
            const ssize_t sent = ::send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL);
            if (sent <= 0) return false;
            done += static_cast<std::size_t>(sent);
        }
        return true;
    }

    // The tunnel, on the plain connection and before TLS: an HTTP request asking the proxy for the venue's own name. A
    // proxy that refuses is named with its status line, because a bare "could not connect" would send a reader looking at
    // the venue instead of at the tunnel. Nothing follows the blank line -- a proxy is silent until the client speaks --
    // so the reader stops at the header's end rather than waiting for bytes that would be tunnel data.
    void tunnel_through(const Url& url)
    {
        const std::string target = url.host + ":" + std::to_string(url.port);
        if (!write_all("CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n\r\n"))
            throw std::runtime_error("the proxy tunnel could not be requested");
        std::string answer;
        const std::int64_t deadline = now_ms() + kConnectTimeoutMs;
        while (answer.find("\r\n\r\n") == std::string::npos)
        {
            if (now_ms() >= deadline)
                throw std::runtime_error("the proxy did not answer the tunnel within " +
                                         std::to_string(kConnectTimeoutMs) + " ms");
            char buffer[512];
            const int got = read(buffer, sizeof(buffer));
            if (got > 0)
                answer.append(buffer, static_cast<std::size_t>(got));
            else if (got != -2)
                throw std::runtime_error("the proxy closed while the tunnel was being set up");
        }
        // The status line is the proxy's answer and the only part of it that matters here; a `200` is the tunnel.
        const std::size_t end = answer.find("\r\n\r\n");
        const std::string status = answer.substr(0, answer.find("\r\n"));
        if (status.find(" 200") == std::string::npos)
            throw std::runtime_error("the proxy refused the tunnel: " + status);
        // A proxy may answer and start relaying in one write, so whatever followed the header's end belongs to the tunnel
        // and is served by `read` before the socket is touched again.
        pushed_back_ = answer.substr(end + 4);
    }

    void close()
    {
#ifdef TOYQUANT_HAVE_OPENSSL
        if (ssl_ != nullptr)
        {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_ != nullptr)
        {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
#endif
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Takes over an already-accepted connection, which is how the server side gets a Stream.
    void adopt(int fd) { fd_ = fd; }

    int fd_{-1};
    // Bytes already read from the socket that belong to the caller: the tunnel's setup is an HTTP exchange, and a proxy
    // may put the far end's first bytes in the same write as its 200.
    std::string pushed_back_;
#ifdef TOYQUANT_HAVE_OPENSSL
    SSL* ssl_{nullptr};
    SSL_CTX* ctx_{nullptr};
#endif
};

// ------------------------------------------------------------------ frames and handshakes

inline std::string base64_encode(const unsigned char* data, std::size_t length)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t index = 0;
    while (index + 3 <= length)
    {
        const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16) |
                                (static_cast<uint32_t>(data[index + 1]) << 8) |
                                static_cast<uint32_t>(data[index + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        index += 3;
    }
    if (index < length)
    {
        uint32_t triple = static_cast<uint32_t>(data[index]) << 16;
        if (index + 1 < length) triple |= static_cast<uint32_t>(data[index + 1]) << 8;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(index + 1 < length ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

inline uint32_t next_mask_word()
{
    static uint32_t state = static_cast<uint32_t>(now_ms()) ^ 0x9E3779B9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// 1 a frame, 0 closed, -1 error, -2 a receive timeout.
inline int read_exact(Stream& stream, void* destination, std::size_t count)
{
    auto* out = static_cast<unsigned char*>(destination);
    std::size_t done = 0;
    while (done < count)
    {
        const int got = stream.read(out + done, count - done);
        if (got > 0)
        {
            done += static_cast<std::size_t>(got);
            continue;
        }
        return got;
    }
    return 1;
}


inline bool send_frame(Stream& stream, int opcode, std::string_view payload, bool mask)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    const std::size_t length = payload.size();
    if (length < 126)
        frame.push_back(static_cast<char>((mask ? 0x80 : 0) | length));
    else if (length < 65536)
    {
        frame.push_back(static_cast<char>((mask ? 0x80 : 0) | 126));
        frame.push_back(static_cast<char>(length >> 8));
        frame.push_back(static_cast<char>(length & 0xFF));
    }
    else
    {
        frame.push_back(static_cast<char>((mask ? 0x80 : 0) | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((static_cast<uint64_t>(length) >> shift) & 0xFF));
    }
    if (mask)
    {
        const uint32_t word = next_mask_word();
        const unsigned char key[4] = {static_cast<unsigned char>(word), static_cast<unsigned char>(word >> 8),
                                      static_cast<unsigned char>(word >> 16),
                                      static_cast<unsigned char>(word >> 24)};
        for (unsigned char byte : key) frame.push_back(static_cast<char>(byte));
        for (std::size_t index = 0; index < length; ++index)
            frame.push_back(static_cast<char>(payload[index] ^ key[index & 3]));
    }
    else
    {
        frame.append(payload);
    }
    return stream.write_all(frame);
}

inline int read_frame(Stream& stream, std::string& payload, int& opcode, bool& fin)
{
    unsigned char header[2];
    const int head = read_exact(stream, header, 2);
    if (head != 1) return head;
    fin = (header[0] & 0x80) != 0;
    opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t length = header[1] & 0x7F;
    if (length == 126)
    {
        unsigned char extended[2];
        if (read_exact(stream, extended, 2) != 1) return -1;
        length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
    }
    else if (length == 127)
    {
        unsigned char extended[8];
        if (read_exact(stream, extended, 8) != 1) return -1;
        length = 0;
        for (unsigned char byte : extended) length = (length << 8) | byte;
    }
    if (length > (uint64_t{1} << 24)) return -1;
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && read_exact(stream, mask, 4) != 1) return -1;
    payload.resize(static_cast<std::size_t>(length));
    if (length > 0 && read_exact(stream, payload.data(), static_cast<std::size_t>(length)) != 1)
        return -1;
    if (masked)
        for (uint64_t index = 0; index < length; ++index)
            payload[static_cast<std::size_t>(index)] ^= mask[index & 3];
    return 1;
}


// The client's opening handshake. The venue's answer is checked for its status line and nothing else: the accept key is
// a value this client never reads, which is what lets a stand-in venue in a test answer with a constant.
inline void client_handshake(Stream& stream, const Url& url)
{
    unsigned char key_bytes[16];
    for (unsigned char& byte : key_bytes) byte = static_cast<unsigned char>(next_mask_word());
    const std::string key = base64_encode(key_bytes, sizeof(key_bytes));
    std::string request = "GET " + url.target + " HTTP/1.1\r\nHost: " + url.host + ":" +
                          std::to_string(url.port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\n"
                          "User-Agent: Mozilla/5.0 toyquant-relay-cpp/1.0\r\n\r\n";
    if (!stream.write_all(request)) throw std::runtime_error("could not write the handshake");
    // The socket polls every 200 ms so a caller's loop keeps its clock, and that poll is not an answer: a venue that is
    // slow to reply must be waited for, which is why the handshake has a deadline of its own. Python's client waits on a
    // blocking read here, measured against fifteen seconds; this is the same budget with the poll inside it.
    std::string response;
    char byte = 0;
    const std::int64_t deadline = now_ms() + 15'000;
    while (response.find("\r\n\r\n") == std::string::npos)
    {
        const int got = stream.read(&byte, 1);
        if (got == -2)
        {
            if (now_ms() >= deadline)
                throw std::runtime_error("the venue did not answer the handshake within fifteen seconds");
            continue;
        }
        if (got != 1)
            throw std::runtime_error("the venue closed during the handshake");
        response.push_back(byte);
        if (response.size() > 16 * 1024) throw std::runtime_error("handshake response too large");
    }
    if (response.rfind("HTTP/1.1 101", 0) != 0 && response.rfind("HTTP/1.0 101", 0) != 0)
        throw std::runtime_error("the venue refused the handshake: " +
                                 response.substr(0, response.find("\r\n")));
}
}  // namespace venue_ws

