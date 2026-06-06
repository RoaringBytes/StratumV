// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── Sha256 ────────────────────────────────────────────────────────
// Pure C++ SHA-256 implementation (FIPS 180-4). Used by the asset
// sync path to produce content-addressable hashes for uploaded
// assets. Lives in the core subset so the Linux dedicated-server
// carve-out can use it without pulling in OpenSSL at the core
// boundary (MsQuicTransport already depends on OpenSSL on Linux,
// but that is a platform-specific gated dependency — Sha256 is
// portable).
//
// ── Why roll our own? ──────────────────────────────────────────────
// SHA-256 is ~150 lines of pure arithmetic. Adding an OpenSSL or
// libcrypto dependency to the core subset just for one hash would
// blow up the dependency surface for every consumer. BLAKE3 would
// be faster but requires a third-party build target. SHA-256 is
// well-known, well-tested, and plenty fast for a collaborative
// editing session's asset upload cadence (typically <1 MiB at a
// time, far below the throughput ceiling of a pure-C++ scalar
// implementation).
//
// ── API shape ──────────────────────────────────────────────────────
// Two entry points: a one-shot `sha256` helper for contiguous
// buffers, and an incremental `Sha256Hasher` for callers that
// feed bytes in chunks (the asset upload path walks a file in
// 64 KiB slices). Both produce the same 32-byte digest.
//
// Callers should wrap the digest in `AssetHash` (defined in
// AssetPersistence.h) for strong typing. This header keeps a raw
// `std::array<uint8_t, 32>` to stay dependency-free.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sv {

// ── One-shot SHA-256 ──────────────────────────────────────────────
// Computes the SHA-256 digest of `data[0..size)` and returns the
// 32-byte result. `data` may be null iff `size` is zero (in which
// case the returned digest is the well-known empty-input digest
// e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855).
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t size);

// ── Incremental hasher ────────────────────────────────────────────
// FIPS 180-4 message block size is 64 bytes; the hasher buffers
// partial blocks between `update` calls. `finalize` flushes the
// last block + length padding and is idempotent (calling it twice
// returns the same digest). `reset` reverts to the fresh state.
class Sha256Hasher {
public:
    Sha256Hasher();

    // Feed `len` bytes into the hash. Safe to call with len == 0.
    void update(const uint8_t* data, size_t len);

    // Flush padding + length and write the 32-byte digest into
    // `out`. Idempotent.
    std::array<uint8_t, 32> finalize();

    // Reset to the initial state so the hasher can be reused.
    void reset();

private:
    void processBlock(const uint8_t* block);

    uint32_t m_state[8];        // H0..H7
    uint64_t m_bitCount = 0;    // total message length in bits
    uint8_t  m_buffer[64]{};    // partial-block scratch
    size_t   m_bufferLen = 0;   // bytes currently buffered (<= 64)
    bool     m_finalized = false;
    std::array<uint8_t, 32> m_cachedDigest{};
};

// ── Hex helpers ───────────────────────────────────────────────────
// Convert a 32-byte digest to / from a 64-char lowercase hex
// string. `fromHex` returns false if the input is not exactly
// 64 ASCII hex characters. The parser is case-insensitive.
std::string digestToHex(const std::array<uint8_t, 32>& digest);
bool        digestFromHex(const std::string& hex,
                          std::array<uint8_t, 32>& out);

} // namespace sv
