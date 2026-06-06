// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── Sha256 implementation ─────────────────────────────────────────
// FIPS 180-4 SHA-256. Lives in the core subset. No external
// dependencies beyond <cstdint> / <cstring> / <array>.

#include "Sha256.h"

#include <cstring>

namespace sv {

namespace {

// ── Round constants ──────────────────────────────────────────────
// First 32 bits of the fractional parts of the cube roots of the
// first 64 primes. Straight from FIPS 180-4 §4.2.2.
constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

// ── Initial hash values ──────────────────────────────────────────
// First 32 bits of the fractional parts of the square roots of
// the first 8 primes. FIPS 180-4 §5.3.3.
constexpr uint32_t kSha256H0[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t bigSigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
inline uint32_t bigSigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
inline uint32_t smallSigma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}
inline uint32_t smallSigma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}
inline uint32_t choose(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}
inline uint32_t majority(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t beLoad32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) <<  8)
         |  static_cast<uint32_t>(p[3]);
}

inline void beStore32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    p[3] = static_cast<uint8_t>( v        & 0xFF);
}

inline void beStore64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[7 - i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

} // namespace

Sha256Hasher::Sha256Hasher() {
    reset();
}

void Sha256Hasher::reset() {
    for (int i = 0; i < 8; ++i) m_state[i] = kSha256H0[i];
    m_bitCount  = 0;
    m_bufferLen = 0;
    m_finalized = false;
    m_cachedDigest.fill(0);
}

void Sha256Hasher::processBlock(const uint8_t* block) {
    uint32_t w[64];
    for (int t = 0; t < 16; ++t) {
        w[t] = beLoad32(block + t * 4);
    }
    for (int t = 16; t < 64; ++t) {
        w[t] = smallSigma1(w[t - 2])
             + w[t - 7]
             + smallSigma0(w[t - 15])
             + w[t - 16];
    }

    uint32_t a = m_state[0];
    uint32_t b = m_state[1];
    uint32_t c = m_state[2];
    uint32_t d = m_state[3];
    uint32_t e = m_state[4];
    uint32_t f = m_state[5];
    uint32_t g = m_state[6];
    uint32_t h = m_state[7];

    for (int t = 0; t < 64; ++t) {
        uint32_t t1 = h + bigSigma1(e) + choose(e, f, g) + kSha256K[t] + w[t];
        uint32_t t2 = bigSigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256Hasher::update(const uint8_t* data, size_t len) {
    if (m_finalized) {
        // Re-use after finalize is a caller bug — quietly reset so the
        // next digest is at least deterministic.
        reset();
    }
    if (!data || len == 0) return;

    m_bitCount += static_cast<uint64_t>(len) * 8;

    // Finish any partial block first.
    if (m_bufferLen > 0) {
        const size_t take = std::min(len, size_t{64} - m_bufferLen);
        std::memcpy(m_buffer + m_bufferLen, data, take);
        m_bufferLen += take;
        data += take;
        len  -= take;
        if (m_bufferLen == 64) {
            processBlock(m_buffer);
            m_bufferLen = 0;
        }
    }

    // Drain full blocks directly from the caller's buffer.
    while (len >= 64) {
        processBlock(data);
        data += 64;
        len  -= 64;
    }

    // Stash the tail.
    if (len > 0) {
        std::memcpy(m_buffer, data, len);
        m_bufferLen = len;
    }
}

std::array<uint8_t, 32> Sha256Hasher::finalize() {
    if (m_finalized) return m_cachedDigest;

    // FIPS 180-4 §5.1.1: append 0x80, then enough zeros so the total
    // length mod 64 == 56, then the 64-bit big-endian length. 56 is
    // the start offset of the length field within the final 64-byte
    // block. If the current buffer is already ≥56, we need to spill
    // into a second block.
    uint8_t tail[128] = {};
    tail[0] = 0x80;

    // How many zero bytes + length bytes we need after the 0x80.
    const size_t residual = (m_bufferLen + 1) % 64;
    const size_t padBytes =
        residual <= 56 ? (56 - residual) : (56 + 64 - residual);

    // tail now holds [0x80, padBytes zero bytes, 8 length bytes].
    const size_t tailLen = 1 + padBytes + 8;

    // The 64-bit length field lives in the last 8 bytes of the tail.
    beStore64(tail + tailLen - 8, m_bitCount);

    update(tail, tailLen);

    // update() has driven processBlock for us. m_bufferLen should be
    // back to zero because tailLen is always a multiple of 64 minus
    // whatever's in the buffer — arithmetic: before update, the
    // buffer holds `initialLen` bytes; we add `1 + padBytes + 8`
    // bytes such that `initialLen + 1 + padBytes == 0 mod 64`. So
    // the total `initialLen + tailLen` is 64 + 8 = ... wait no,
    // `initialLen + 1 + padBytes + 8 == 64 or 128`. Either way,
    // the end state is buffer empty.

    uint8_t digest[32];
    for (int i = 0; i < 8; ++i) {
        beStore32(digest + i * 4, m_state[i]);
    }

    for (size_t i = 0; i < 32; ++i) m_cachedDigest[i] = digest[i];
    m_finalized = true;
    return m_cachedDigest;
}

// ── One-shot ──────────────────────────────────────────────────────

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t size) {
    Sha256Hasher h;
    h.update(data, size);
    return h.finalize();
}

// ── Hex helpers ───────────────────────────────────────────────────

std::string digestToHex(const std::array<uint8_t, 32>& digest) {
    static const char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2    ] = kHexDigits[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHexDigits[ digest[i]       & 0xF];
    }
    return out;
}

bool digestFromHex(const std::string& hex, std::array<uint8_t, 32>& out) {
    if (hex.size() != 64) return false;
    auto parseNibble = [](char c, uint8_t& nib) -> bool {
        if (c >= '0' && c <= '9') { nib = static_cast<uint8_t>(c - '0');      return true; }
        if (c >= 'a' && c <= 'f') { nib = static_cast<uint8_t>(c - 'a' + 10); return true; }
        if (c >= 'A' && c <= 'F') { nib = static_cast<uint8_t>(c - 'A' + 10); return true; }
        return false;
    };
    for (size_t i = 0; i < 32; ++i) {
        uint8_t hi = 0, lo = 0;
        if (!parseNibble(hex[i * 2],     hi)) return false;
        if (!parseNibble(hex[i * 2 + 1], lo)) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

} // namespace sv
