#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef TOYQUANT_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <chrono>
#include <cerrno>
#include <cstdint>
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
    parsed.secure = url.substr(0, scheme) == "wss";
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

    void connect(const Url& url)
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
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const std::string port = std::to_string(url.port);
        if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &result) != 0)
            throw std::runtime_error("could not resolve " + url.host);
        for (addrinfo* address = result; address != nullptr; address = address->ai_next)
        {
            fd_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd_ < 0) continue;
            if (::connect(fd_, address->ai_addr, address->ai_addrlen) == 0) break;
            ::close(fd_);
            fd_ = -1;
        }
        freeaddrinfo(result);
        if (fd_ < 0) throw std::runtime_error("could not connect to " + url.host + ":" + port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200'000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

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
            if (SSL_connect(ssl_) != 1)
            {
                const unsigned long code = ERR_get_error();
                throw std::runtime_error(std::string("TLS handshake failed: ") +
                                         ERR_error_string(code, nullptr));
            }
        }
#endif
    }

    // >0 bytes, 0 closed, -1 error, -2 a receive timeout.
    int read(void* buffer, std::size_t count)
    {
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
    std::string response;
    char byte = 0;
    while (response.find("\r\n\r\n") == std::string::npos)
    {
        if (stream.read(&byte, 1) != 1)
            throw std::runtime_error("the venue closed during the handshake");
        response.push_back(byte);
        if (response.size() > 16 * 1024) throw std::runtime_error("handshake response too large");
    }
    if (response.rfind("HTTP/1.1 101", 0) != 0 && response.rfind("HTTP/1.0 101", 0) != 0)
        throw std::runtime_error("the venue refused the handshake: " +
                                 response.substr(0, response.find("\r\n")));
}
}  // namespace venue_ws

