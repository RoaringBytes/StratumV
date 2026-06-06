// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── WorldPersistence ──────────────────────────────────────────────
// Disk-backed snapshot of the dedicated server's authoritative
// entity map. Used by `stratumv_server` to preserve world state
// across restarts so clients reconnecting to a freshly-started
// server see the same entity positions they left behind.
//
// ── Why a new module instead of extending SceneStatePersistence? ──
// SceneStatePersistence is a Layer 4 graphics-adjacent persistence
// path that writes post-process / world-clock / camera settings
// as JSON. It depends on the full `stratumv.lib` (nlohmann::json +
// game-side registration), which would defeat the Linux dedicated-
// server carve-out. WorldPersistence lives in the core subset via
// `cmake/stratumv_core_sources.cmake` so it compiles on both flavors
// without pulling graphics dependencies. It uses a small bespoke
// binary format instead of JSON to stay dependency-light.
//
// ── File format (little-endian) ───────────────────────────────────
//
//   Header (32 bytes):
//     Offset  Size  Field
//     0       8     magic        "SVWLD001" (ASCII)
//     8       4     version      u32 — currently 1
//     12      4     entityCount  u32
//     16      4     nextEntityId u32
//     20      4     nextClientId u32
//     24      8     nextTxId     u64
//
//   For each entity:
//     [u32   entityId]
//     [u8    authority]        — matches sv::Authority enum byte
//     [u32   ownerClientId]
//     [u32   typeNameHash]     — fnv1a32(component typeName)
//     [u16   labelLen]
//     [byte* label[labelLen]]
//     [u32   payloadLen]
//     [byte* payload[payloadLen]] — encodeSnapshot full-mask output
//
//   The payload is produced by calling the same generic
//   EditTransaction::writeGenericSetFieldPayload helper the wire
//   layer uses, so loaded entities are serialised via the
//   ReplicationRegistry path. Any type registered via SV_REPLICATE
//   round-trips through the file.
//
// ── Version handling ──────────────────────────────────────────────
// The loader refuses any file whose magic is not "SVWLD001" or
// whose version is not 1. On failure it returns
// WorldPersistenceStatus::CorruptHeader or UnsupportedVersion;
// callers log the status and start with a fresh empty world. The
// server uses MissingFile as a "no state to restore, start fresh"
// signal — it is NOT a failure.
//
// ── Thread safety ──────────────────────────────────────────────────
// File I/O is synchronous and happens inside the server's main
// loop (on SIGINT or at a 30s cadence). The helpers take a locked
// snapshot of the entity map in `PersistedWorld`, so callers only
// have to serialise access at the read/assemble site.

#include "NetTransform.h"
#include "ReplicationRegistry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sv {

// ── Status codes ──────────────────────────────────────────────────
// Returned from every entry point. Callers map these to engine-log
// levels: Ok/MissingFile are info; everything else is a warn.
enum class WorldPersistenceStatus : uint8_t {
    Ok                 = 0,
    MissingFile        = 1,   // file does not exist — fresh start
    IoError            = 2,   // fopen/read/write failed
    CorruptHeader      = 3,   // magic/version mismatch or short read
    UnsupportedVersion = 4,   // header valid but version > 1
    PayloadDecodeFail  = 5,   // per-entity payload decode refused
    UnknownType        = 6,   // typeNameHash not in local registry
};

const char* worldPersistenceStatusToString(WorldPersistenceStatus s);

// ── PersistedEntity POD ───────────────────────────────────────────
// Single-entity record shared by the server and the persistence
// layer. Keeps the persistence module out of the server's
// ReplicatedEntity struct so the header stays core-compatible.
// `payload` is the full-mask encodeSnapshot buffer for the
// component described by typeNameHash.
struct PersistedEntity {
    uint32_t             entityId      = 0;
    uint8_t              authority     = 0;   // sv::Authority raw byte
    uint32_t             ownerClientId = 0;
    uint32_t             typeNameHash  = 0;
    std::string          label;
    std::vector<uint8_t> payload;              // generic SetField payload bytes
};

// ── PersistedWorld POD ────────────────────────────────────────────
// Whole-world snapshot assembled by the caller before saveWorldToFile,
// or produced by loadWorldFromFile for the caller to rehydrate into
// its in-memory state.
struct PersistedWorld {
    uint32_t                       nextEntityId = 100;
    uint32_t                       nextClientId = 1;
    uint64_t                       nextTxId     = 1;
    std::vector<PersistedEntity>   entities;
};

// ── Wire layout constants ─────────────────────────────────────────
constexpr const char*   kWorldFileMagic       = "SVWLD001";
constexpr size_t        kWorldFileMagicLen    = 8;
constexpr uint32_t      kWorldFileVersion     = 1;
constexpr size_t        kWorldFileHeaderSize  = 32;

// ── Save: write world to file ─────────────────────────────────────
// Emits the header + per-entity records. Returns Ok on success,
// IoError on any failed fwrite. The file is written atomically via a
// temporary-file + rename swap so an interrupted save never leaves a
// partially-written file at the target path.
WorldPersistenceStatus saveWorldToFile(const std::string&    filePath,
                                       const PersistedWorld& world);

// ── Load: read world from file ────────────────────────────────────
// Returns MissingFile if the path does not exist. Returns
// CorruptHeader if the magic/version bytes are wrong, or if any
// fixed-size field reads short. Returns UnknownType if a
// typeNameHash is not in the local ReplicationRegistry. On Ok the
// outWorld is fully populated; on any error outWorld is left at its
// default-constructed state.
WorldPersistenceStatus loadWorldFromFile(const std::string& filePath,
                                         PersistedWorld&    outWorld);

// ── In-memory round-trip helper ───────────────────────────────────
// Used by tests and the server for sanity checks. Encodes a world
// directly into a byte buffer (same format as the file), then
// decodes it back. Not a primary runtime path.
WorldPersistenceStatus encodeWorldToBytes(const PersistedWorld& world,
                                          std::vector<uint8_t>& out);

WorldPersistenceStatus decodeWorldFromBytes(const uint8_t*  data,
                                            size_t          size,
                                            PersistedWorld& outWorld);

} // namespace sv
