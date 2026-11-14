// The two signatures the venue checks, against vectors computed outside this tree.
//
// This is the one slice of the order session that a reader cannot check by reading: a wrong preimage or a wrong
// encoding produces a login the venue refuses, and nothing inside the process notices. So the expectations here are
// pasted constants -- RFC 4231's HMAC vectors, and the relay's pinned login signature (its secret and timestamp are
// fixed in `tools/live/venue_relay.py`, and the value was computed once, elsewhere) -- never a second call of the code
// under test, which would agree with any mistake.

#include <cassert>
#include <cstdint>
#include <string>

#include "lines/venue_auth.h"

int main()
{
    using namespace venue_auth;

    // SHA-256's own two standard answers, because the signature vectors below would pass on a consistently wrong hash
    // only if the HMAC and the base64 were wrong the same way, and this pins the digest on its own.
    assert(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Base64, including both padding shapes: an unpadded or a wrongly padded signature is a key the venue reads as
    // somebody else's.
    assert(base64("") == "");
    assert(base64("f") == "Zg==");
    assert(base64("fo") == "Zm8=");
    assert(base64("foo") == "Zm9v");
    assert(base64("foob") == "Zm9vYg==");

    // RFC 4231's first case, whose key is twenty 0x0b bytes: the exact hex is in the RFC.
    assert(hmac_sha256(std::string(20, '\x0b'), "Hi There") ==
           std::string("\xb0\x34\x4c\x61\xd8\xdb\x38\x53\x5c\xa8\xaf\xce\xaf\x0b\xf1\x2b"
                       "\x88\x1d\xc2\x00\xc9\x83\x3d\xa7\x26\xe9\x37\x6c\x2e\x32\xcf\xf7",
                       32));
    // And its sixth, whose key is longer than the block: that branch hashes the key first, and a port that skipped it
    // would pass every short-key case and fail against the venue's own secrets.
    assert(hmac_sha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") ==
           std::string("\x60\xe4\x31\x59\x1e\xe0\xb6\x7f\x0d\x8a\x26\xaa\xcb\xf5\xb7\x7f"
                       "\x8e\x0b\xc6\x21\x37\x28\xc5\x14\x05\x46\x04\x0f\x0e\xe3\x7f\x54",
                       32));

    // The venue's login preimage, against the relay's pinned vector: `timestamp + "GET" + "/users/self/verify"`, where
    // the timestamp is Unix epoch **seconds** as a string and the signature is over that exact string.
    const std::string secret = "toyquant-relay-self-test-secret";
    assert(login_signature("1794528000", secret) == "WIUsY7IKdr05lNBq2VgPKZu5ME3ZgT1i6w4ixCiJ9W8=");

    // The REST preimage is the other timestamp format and the other tail, and its vector was computed the same way, with
    // an independent implementation. The two must not agree: a client that shares one preimage across both surfaces is
    // one error code away from looking like a bad key.
    const std::string body = R"({"instId":"BTC-USDT-SWAP","clOrdId":"1","sz":"1","px":"66664.5"})";
    const std::string rest = rest_signature("2026-11-13T00:00:00.000Z", secret, "POST", "/api/v5/trade/order", body);
    assert(rest == "U30RjDGwZ5VrtgLFKE2PlWVy7getgcpm5W6TsKbjU24=");
    assert(rest != login_signature("1794528000", secret));
    // The body is part of the signature: the same call with one byte more is a different signature, which is what makes
    // a resent request detectable rather than merely unlucky.
    assert(rest_signature("2026-11-13T00:00:00.000Z", secret, "POST", "/api/v5/trade/order", body + " ") != rest);

    return 0;
}
