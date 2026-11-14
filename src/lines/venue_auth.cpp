#include "lines/venue_auth.h"

#include <vector>

namespace
{
// FIPS 180-4's first 32 bits of the fractional parts of the cube roots of the first 64 primes.
constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotate_right(uint32_t value, int places)
{
    return (value >> places) | (value << (32 - places));
}

std::string hex_of(const std::array<uint8_t, 32>& digest)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const uint8_t byte : digest)
    {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}
}  // namespace

namespace venue_auth
{
std::array<uint8_t, 32> sha256(std::string_view message)
{
    std::vector<uint8_t> data(message.begin(), message.end());
    // Padding: a one bit, zeros to 56 modulo 64, then the length in bits as a big-endian 64-bit word.
    const uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0x00);
    for (int shift = 56; shift >= 0; shift -= 8)
        data.push_back(static_cast<uint8_t>((bit_length >> shift) & 0xff));

    // The eight working words: the first 32 bits of the fractional parts of the square roots of the first eight primes.
    uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (std::size_t offset = 0; offset < data.size(); offset += 64)
    {
        uint32_t words[64] = {};
        for (int index = 0; index < 16; ++index)
        {
            const std::size_t at = offset + static_cast<std::size_t>(index) * 4;
            words[index] = (static_cast<uint32_t>(data[at]) << 24) |
                           (static_cast<uint32_t>(data[at + 1]) << 16) |
                           (static_cast<uint32_t>(data[at + 2]) << 8) |
                           static_cast<uint32_t>(data[at + 3]);
        }
        for (int index = 16; index < 64; ++index)
        {
            const uint32_t s0 = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^
                                (words[index - 15] >> 3);
            const uint32_t s1 = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^
                                (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
        uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
        for (int index = 0; index < 64; ++index)
        {
            const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + choose + kRoundConstants[index] + words[index];
            const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::array<uint8_t, 32> digest{};
    for (int index = 0; index < 8; ++index)
    {
        digest[static_cast<std::size_t>(index) * 4 + 0] = static_cast<uint8_t>(hash[index] >> 24);
        digest[static_cast<std::size_t>(index) * 4 + 1] = static_cast<uint8_t>(hash[index] >> 16);
        digest[static_cast<std::size_t>(index) * 4 + 2] = static_cast<uint8_t>(hash[index] >> 8);
        digest[static_cast<std::size_t>(index) * 4 + 3] = static_cast<uint8_t>(hash[index]);
    }
    return digest;
}

std::string sha256_hex(std::string_view message)
{
    return hex_of(sha256(message));
}

std::string hmac_sha256(std::string_view key, std::string_view message)
{
    // RFC 2104: a key longer than the block is hashed first, then zero-padded to 64 bytes and used as both pads.
    std::string block(key);
    if (block.size() > 64)
    {
        const std::array<uint8_t, 32> digest = sha256(block);
        block.assign(digest.begin(), digest.end());
    }
    block.resize(64, '\0');

    std::string inner(64, '\0');
    std::string outer(64, '\0');
    for (std::size_t index = 0; index < 64; ++index)
    {
        inner[index] = static_cast<char>(block[index] ^ 0x36);
        outer[index] = static_cast<char>(block[index] ^ 0x5c);
    }
    const std::array<uint8_t, 32> inner_digest = sha256(inner + std::string(message));
    // The outer hash is the second half of the construction: H((K ^ opad) || H((K ^ ipad) || message)). Returning
    // the two halves concatenated instead passes a length check and fails every vector.
    const std::array<uint8_t, 32> outer_digest =
        sha256(std::string(outer) + std::string(inner_digest.begin(), inner_digest.end()));
    return std::string(outer_digest.begin(), outer_digest.end());
}

std::string base64(std::string_view bytes)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 3 <= bytes.size())
    {
        const uint32_t chunk = (static_cast<uint32_t>(static_cast<uint8_t>(bytes[index])) << 16) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[index + 1])) << 8) |
                               static_cast<uint32_t>(static_cast<uint8_t>(bytes[index + 2]));
        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
        out.push_back(kAlphabet[chunk & 0x3f]);
        index += 3;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1)
    {
        const uint32_t chunk = static_cast<uint32_t>(static_cast<uint8_t>(bytes[index])) << 16;
        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.append("==");
    }
    else if (remaining == 2)
    {
        const uint32_t chunk = (static_cast<uint32_t>(static_cast<uint8_t>(bytes[index])) << 16) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[index + 1])) << 8);
        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

std::string login_signature(std::string_view timestamp, std::string_view secret)
{
    const std::string message = std::string(timestamp) + "GET" + "/users/self/verify";
    return base64(hmac_sha256(secret, message));
}

std::string rest_signature(std::string_view timestamp, std::string_view secret, std::string_view method,
                           std::string_view path, std::string_view body)
{
    const std::string message = std::string(timestamp) + std::string(method) + std::string(path) +
                                std::string(body);
    return base64(hmac_sha256(secret, message));
}
}  // namespace venue_auth
