// sha256.h
// the SHA-256 digest (FIPS 180-4) of a byte string, for checking what is inside a package file (core/package.h).
//
// usage
//   unsigned char d[32]; embr::sha256(bytes.data(), bytes.size(), d);
//   std::string hex = embr::hexOf(d, 32);

#ifndef EMBR_CORE_SHA256_H
#define EMBR_CORE_SHA256_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace embr {

namespace sha_detail {
inline const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void block(uint32_t h[8], const unsigned char* b) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) | ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
} // namespace sha_detail

// the 32 byte digest of msg[0..len)
inline void sha256(const void* data, size_t len, unsigned char out[32]) {
    const unsigned char* msg = static_cast<const unsigned char*>(data);
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) sha_detail::block(h, msg + i * 64);
    unsigned char tail[128];
    size_t rest = len - full * 64;
    std::memset(tail, 0, sizeof tail);
    if (rest) std::memcpy(tail, msg + full * 64, rest);
    tail[rest] = 0x80;
    size_t tailLen = rest < 56 ? 64 : 128;
    uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<unsigned char>(bits >> (i * 8));
    for (size_t off = 0; off < tailLen; off += 64) sha_detail::block(h, tail + off);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<unsigned char>(h[i] >> (24 - 8 * j));
}

inline std::string hexOf(const unsigned char* d, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
    return s;
}

inline std::string sha256Hex(const std::string& bytes) {
    unsigned char d[32];
    sha256(bytes.data(), bytes.size(), d);
    return hexOf(d, 32);
}

} // namespace embr

#endif // EMBR_CORE_SHA256_H
