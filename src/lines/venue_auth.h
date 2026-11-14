#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// The venue's two signatures, written here rather than reached for, because they are the one part of a live session a
// peer can check and this tree cannot: a wrong preimage or a wrong encoding produces a login the venue refuses, and
// nothing inside the process would notice. `tests/venue_auth_test` therefore compares them against vectors computed
// elsewhere -- RFC 4231's for HMAC and the relay's own pinned login for the venue's preimage -- rather than against a
// second call of the same code.
//
// SHA-256 and base64 are hand-written for the same reason `src/market/ws_feed.cpp` writes SHA-1 and base64 for the
// opening handshake: linking OpenSSL into the engine's line library would make every build depend on it, and CMake keeps
// that dependency on the relay target, where a `wss://` venue needs it anyway.
namespace venue_auth
{
// The digest behind both signatures. Exposed because the vectors are hex, and a test that could only call the whole
// signature could say it failed but not which half.
std::array<uint8_t, 32> sha256(std::string_view message);
std::string sha256_hex(std::string_view message);
std::string hmac_sha256(std::string_view key, std::string_view message);
std::string base64(std::string_view bytes);

// The venue's login: base64(HMAC-SHA256(secret, timestamp + "GET" + "/users/self/verify")). The timestamp is Unix epoch
// *seconds* as a string, and the signature is computed over that exact string, so the two have to be produced together.
// The path is not a request anyone makes -- the signature is the request -- which is why it is a constant and not an
// argument.
std::string login_signature(std::string_view timestamp, std::string_view secret);

// A REST call: base64(HMAC-SHA256(secret, timestamp + method + path + body)), the body being the exact bytes sent. The
// timestamp here is ISO 8601 **milliseconds**, the opposite format from the login's, which is the trap the live page
// records: a client that shares one timestamp function across both is one error code away from looking like a bad key.
std::string rest_signature(std::string_view timestamp, std::string_view secret, std::string_view method,
                           std::string_view path, std::string_view body);
}  // namespace venue_auth
