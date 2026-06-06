// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── AssetPersistence ────────────────────────────────────
// Server-side content-addressable store (CAS) for collaborative
// asset sync. Each known asset is keyed by its SHA-256 digest; the
// primary data lives in an in-memory cache (always) with an optional
// on-disk mirror at `<server-data>/assets/<2hex>/<rest>.bin`.
//
// Used by `stratumv_server` and by the lab test harness for the
// receiver-side replicated-asset cache. Pure logic — no MsQuic, no
// graphics — so it lives in the core subset alongside EditTransaction
// and WorldPersistence.
//
// ── Why both in-memory AND on-disk? ─────────────────────────────
// In-memory means the server can serve broadcasts on the first
// reliable stream write without blocking on disk reads. On-disk
// means assets persist across restarts and survive a crash without
// having to re-upload from every client. The interplay matches the
// `--server-data` flag: running the server ephemeral keeps only the
// in-memory cache; adding `--server-data DIR` enables write-through
// persistence at `<DIR>/assets/`. Existing assets in the target
// directory are loaded into memory at startup.
//
// ── File layout on disk ───────────────────────────────────────────
//
//   <root>/<2hex>/<62hex>.bin        raw asset bytes
//   <root>/<2hex>/<62hex>.meta.json  {"kind":N,"name":"...","size":N}
//
// Files are written atomically via temp + rename so an interrupted
// save never leaves a half-written asset at the keyed path.
//
// ── Hash verification ────────────────────────────────────────────
// `save()` verifies the SHA-256 of the provided bytes against the
// declared hash BEFORE writing. A mismatch returns HashMismatch and
// writes nothing — prevents a malicious or corrupted client from
// poisoning the CAS under a chosen hash.

#include "Sha256.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sv {

// Type alias so call sites don't carry the std::array<...> spelling.
using AssetHash = std::array<uint8_t, 32>;

// ── Status codes ──────────────────────────────────────────────────
enum class AssetPersistenceStatus : uint8_t {
    Ok             = 0,
    BadArg         = 1,   // null pointer, zero-length where non-zero required
    HashMismatch   = 2,   // declared hash != actual sha256(bytes)
    MissingFile    = 3,   // load() called on unknown hash
    IoError        = 4,   // open / write / rename failed
    CorruptHeader  = 5,   // meta.json missing, unreadable, or invalid
    SizeExceeded   = 6,   // byteSize > kAssetByteLimit
};

const char* assetPersistenceStatusToString(AssetPersistenceStatus s);

// ── AssetRecord ───────────────────────────────────────────────────
// In-memory representation of a cached asset. The `bytes` vector
// owns the full content. For assets read from disk, the bytes are
// copied into memory at load time (no lazy-mmap).
struct AssetRecord {
    AssetHash            hash{};
    uint32_t             byteSize  = 0;
    uint8_t              assetKind = 0;
    std::string          name;
    std::vector<uint8_t> bytes;
};

// ── Hashing helper ────────────────────────────────────────────────
// Convenience wrapper around sv::sha256 — keeps the caller free of
// the raw std::array spelling.
AssetHash hashAssetBytes(const uint8_t* data, size_t size);

// ── File path helpers ─────────────────────────────────────────────
// pathForHash("<root>", hash) returns
//   <root>/<2 hex>/<62 hex>.bin
// Empty rootDir yields an empty string.
std::string assetFilePath(const std::string& rootDir, const AssetHash& hash);
std::string assetMetaPath(const std::string& rootDir, const AssetHash& hash);

// ── AssetPersistence ──────────────────────────────────────────────
// Primary server-side interface. Thread-safety is the caller's
// responsibility — stratumv_server drains asset messages on the
// main thread, so no internal mutex is needed.
class AssetPersistence {
public:
    AssetPersistence() = default;
    ~AssetPersistence() = default;

    AssetPersistence(const AssetPersistence&) = delete;
    AssetPersistence& operator=(const AssetPersistence&) = delete;

    // Enable on-disk write-through at `rootDir`. Creates the
    // directory tree (including the 2-hex shard parents) if needed.
    // Scans the root for existing `.bin` files and pulls each of
    // them into the in-memory cache. Returns Ok if the root is
    // usable. Passing an empty string clears any previous root and
    // keeps only the in-memory cache (does NOT drop already-loaded
    // bytes).
    AssetPersistenceStatus setRootDir(const std::string& rootDir);

    const std::string& rootDir() const { return m_rootDir; }

    // True if `hash` is in the in-memory cache.
    bool contains(const AssetHash& hash) const;

    // Save bytes keyed by `hash`. Verifies SHA-256(bytes) ==
    // `hash` before storing. On hash match: writes to memory,
    // then (if rootDir is set) writes `<root>/<2hex>/<62hex>.bin`
    // and a sibling `.meta.json` atomically. Returns Ok.
    // On mismatch: returns HashMismatch and nothing is written.
    AssetPersistenceStatus save(const AssetHash&   hash,
                                uint8_t            assetKind,
                                const std::string& name,
                                const uint8_t*     bytes,
                                size_t             byteSize);

    // Retrieve a cached asset into `out`. Returns MissingFile if
    // the hash is unknown. The returned bytes are a copy; the
    // AssetPersistence retains its own authoritative copy.
    AssetPersistenceStatus load(const AssetHash& hash,
                                AssetRecord&     out) const;

    // Pointer to the in-memory record, or nullptr if unknown.
    // The pointer is valid until the next save() / setRootDir().
    const AssetRecord* find(const AssetHash& hash) const;

    // Number of assets currently in memory.
    size_t size() const { return m_records.size(); }
    bool   empty() const { return m_records.empty(); }

    // Read-only access for diagnostics + the test suite. Keys are
    // lowercase 64-char hex strings (digestToHex output).
    const std::unordered_map<std::string, AssetRecord>& records() const {
        return m_records;
    }

    // Drop every cached record from memory. Does NOT touch disk.
    // Used by tests to reset the cache between cases.
    void clear();

private:
    // Write `rec` to disk atomically at `<root>/<2hex>/<62hex>.bin`
    // + sibling `.meta.json`. Returns true on success.
    bool writeToDisk(const AssetRecord& rec) const;

    // Scan `m_rootDir` for .bin/.meta.json pairs and populate the
    // in-memory cache. Called from `setRootDir`.
    void scanRootDir();

    std::string                                    m_rootDir;
    std::unordered_map<std::string, AssetRecord>   m_records;  // key = hex(hash)
};

} // namespace sv
