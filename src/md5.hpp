#pragma once

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace weflow {

// Standalone RFC 1321 compliant MD5 implementation for cross-platform support.
// Removes hard dependency on macOS CommonCrypto/CommonDigest.h or OpenSSL.
namespace detail {

inline uint32_t LeftRotate(uint32_t x, uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

inline void MD5Transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) {
        x[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }

    #define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
    #define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
    #define H(x, y, z) ((x) ^ (y) ^ (z))
    #define I(x, y, z) ((y) ^ ((x) | (~z)))

    #define FF(a, b, c, d, k, s, t) { \
        (a) += F((b), (c), (d)) + (x[k]) + (t); \
        (a) = detail::LeftRotate((a), (s)); \
        (a) += (b); \
    }
    #define GG(a, b, c, d, k, s, t) { \
        (a) += G((b), (c), (d)) + (x[k]) + (t); \
        (a) = detail::LeftRotate((a), (s)); \
        (a) += (b); \
    }
    #define HH(a, b, c, d, k, s, t) { \
        (a) += H((b), (c), (d)) + (x[k]) + (t); \
        (a) = detail::LeftRotate((a), (s)); \
        (a) += (b); \
    }
    #define II(a, b, c, d, k, s, t) { \
        (a) += I((b), (c), (d)) + (x[k]) + (t); \
        (a) = detail::LeftRotate((a), (s)); \
        (a) += (b); \
    }

    // Round 1
    FF(a, b, c, d,  0,  7, 0xd76aa478);
    FF(d, a, b, c,  1, 12, 0xe8c7b756);
    FF(c, d, a, b,  2, 17, 0x242070db);
    FF(b, c, d, a,  3, 22, 0xc1bdceee);
    FF(a, b, c, d,  4,  7, 0xf57c0faf);
    FF(d, a, b, c,  5, 12, 0x4787c62a);
    FF(c, d, a, b,  6, 17, 0xa8304613);
    FF(b, c, d, a,  7, 22, 0xfd469501);
    FF(a, b, c, d,  8,  7, 0x698098d8);
    FF(d, a, b, c,  9, 12, 0x8b44f7af);
    FF(c, d, a, b, 10, 17, 0xffff5bb1);
    FF(b, c, d, a, 11, 22, 0x895cd7be);
    FF(a, b, c, d, 12,  7, 0x6b901122);
    FF(d, a, b, c, 13, 12, 0xfd987193);
    FF(c, d, a, b, 14, 17, 0xa679438e);
    FF(b, c, d, a, 15, 22, 0x49b40821);

    // Round 2
    GG(a, b, c, d,  1,  5, 0xf61e2562);
    GG(d, a, b, c,  6,  9, 0xc040b340);
    GG(c, d, a, b, 11, 14, 0x265e5a51);
    GG(b, c, d, a,  0, 20, 0xe9b6c7aa);
    GG(a, b, c, d,  5,  5, 0xd62f105d);
    GG(d, a, b, c, 10,  9, 0x02441453);
    GG(c, d, a, b, 15, 14, 0xd8a1e681);
    GG(b, c, d, a,  4, 20, 0xe7d3fbc8);
    GG(a, b, c, d,  9,  5, 0x21e1cde6);
    GG(d, a, b, c, 14,  9, 0xc33707d6);
    GG(c, d, a, b,  3, 14, 0xf4d50d87);
    GG(b, c, d, a,  8, 20, 0x455a14ed);
    GG(a, b, c, d, 13,  5, 0xa9e3e905);
    GG(d, a, b, c,  2,  9, 0xfcefa3f8);
    GG(c, d, a, b,  7, 14, 0x676f02d9);
    GG(b, c, d, a, 12, 20, 0x8d2a4c8a);

    // Round 3
    HH(a, b, c, d,  5,  4, 0xfffa3942);
    HH(d, a, b, c,  8, 11, 0x8771f681);
    HH(c, d, a, b, 11, 16, 0x6d9d6122);
    HH(b, c, d, a, 14, 23, 0xfde5380c);
    HH(a, b, c, d,  1,  4, 0xa4beea44);
    HH(d, a, b, c,  4, 11, 0x4bdecfa9);
    HH(c, d, a, b,  7, 16, 0xf6bb4b60);
    HH(b, c, d, a, 10, 23, 0xbebfbc70);
    HH(a, b, c, d, 13,  4, 0x289b7ec6);
    HH(d, a, b, c,  0, 11, 0xeaa127fa);
    HH(c, d, a, b,  3, 16, 0xd4ef3085);
    HH(b, c, d, a,  6, 23, 0x04881d05);
    HH(a, b, c, d,  9,  4, 0xd9d4d039);
    HH(d, a, b, c, 12, 11, 0xe6db99e5);
    HH(c, d, a, b, 15, 16, 0x1fa27cf8);
    HH(b, c, d, a,  2, 23, 0xc4ac5665);

    // Round 4
    II(a, b, c, d,  0,  6, 0xf4292244);
    II(d, a, b, c,  7, 10, 0x432aff97);
    II(c, d, a, b, 14, 15, 0xab9423a7);
    II(b, c, d, a,  5, 21, 0xfc93a039);
    II(a, b, c, d, 12,  6, 0x655b59c3);
    II(d, a, b, c,  3, 10, 0x8f0ccc92);
    II(c, d, a, b, 10, 15, 0xffeff47d);
    II(b, c, d, a,  1, 21, 0x85845dd1);
    II(a, b, c, d,  8,  6, 0x6fa87e4f);
    II(d, a, b, c, 15, 10, 0xfe2ce6e0);
    II(c, d, a, b,  6, 15, 0xa3014314);
    II(b, c, d, a, 13, 21, 0x4e0811a1);
    II(a, b, c, d,  4,  6, 0xf7537e82);
    II(d, a, b, c, 11, 10, 0xbd3af235);
    II(c, d, a, b,  2, 15, 0x2ad7d2bb);
    II(b, c, d, a,  9, 21, 0xeb86d391);

    #undef F
    #undef G
    #undef H
    #undef I
    #undef FF
    #undef GG
    #undef HH
    #undef II

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace detail

inline std::string ComputeMD5Hex(const std::string& input) {
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;

    std::vector<uint8_t> padded(input.begin(), input.end());
    padded.push_back(0x80);
    while ((padded.size() % 64) != 56) {
        padded.push_back(0x00);
    }
    for (int i = 0; i < 8; ++i) {
        padded.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));
    }

    for (size_t offset = 0; offset < padded.size(); offset += 64) {
        detail::MD5Transform(state, &padded[offset]);
    }

    uint8_t digest[16];
    for (int i = 0; i < 4; ++i) {
        digest[i * 4]     = static_cast<uint8_t>(state[i] & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 8) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>((state[i] >> 24) & 0xff);
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        oss << std::setw(2) << static_cast<int>(digest[i]);
    }
    return oss.str();
}

} // namespace weflow
