// ed25519.h
// Ed25519 signatures (RFC 8032) and the SHA-512 they need, for signing packages (core/package.h). a small, plain implementation
// after TweetNaCl (public domain): it is easy to read and fast enough to sign or check one 32 byte hash.
//
// usage
//   unsigned char seed[32], pub[32], sec[64], sig[64];
//   embr::ed25519::randomBytes(seed, 32);
//   embr::ed25519::keypair(seed, pub, sec);
//   embr::ed25519::sign(msg, len, sec, sig);
//   bool ok = embr::ed25519::verify(msg, len, sig, pub);
//
// good to know
//   - not constant time in every step (the field arithmetic is, the loops over the message are not). it signs hashes of public files, so
//     there is no secret-dependent timing worth hiding between callers
//   - the constants are not typed in: they were computed from their definitions (square roots and cube roots of primes for SHA-512,
//     -121665/121666 and 4/5 modulo 2^255-19 for the curve), and the code is checked against the RFC 8032 test vectors in
//     tests/ed25519_test.cpp
//   - `verify` takes any signature the equation allows, the way TweetNaCl does; it does not reject S above the group order. a package
//     signature signs one hash, so a second valid encoding of it gives an attacker nothing

#ifndef EMBR_CORE_ED25519_H
#define EMBR_CORE_ED25519_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/random.h>
#  endif
#endif

namespace embr {
namespace ed25519 {

namespace detail {
// generated: the constants of SHA-512 and Ed25519, worked out from their definitions (see the comments in ed25519.h)
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
static const uint64_t H512[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
static const int64_t D_[16] = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const int64_t D2_[16] = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const int64_t X_[16] = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const int64_t Y_[16] = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const int64_t I_[16] = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

inline void sha512Block(uint64_t h[8], const unsigned char* b) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        uint64_t v = 0;
        for (int j = 0; j < 8; ++j) v = (v << 8) | b[i * 8 + j];
        w[i] = v;
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = hh + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t mj = (a & bb) ^ (a & c) ^ (bb & c);
        uint64_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

typedef int64_t gf[16];
static const gf gf0 = {0}, gf1 = {1};

inline void set25519(gf r, const gf a) { for (int i = 0; i < 16; ++i) r[i] = a[i]; }
inline void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t(1) << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
inline void sel25519(gf p, gf q, int b) {
    int64_t c = ~(int64_t(b) - 1);
    for (int i = 0; i < 16; ++i) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
inline void pack25519(unsigned char* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) { o[2 * i] = static_cast<unsigned char>(t[i] & 0xff); o[2 * i + 1] = static_cast<unsigned char>(t[i] >> 8); }
}
inline int differ32(const unsigned char* x, const unsigned char* y) {
    unsigned d = 0;
    for (int i = 0; i < 32; ++i) d |= x[i] ^ y[i];
    return (1 & ((d - 1) >> 8)) - 1;
}
inline int neq25519(const gf a, const gf b) { unsigned char c[32], d[32]; pack25519(c, a); pack25519(d, b); return differ32(c, d); }
inline unsigned char par25519(const gf a) { unsigned char d[32]; pack25519(d, a); return d[0] & 1; }
inline void unpack25519(gf o, const unsigned char* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (int64_t(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}
inline void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
inline void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
inline void M(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i) for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o); car25519(o);
}
inline void S(gf o, const gf a) { M(o, a, a); }
inline void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}
inline void pow2523(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 250; a >= 0; --a) { S(c, c); if (a != 1) M(c, c, i); }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}
inline void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, D2_);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}
inline void cswap(gf p[4], gf q[4], unsigned char b) { for (int i = 0; i < 4; ++i) sel25519(p[i], q[i], b); }
inline void packPoint(unsigned char* r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]); M(tx, p[0], zi); M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= static_cast<unsigned char>(par25519(tx) << 7);
}
inline void scalarmult(gf p[4], gf q[4], const unsigned char* s) {
    set25519(p[0], gf0); set25519(p[1], gf1); set25519(p[2], gf1); set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        unsigned char b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}
inline void scalarbase(gf p[4], const unsigned char* s) {
    gf q[4];
    set25519(q[0], X_); set25519(q[1], Y_); set25519(q[2], gf1); M(q[3], X_, Y_);
    scalarmult(p, q, s);
}
static const uint64_t Lorder[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
inline void modL(unsigned char* r, int64_t x[64]) {
    int64_t carry, i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * static_cast<int64_t>(Lorder[j - (i - 32)]);
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * static_cast<int64_t>(Lorder[j]);
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) x[j] -= carry * static_cast<int64_t>(Lorder[j]);
    for (i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<unsigned char>(x[i] & 255);
    }
}
inline void reduce(unsigned char* r) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = r[i];
    for (int i = 0; i < 64; ++i) r[i] = 0;
    modL(r, x);
}
inline int unpackneg(gf r[4], const unsigned char p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D_);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den); S(den4, den2); M(den6, den4, den2); M(t, den6, num); M(t, t, den);
    pow2523(t, t);
    M(t, t, num); M(t, t, den); M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I_);
    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}
} // namespace detail

// the longest message sign() and verify() take (they hold it in memory); a package signs 48 bytes
constexpr size_t kMaxMessage = size_t(1) << 40;

// the SHA-512 digest of msg[0..len)
inline void sha512(const unsigned char* msg, size_t len, unsigned char out[64]) {
    uint64_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = detail::H512[i];
    size_t full = len / 128;
    for (size_t i = 0; i < full; ++i) detail::sha512Block(h, msg + i * 128);
    unsigned char tail[256];
    size_t rest = len - full * 128;
    std::memset(tail, 0, sizeof tail);
    if (rest) std::memcpy(tail, msg + full * 128, rest);
    tail[rest] = 0x80;
    size_t tailLen = rest < 112 ? 128 : 256;
    uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<unsigned char>(bits >> (i * 8));
    for (size_t off = 0; off < tailLen; off += 128) detail::sha512Block(h, tail + off);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) out[i * 8 + j] = static_cast<unsigned char>(h[i] >> (56 - 8 * j));
}

// the key pair for a 32 byte secret seed: pub is the 32 byte public key, sec the 64 bytes (seed, then public key) that sign() wants
inline void keypair(const unsigned char seed[32], unsigned char pub[32], unsigned char sec[64]) {
    using namespace detail;
    unsigned char d[64];
    gf p[4];
    sha512(seed, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    scalarbase(p, d);
    packPoint(pub, p);
    for (int i = 0; i < 32; ++i) { sec[i] = seed[i]; sec[32 + i] = pub[i]; }
}

// a detached signature (64 bytes) of msg[0..len)
inline void sign(const unsigned char* msg, size_t len, const unsigned char sec[64], unsigned char sig[64]) {
    using namespace detail;
    unsigned char d[64], h[64], r[64];
    int64_t x[64];
    gf p[4];
    if (len > kMaxMessage) { std::memset(sig, 0, 64); return; }
    sha512(sec, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    std::vector<unsigned char> buf(len + 64);
    std::memcpy(buf.data() + 64, msg, len);
    for (int i = 0; i < 32; ++i) buf[32 + i] = d[32 + i];
    sha512(buf.data() + 32, len + 32, r);
    reduce(r);
    scalarbase(p, r);
    packPoint(buf.data(), p);
    for (int i = 0; i < 32; ++i) buf[i + 32] = sec[i + 32];
    sha512(buf.data(), len + 64, h);
    reduce(h);
    for (int i = 0; i < 64; ++i) x[i] = 0;
    for (int i = 0; i < 32; ++i) x[i] = r[i];
    for (int i = 0; i < 32; ++i) for (int j = 0; j < 32; ++j) x[i + j] += static_cast<int64_t>(h[i]) * static_cast<int64_t>(d[j]);
    modL(buf.data() + 32, x);
    std::memcpy(sig, buf.data(), 64);
}

// does sig sign msg[0..len) under the public key pub?
inline bool verify(const unsigned char* msg, size_t len, const unsigned char sig[64], const unsigned char pub[32]) {
    using namespace detail;
    unsigned char t[32], h[64];
    gf p[4], q[4];
    if (len > kMaxMessage) return false;
    if (unpackneg(q, pub)) return false;
    std::vector<unsigned char> m(len + 64);
    std::memcpy(m.data(), sig, 32);
    std::memcpy(m.data() + 32, pub, 32);
    if (len) std::memcpy(m.data() + 64, msg, len);
    sha512(m.data(), len + 64, h);
    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    add(p, q);
    packPoint(t, p);
    return differ32(sig, t) == 0;
}

// n bytes from the operating system's random source. false if there is none
inline bool randomBytes(unsigned char* buf, size_t n) {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("advapi32.dll");
    if (!lib) return false;
    using Fn = BOOLEAN(WINAPI*)(PVOID, ULONG);
    Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(lib, "SystemFunction036")));   // RtlGenRandom
    bool ok = fn && fn(buf, static_cast<ULONG>(n));
    FreeLibrary(lib);
    return ok;
#else
    size_t got = 0;
#  if defined(__linux__)
    while (got < n) {
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        got += static_cast<size_t>(r);
    }
    if (got == n) return true;
#  endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return false; }
        got += static_cast<size_t>(r);
    }
    close(fd);
    return true;
#endif
}

} // namespace ed25519
} // namespace embr

#endif // EMBR_CORE_ED25519_H
