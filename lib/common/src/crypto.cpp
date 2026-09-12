// ============================================================================
//  Arches Instrumentation
//
//  Copyright (C) 2026 Soccentric. All rights reserved.
//  Author: Sandesh Ghimire (sandesh@soccentric)
//
//  This file is part of a proprietary product of Soccentric.
//
//  NO WARRANTY:
//  This software is provided "AS IS", without warranty of any kind, either
//  express or implied, including but not limited to the warranties of
//  merchantability, fitness for a particular purpose, and non-infringement.
//  In no event shall Soccentric be liable for any claim, damages, or other
//  liability arising from the use of this software.
//
//  This software may not be used, copied, modified, distributed, or
//  disclosed, in whole or in part, without the express prior written
//  consent of Soccentric.
// ============================================================================
/**
 * @file lib/common/src/crypto.cpp
 * @brief Self-contained cryptographic primitives: SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), X25519 (RFC 7748), ChaCha20 (RFC 8439), and HKDF-SHA256 (RFC 5869), tested against published vectors (REQ §.2, §, §10).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/crypto.h"

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace instr
{
/// @brief Self-contained cryptographic primitives (hashes, AEAD, X25519) and signature verification.
namespace crypto
{

/// @brief Render the digest as 64 lowercase hex characters.
/// @return 64 lowercase hex characters.
std::string Digest32::hex() const
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (uint8_t x : b)
    {
        s += d[x >> 4];
        s += d[x & 0xF];
    }
    return s;
}

// ---------------- SHA-256 ----------------

namespace
{

struct Sha256Ctx
{
    uint32_t h[8];
    uint64_t total = 0;
    uint8_t buf[64];
    size_t buflen = 0;
};

const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
                        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
                        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/// @brief Rotate right helper for SHA-256.
/// @param x Value to rotate.
/// @param n Bit count.
/// @return Rotated value.
inline uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

/// @brief Initialise a SHA-256 context with the FIPS 180-4 IV.
/// @param c Context to initialise.
void sha_init(Sha256Ctx& c)
{
    c.h[0] = 0x6a09e667;
    c.h[1] = 0xbb67ae85;
    c.h[2] = 0x3c6ef372;
    c.h[3] = 0xa54ff53a;
    c.h[4] = 0x510e527f;
    c.h[5] = 0x9b05688c;
    c.h[6] = 0x1f83d9ab;
    c.h[7] = 0x5be0cd19;
    c.total = 0;
    c.buflen = 0;
}

/// @brief Absorb one 64-byte SHA-256 block into the context.
/// @param c Context to absorb into.
/// @param p One 64-byte block.
void sha_block(Sha256Ctx& c, const uint8_t* p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i)
    {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c.h[0] += a;
    c.h[1] += b;
    c.h[2] += cc;
    c.h[3] += d;
    c.h[4] += e;
    c.h[5] += f;
    c.h[6] += g;
    c.h[7] += h;
    (void)0;
}

/// @brief Absorb an arbitrary-length byte string into the SHA-256 context.
/// @param c Context to absorb into.
/// @param p Input bytes.
/// @param n Input length.
void sha_update(Sha256Ctx& c, const uint8_t* p, size_t n)
{
    c.total += n;
    while (n > 0)
    {
        if (c.buflen == 0 && n >= 64)
        {
            sha_block(c, p);
            p += 64;
            n -= 64;
            continue;
        }
        size_t take = std::min(n, size_t(64) - c.buflen);
        std::memcpy(c.buf + c.buflen, p, take);
        c.buflen += take;
        p += take;
        n -= take;
        if (c.buflen == 64)
        {
            sha_block(c, c.buf);
            c.buflen = 0;
        }
    }
}

/// @brief Finalise the SHA-256 context and emit the 32-byte digest.
/// @param c Context to finalise.
void sha_final(Sha256Ctx& c, uint8_t out[32])
{
    uint64_t bits = c.total * 8;
    uint8_t pad = 0x80;
    sha_update(c, &pad, 1);
    uint8_t z = 0;
    while (c.buflen != 56) sha_update(c, &z, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    // update would recount total; block-write directly
    std::memcpy(c.buf + 56, len, 8);
    sha_block(c, c.buf);
    for (int i = 0; i < 8; ++i)
    {
        out[i * 4] = uint8_t(c.h[i] >> 24);
        out[i * 4 + 1] = uint8_t(c.h[i] >> 16);
        out[i * 4 + 2] = uint8_t(c.h[i] >> 8);
        out[i * 4 + 3] = uint8_t(c.h[i]);
    }
}

} // namespace

/// @brief SHA-256 over a byte string (convenience overload).
/// @param data Input bytes.
/// @param len Input length.
/// @return 32-byte digest.
Digest32 sha256(const uint8_t* data, size_t len)
{
    Sha256Ctx c;
    sha_init(c);
    sha_update(c, data, len);
    Digest32 d;
    sha_final(c, d.b.data());
    return d;
}

/// @brief HMAC-SHA256 message authentication code (RFC 2104).
/// @param key Secret key bytes.
/// @param msg Message to authenticate.
/// @return 32-byte MAC.
Digest32 hmac_sha256(const std::string& key, const std::string& msg)
{
    std::string k = key;
    if (k.size() > 64) k = std::string(reinterpret_cast<const char*>(sha256(k).b.data()), 32);
    k.resize(64, '\0');
    std::string ipad, opad;
    ipad.reserve(64);
    opad.reserve(64);
    for (int i = 0; i < 64; ++i)
    {
        ipad += char(k[i] ^ 0x36);
        opad += char(k[i] ^ 0x5c);
    }
    Digest32 inner = sha256(ipad + msg);
    Digest32 outer = sha256(opad + std::string(reinterpret_cast<const char*>(inner.b.data()), 32));
    return outer;
}

// ---------------- X25519 (RFC 7748) ----------------

namespace
{

typedef uint64_t fe[5]; // radix 2^51

constexpr uint64_t MASK51 = (1ull << 51) - 1;

/// @brief Field-arithmetic helpers for X25519 over GF(2^255 - 19). Zero a field element.
/// @param h Field element to zero.
static void fe_zero(fe h)
{
    h[0] = h[1] = h[2] = h[3] = h[4] = 0;
}
/// @brief Set a field element to one.
/// @param h Field element to set.
static void fe_one(fe h)
{
    h[0] = 1;
    h[1] = h[2] = h[3] = h[4] = 0;
}
/// @brief Copy a field element.
/// @param out Destination.
/// @param in Source.
static void fe_copy(fe out, const fe in)
{
    for (int i = 0; i < 5; ++i) out[i] = in[i];
}

/// @brief Field addition (mod p).
/// @param h Result.
/// @param f Left operand.
/// @param g Right operand.
static void fe_add(fe h, const fe f, const fe g)
{
    for (int i = 0; i < 5; ++i) h[i] = f[i] + g[i];
}

/// @brief Field subtraction (mod p).
/// @param h Result.
/// @param f Left operand.
/// @param g Right operand.
static void fe_sub(fe h, const fe f, const fe g)
{
    // h = f - g + 2p, limb-wise, keeping values positive.
    static const uint64_t two_p[5] = {2ull * ((1ull << 51) - 19), 2ull * ((1ull << 51) - 1), 2ull * ((1ull << 51) - 1), 2ull * ((1ull << 51) - 1), 2ull * ((1ull << 51) - 1)};
    for (int i = 0; i < 5; ++i) h[i] = f[i] + two_p[i] - g[i];
}

/// @brief Normalise limb overflow after arithmetic.
/// @param h Field element to normalise.
static void fe_carry(fe h)
{
    uint64_t c;
    c = h[0] >> 51;
    h[0] &= MASK51;
    h[1] += c;
    c = h[1] >> 51;
    h[1] &= MASK51;
    h[2] += c;
    c = h[2] >> 51;
    h[2] &= MASK51;
    h[3] += c;
    c = h[3] >> 51;
    h[3] &= MASK51;
    h[4] += c;
    c = h[4] >> 51;
    h[4] &= MASK51;
    h[0] += 19 * c;
    c = h[0] >> 51;
    h[0] &= MASK51;
    h[1] += c;
}

/// @brief Field multiplication (mod p).
/// @param h Result.
/// @param f Left operand.
/// @param g Right operand.
static void fe_mul(fe h, const fe f, const fe g)
{
    unsigned __int128 t0, t1, t2, t3, t4;
    unsigned __int128 f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    unsigned __int128 g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    unsigned __int128 g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4;
    t0 = f0 * g0 + f1 * g4_19 + f2 * g3_19 + f3 * g2_19 + f4 * g1_19;
    t1 = f0 * g1 + f1 * g0 + f2 * g4_19 + f3 * g3_19 + f4 * g2_19;
    t2 = f0 * g2 + f1 * g1 + f2 * g0 + f3 * g4_19 + f4 * g3_19;
    t3 = f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0 + f4 * g4_19;
    t4 = f0 * g4 + f1 * g3 + f2 * g2 + f3 * g1 + f4 * g0;
    // Fold the five 128-bit accumulators into 51-bit limbs, carrying in 128-bit
    // so no high bits are lost.
    unsigned __int128 u = t0;
    h[0] = uint64_t(u) & MASK51;
    u >>= 51;
    u += t1;
    h[1] = uint64_t(u) & MASK51;
    u >>= 51;
    u += t2;
    h[2] = uint64_t(u) & MASK51;
    u >>= 51;
    u += t3;
    h[3] = uint64_t(u) & MASK51;
    u >>= 51;
    u += t4;
    h[4] = uint64_t(u) & MASK51;
    uint64_t top = uint64_t(u >> 51);
    h[0] += 19 * top;
    uint64_t c;
    c = h[0] >> 51;
    h[0] &= MASK51;
    h[1] += c;
    c = h[1] >> 51;
    h[1] &= MASK51;
    h[2] += c;
    c = h[2] >> 51;
    h[2] &= MASK51;
    h[3] += c;
    c = h[3] >> 51;
    h[3] &= MASK51;
    h[4] += c;
}

/// @brief Multiply a field element by a small constant.
/// @param h Result.
/// @param f Left operand.
/// @param n Small constant.
static void fe_mul_small(fe h, const fe f, uint64_t n)
{
    unsigned __int128 t[5];
    for (int i = 0; i < 5; ++i) t[i] = (unsigned __int128)f[i] * n;
    unsigned __int128 u = t[0];
    h[0] = uint64_t(u) & MASK51;
    u >>= 51;
    for (int i = 1; i < 5; ++i)
    {
        u += t[i];
        h[i] = uint64_t(u) & MASK51;
        u >>= 51;
    }
    h[0] += 19 * uint64_t(u);
    uint64_t c;
    c = h[0] >> 51;
    h[0] &= MASK51;
    h[1] += c;
    c = h[1] >> 51;
    h[1] &= MASK51;
    h[2] += c;
    c = h[2] >> 51;
    h[2] &= MASK51;
    h[3] += c;
    c = h[3] >> 51;
    h[3] &= MASK51;
    h[4] += c;
}

/// @brief Field squaring.
/// @param h Result.
/// @param f Operand.
static void fe_sq(fe h, const fe f)
{
    fe_mul(h, f, f);
}

/// @brief Decode 32 little-endian bytes into a field element (masking the high bit).
/// @param h Destination field element.
static void fe_frombytes(fe h, const uint8_t in[32])
{
    uint64_t w0 = uint64_t(in[0]) | (uint64_t(in[1]) << 8) | (uint64_t(in[2]) << 16) | (uint64_t(in[3]) << 24) | (uint64_t(in[4]) << 32) | (uint64_t(in[5]) << 40) | (uint64_t(in[6]) << 48) | (uint64_t(in[7]) << 56);
    uint64_t w1 = uint64_t(in[8]) | (uint64_t(in[9]) << 8) | (uint64_t(in[10]) << 16) | (uint64_t(in[11]) << 24) | (uint64_t(in[12]) << 32) | (uint64_t(in[13]) << 40) | (uint64_t(in[14]) << 48) | (uint64_t(in[15]) << 56);
    uint64_t w2 = uint64_t(in[16]) | (uint64_t(in[17]) << 8) | (uint64_t(in[18]) << 16) | (uint64_t(in[19]) << 24) | (uint64_t(in[20]) << 32) | (uint64_t(in[21]) << 40) | (uint64_t(in[22]) << 48) | (uint64_t(in[23]) << 56);
    uint64_t w3 = uint64_t(in[24]) | (uint64_t(in[25]) << 8) | (uint64_t(in[26]) << 16) | (uint64_t(in[27]) << 24) | (uint64_t(in[28]) << 32) | (uint64_t(in[29]) << 40) | (uint64_t(in[30]) << 48) | (uint64_t(in[31]) << 56);
    h[0] = w0 & MASK51;
    h[1] = ((w0 >> 51) | (w1 << 13)) & MASK51;
    h[2] = ((w1 >> 38) | (w2 << 26)) & MASK51;
    h[3] = ((w2 >> 25) | (w3 << 39)) & MASK51;
    h[4] = (w3 >> 12) & MASK51;
}

/// @brief Encode a field element as 32 little-endian bytes.
/// @param f Field element to encode.
static void fe_tobytes(uint8_t out[32], const fe f)
{
    fe t;
    fe_copy(t, f);
    fe_carry(t);
    // p = {MASK51 - 18, MASK51, MASK51, MASK51, MASK51} in limbs
    // (limb0 = 2^51 - 19). Conditionally subtract p once: t in [0, 2^255).
    bool ge;
    if (t[4] != MASK51)
        ge = t[4] > MASK51;
    else if (t[3] != MASK51)
        ge = t[3] > MASK51;
    else if (t[2] != MASK51)
        ge = t[2] > MASK51;
    else if (t[1] != MASK51)
        ge = t[1] > MASK51;
    else
        ge = t[0] >= MASK51 - 18;
    if (ge)
    {
        uint64_t b = 0;
        for (int i = 0; i < 5; ++i)
        {
            uint64_t pi = (i == 0) ? (MASK51 - 18) : MASK51;
            uint64_t sub = pi + b;
            uint64_t v;
            if (t[i] >= sub)
            {
                v = t[i] - sub;
                b = 0;
            }
            else
            {
                v = t[i] + (1ull << 51) - sub;
                b = 1;
            }
            t[i] = v;
        }
    }
    uint64_t w0 = t[0] | (t[1] << 51);
    uint64_t w1 = (t[1] >> 13) | (t[2] << 38);
    uint64_t w2 = (t[2] >> 26) | (t[3] << 25);
    uint64_t w3 = (t[3] >> 39) | (t[4] << 12);
    std::memcpy(out + 0, &w0, 8);
    std::memcpy(out + 8, &w1, 8);
    std::memcpy(out + 16, &w2, 8);
    std::memcpy(out + 24, &w3, 8);
}

/// @brief Field inversion via Fermat (z^(p-2)).
/// @param out Result.
/// @param z Element to invert.
static void fe_invert(fe out, const fe z)
{
    // Exponentiation by squaring with exponent e = p-2 = 2^255 - 21.
    // Little-endian bytes of e: e[0] = 0xff - 21 = 0xea, e[1..30] = 0xff,
    // e[31] = 0x7f. Process bits MSB-first.
    fe acc, base;
    fe_copy(base, z);
    fe_one(acc);
    uint8_t e[32];
    std::memset(e, 0xff, 32);
    e[0] = 0xeb; // low byte of p-2 = 2^255 - 21 (borrow propagates)
    e[31] &= 0x7f; // clear bit 255
    bool first = true;
    for (int byte = 31; byte >= 0; --byte)
    {
        for (int bit = 7; bit >= 0; --bit)
        {
            if (!first) fe_sq(acc, acc);
            first = false;
            if ((e[byte] >> bit) & 1) fe_mul(acc, acc, base);
        }
    }
    fe_copy(out, acc);
}

/// @brief Constant-time conditional swap of two field elements (ladder step helper).
/// @param swap 1 to swap, 0 to leave unchanged.
/// @param a First element.
/// @param b Second element.
static void cswap(uint64_t swap, fe a, fe b)
{
    uint64_t mask = 0 - swap; // all-ones if swap == 1
    for (int i = 0; i < 5; ++i)
    {
        uint64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

} // namespace

/// @brief Compute the X25519 shared secret (RFC 7748).
/// @param priv Our 32-byte private scalar.
/// @param pub_in Peer's 32-byte public point.
/// @return 32-byte shared secret.
std::array<uint8_t, 32> x25519(const uint8_t priv_in[32], const uint8_t pub_in[32])
{
    uint8_t k[32], u[32];
    std::memcpy(k, priv_in, 32);
    std::memcpy(u, pub_in, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    u[31] &= 127; // mask MSB per RFC 7748

    fe x1, x2, z2, x3, z3;
    fe_frombytes(x1, u);
    fe_one(x2);
    fe_zero(z2);
    fe_copy(x3, x1);
    fe_one(z3);

    uint64_t swap = 0;
    for (int t = 254; t >= 0; --t)
    {
        uint64_t kt = (k[t / 8] >> (t % 8)) & 1;
        swap ^= kt;
        cswap(swap, x2, x3);
        cswap(swap, z2, z3);
        swap = kt;

        fe a, aa, b, bb, e, c, d, da, cb, t0, t1;
        fe_add(a, x2, z2); // A = x2 + z2
        fe_sq(aa, a); // AA = A^2
        fe_sub(b, x2, z2); // B = x2 - z2
        fe_sq(bb, b); // BB = B^2
        fe_sub(e, aa, bb); // E = AA - BB
        fe_add(c, x3, z3); // C = x3 + z3
        fe_sub(d, x3, z3); // D = x3 - z3
        fe_mul(da, d, a); // DA = D * A
        fe_mul(cb, c, b); // CB = C * B
        fe_add(t0, da, cb);
        fe_sq(x3, t0); // x3 = (DA + CB)^2
        fe_sub(t1, da, cb);
        fe_sq(t0, t1);
        fe_mul(z3, x1, t0); // z3 = x1 * (DA - CB)^2
        fe_mul(x2, aa, bb); // x2 = AA * BB
        fe_mul_small(t1, e, 121665);
        fe_add(t0, aa, t1);
        fe_mul(z2, e, t0); // z2 = E * (AA + 121665*E)
    }
    cswap(swap, x2, x3);
    cswap(swap, z2, z3);

    fe zinv, res;
    fe_invert(zinv, z2);
    fe_mul(res, x2, zinv);
    std::array<uint8_t, 32> out;
    fe_tobytes(out.data(), res);
    return out;
}

/// @brief Generate an X25519 keypair using @p rng.
/// @param rng Random source (tests inject a deterministic one).
/// @return Fresh keypair.
X25519Key x25519_generate(const Rng32& rng)
{
    X25519Key k;
    rng(k.priv.data(), 32);
    k.priv[0] &= 248;
    k.priv[31] &= 127;
    k.priv[31] |= 64;
    static const uint8_t base[32] = {9}; // u = 9
    auto p = x25519(k.priv.data(), base);
    std::memcpy(k.pub.data(), p.data(), 32);
    return k;
}

// ---------------- ChaCha20 (RFC 8439) ----------------

namespace
{

/// @brief Rotate left helper for ChaCha20.
/// @param v Value to rotate.
/// @param c Bit count.
/// @return Rotated value.
inline uint32_t rotl32(uint32_t v, int c)
{
    return (v << c) | (v >> (32 - c));
}

/// @brief ChaCha20 quarter-round mixing function.
/// @param a First lane.
/// @param b Second lane.
/// @param c Third lane.
/// @param d Fourth lane.
void quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
{
    a += b;
    d ^= a;
    d = rotl32(d, 16);
    c += d;
    b ^= c;
    b = rotl32(b, 12);
    a += b;
    d ^= a;
    d = rotl32(d, 8);
    c += d;
    b ^= c;
    b = rotl32(b, 7);
}

/// @brief Produce one 64-byte ChaCha20 keystream block.
void chacha_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64])
{
    static const uint8_t sigma[] = "expand 32-byte k";
    uint32_t st[16];
    for (int i = 0; i < 4; ++i) st[i] = uint32_t(sigma[i * 4]) | (uint32_t(sigma[i * 4 + 1]) << 8) | (uint32_t(sigma[i * 4 + 2]) << 16) | (uint32_t(sigma[i * 4 + 3]) << 24);
    for (int i = 0; i < 8; ++i) st[4 + i] = uint32_t(key[i * 4]) | (uint32_t(key[i * 4 + 1]) << 8) | (uint32_t(key[i * 4 + 2]) << 16) | (uint32_t(key[i * 4 + 3]) << 24);
    st[12] = counter;
    for (int i = 0; i < 3; ++i) st[13 + i] = uint32_t(nonce[i * 4]) | (uint32_t(nonce[i * 4 + 1]) << 8) | (uint32_t(nonce[i * 4 + 2]) << 16) | (uint32_t(nonce[i * 4 + 3]) << 24);

    uint32_t x[16];
    std::memcpy(x, st, sizeof(x));
    for (int round = 0; round < 10; ++round)
    {
        quarter(x[0], x[4], x[8], x[12]);
        quarter(x[1], x[5], x[9], x[13]);
        quarter(x[2], x[6], x[10], x[14]);
        quarter(x[3], x[7], x[11], x[15]);
        quarter(x[0], x[5], x[10], x[15]);
        quarter(x[1], x[6], x[11], x[12]);
        quarter(x[2], x[7], x[8], x[13]);
        quarter(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i)
    {
        uint32_t v = x[i] + st[i];
        out[i * 4] = uint8_t(v);
        out[i * 4 + 1] = uint8_t(v >> 8);
        out[i * 4 + 2] = uint8_t(v >> 16);
        out[i * 4 + 3] = uint8_t(v >> 24);
    }
}

} // namespace

/// @brief ChaCha20 stream cipher keystream application (RFC 8439).
/// @param key 32-byte key.
/// @param nonce 12-byte nonce.
/// @param counter Initial block counter.
/// @param in Plaintext input.
/// @param out Ciphertext output (may alias @p in).
/// @param len Byte length to process.
void chacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, uint8_t* out, size_t len)
{
    uint8_t block[64];
    size_t off = 0;
    while (off < len)
    {
        chacha_block(key, nonce, counter++, block);
        size_t take = std::min(len - off, size_t(64));
        for (size_t i = 0; i < take; ++i) out[off + i] = in[off + i] ^ block[i];
        off += take;
    }
}

/// @brief Encrypt-then-MAC convenience: ChaCha20 stream + HMAC-SHA256 over (nonce || ciphertext).
///
/// Output layout: nonce(12) || ciphertext || tag(32).
///
/// @param key32 32-byte symmetric key.
/// @param plaintext Bytes to protect.
/// @param rng Random source for the nonce.
/// @return Sealed token (nonce || ciphertext || tag).
std::string aead_seal(const std::string& key32, const std::string& plaintext, const Rng32& rng)
{
    uint8_t nonce[12];
    rng(nonce, 12);
    std::string ct(plaintext.size(), '\0');
    chacha20(reinterpret_cast<const uint8_t*>(key32.data()), nonce, 1, reinterpret_cast<const uint8_t*>(plaintext.data()), reinterpret_cast<uint8_t*>(&ct[0]), plaintext.size());
    std::string nonce_s(reinterpret_cast<const char*>(nonce), 12);
    Digest32 tag = hmac_sha256(key32, nonce_s + ct);
    return nonce_s + ct + std::string(reinterpret_cast<const char*>(tag.b.data()), 32);
}

/// @brief Open a token produced by aead_seal(); verifies the tag before decrypting.
/// @param key32 32-byte symmetric key.
/// @param sealed Sealed token.
/// @return Plaintext, or a SignatureInvalid/Corrupt error on tampering.
Result<std::string> aead_open(const std::string& key32, const std::string& sealed)
{
    if (sealed.size() < 12 + 32) return Result<std::string>::fail(Errc::SignatureInvalid, "sealed blob too short", "aead_open");
    std::string nonce_s = sealed.substr(0, 12);
    std::string ct = sealed.substr(12, sealed.size() - 44);
    std::string tag = sealed.substr(sealed.size() - 32, 32);
    Digest32 expect = hmac_sha256(key32, nonce_s + ct);
    if (std::memcmp(expect.b.data(), tag.data(), 32) != 0) return Result<std::string>::fail(Errc::SignatureInvalid, "authentication tag mismatch", "aead_open");
    std::string pt(ct.size(), '\0');
    chacha20(reinterpret_cast<const uint8_t*>(key32.data()), reinterpret_cast<const uint8_t*>(nonce_s.data()), 1, reinterpret_cast<const uint8_t*>(ct.data()), reinterpret_cast<uint8_t*>(&pt[0]), ct.size());
    return Result<std::string>::good(pt);
}

// ---------------- HKDF-SHA256 (RFC 5869) ----------------

/// @brief HKDF-SHA256 key derivation (RFC 5869).
/// @param ikm Input key material.
/// @param salt Optional salt (may be empty).
/// @param info Context/application string.
/// @param len Desired output length in bytes.
/// @return Derived key bytes.
std::string hkdf_sha256(const std::string& ikm, const std::string& salt, const std::string& info, size_t len)
{
    std::string actual_salt = salt.empty() ? std::string(32, '\0') : salt;
    Digest32 prk = hmac_sha256(actual_salt, ikm);
    std::string prk_s(reinterpret_cast<const char*>(prk.b.data()), 32);
    std::string okm;
    std::string t;
    uint32_t counter = 1;
    while (okm.size() < len)
    {
        // T(i) = HMAC(PRK, T(i-1) | info | i) with i a SINGLE octet (RFC 5869).
        char cbuf[1] = {char(counter & 0xFF)};
        Digest32 dig = hmac_sha256(prk_s, t + info + std::string(cbuf, 1));
        t.assign(reinterpret_cast<const char*>(dig.b.data()), 32);
        okm += t;
        ++counter;
    }
    okm.resize(len);
    return okm;
}

// ---------------- encodings ----------------

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Standard base64 encode (with padding).
/// @param raw Bytes to encode.
/// @return Base64 text.
std::string base64_encode(const std::string& raw)
{
    std::string out;
    size_t i = 0;
    while (i + 2 < raw.size())
    {
        uint32_t v = (uint8_t(raw[i]) << 16) | (uint8_t(raw[i + 1]) << 8) | uint8_t(raw[i + 2]);
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += B64[v & 63];
        i += 3;
    }
    if (i + 1 == raw.size())
    {
        uint32_t v = uint8_t(raw[i]) << 16;
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += "==";
    }
    else if (i + 2 == raw.size())
    {
        uint32_t v = (uint8_t(raw[i]) << 16) | (uint8_t(raw[i + 1]) << 8);
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

/// @brief Standard base64 decode.
/// @param b64 Base64 text.
/// @param b64in Base64 text to decode.
/// @return Decoded bytes, or an InvalidArgument error for malformed input.
/// @return Decoded bytes, or an InvalidArgument error.
Result<std::string> base64_decode(const std::string& b64in)
{
    static int8_t rev[256];
    static bool init = false;
    if (!init)
    {
        std::memset(rev, -1, sizeof(rev));
        for (int i = 0; i < 64; ++i) rev[uint8_t(B64[i])] = int8_t(i);
        rev['-'] = 62;
        rev['_'] = 63; // accept URL-safe too
        init = true;
    }
    std::string out;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : b64in)
    {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t v = rev[uint8_t(c)];
        if (v < 0) return Result<std::string>::fail(Errc::InvalidArgument, "invalid base64 character", "base64_decode");
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out += char((buf >> bits) & 0xFF);
        }
    }
    return Result<std::string>::good(out);
}

/// @brief Lowercase hex encode.
/// @param raw Bytes to encode.
/// @return Hex text.
std::string hex_encode(const std::string& raw)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw)
    {
        out += d[c >> 4];
        out += d[c & 0xF];
    }
    return out;
}

/// @brief Hex decode (upper or lower case accepted).
/// @param hex Hex text.
/// @param hexin Hex text to decode.
/// @return Decoded bytes, or an InvalidArgument error for malformed input.
/// @return Decoded bytes, or an InvalidArgument error.
Result<std::string> hex_decode(const std::string& hexin)
{
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hexin.size() % 2 != 0) return Result<std::string>::fail(Errc::InvalidArgument, "hex length must be even", "hex_decode");
    std::string out;
    out.reserve(hexin.size() / 2);
    for (size_t i = 0; i < hexin.size(); i += 2)
    {
        int hi = val(hexin[i]), lo = val(hexin[i + 1]);
        if (hi < 0 || lo < 0) return Result<std::string>::fail(Errc::InvalidArgument, "invalid hex digit", "hex_decode");
        out += char((hi << 4) | lo);
    }
    return Result<std::string>::good(out);
}

// ---------------- randomness ----------------

/// @brief Fill @p n bytes from the OS CSPRNG (getrandom(2)).
///
/// Fails loudly rather than silently degrading — key generation must not run
/// without entropy (§6.2).
///
/// @param out Buffer to fill.
/// @param n Number of bytes.
/// @return ok() on success, or the OS error.
Error random_bytes(uint8_t* out, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        ssize_t r = getrandom(out + off, n - off, 0);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            return Error::make(Errc::Unavailable, "CSPRNG unavailable (getrandom failed)", "random_bytes");
        }
        off += size_t(r);
    }
    return Error::none();
}

} // namespace crypto
} // namespace instr