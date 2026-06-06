// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── EditTransaction ─────────────────────────────────────────────────
// Minimal collaborative-editing transaction type carried on the
// reliable QUIC stream alongside the schema handshake preamble. Every
// mutation to a replicated component goes through one of these; the
// server is the sole authority for applying, logging, and
// broadcasting them.
//
// ── Kind taxonomy ──────────────────────────────────────────────────
//
//   SetField (0): client-requested mutation of a replicated
//                 component on a replicated entity. The server
//                 validates scope + ownership, applies the new
//                 state, pushes the transaction onto the UndoLog,
//                 and broadcasts the resulting snapshot via the
//                 normal datagram path. There is NO rebroadcast of
//                 the transaction itself — the datagram snapshot is
//                 sufficient for all connected clients to see the
//                 new state. A per-client transaction log mirror is
//                 a separate feature.
//
//   Undo (1):     client-requested rollback of the latest SetField
//                 they authored. The server walks the UndoLog for
//                 entries originated by this client, marks the
//                 latest non-undone one undone, applies the
//                 before-state, and emits a fresh datagram
//                 snapshot reflecting the rollback.
//
//   Redo (2):     inverse of Undo. Walks for the latest undone
//                 entry from this client and re-applies its
//                 after-state.
//
//   Spawn (3):    server-originated. Appears on the reliable stream
//                 so clients can populate their local entity list.
//                 Sent by the server (a) once per existing entity
//                 to a newly-joined client during world-sync and
//                 (b) once to every already-connected client when
//                 a new entity appears. Payload carries the
//                 initial NetTransform state + ownerClientId.
//
//   Despawn (4):  server-originated. Sent once per existing client
//                 when an entity goes away (e.g. an owner
//                 disconnects). Payload is empty — clients look up
//                 the entity by entityId and remove it.
//
// ── Scope model ────────────────────────────────────────────────────
//
// Every transaction carries a `requiredScope`. The server validates
// `originScope >= requiredScope` before applying. Default scopes:
//   SetField on a NetTransform:        Editor (default scope)
//   Undo / Redo:                       Editor
//   Spawn / Despawn:                   Admin (server-only in practice)
//
// Client-supplied scope/origin fields on an inbound transaction are
// OVERWRITTEN by the server before any action is taken — clients
// cannot forge their clientId nor their scope. The server-side
// dispatcher in stratumv_server/main.cpp handles the rewrite.
//
// ── Wire layout (little-endian, one reliable-stream message) ────────
//
//   Offset  Size  Field
//   0       1     msgType        (u8)  — kFrameEditTransaction (= 3)
//   1       1     kind           (u8)  — EditKind enum value
//   2       8     txId           (u64) — server-assigned monotonic id
//   10      4     originClientId (u32) — 0 = server-originated
//   14      1     requiredScope  (u8)  — PermissionScope enum
//   15      4     entityId       (u32)
//   19      4     typeNameHash   (u32) — fnv1a32(component typeName)
//   23      8     timestampMs    (u64) — server wall clock
//   31      2     payloadLen     (u16) — 0..65535
//   33      P     payload        (u8[P])
//
// Total header overhead: 33 bytes. Max payload: 65535 (limited by
// the u16 length). Practical SetField payloads are 28
// bytes; Spawn payloads are 32.
//
// Payload by kind (generalised from NetTransform-only via
// the ReplicationRegistry meta dispatch path — see writeGeneric*
// helpers below):
//
//   SetField: generic encodeSnapshot output = [u16 schemaVersion]
//             [byte-packed DirtyMask][dirty field values]. Any
//             component type registered via SV_REPLICATE flows here
//             as long as its typeNameHash matches the transaction's
//             typeNameHash field.
//   Undo:     8 bytes target txId (u64, little-endian) — server
//             fills this in on broadcast, empty on client request
//   Redo:     8 bytes target txId (same convention as Undo)
//   Spawn:    [u32 ownerClientId][encodeSnapshot full-mask bytes].
//             A future change that drops the legacy call sites
//             may merge ownerClientId into the snapshot itself, but
//             the current code keeps the prefix so the server can
//             read the owner without dispatching through the registry.
//   Despawn:  empty
//
// The legacy helpers (writeNetTransformLE/readNetTransformLE/
// writeSpawnPayload/readSpawnPayload) still exist as thin raw-byte
// codecs because the wire-layout tests use them; the server and lab
// client now go through the generic path. See the "legacy" block at
// the bottom of this header.

#include "NetTransform.h"
#include "PermissionScope.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace sv {
struct ReplicationMeta;
class  DirtyMask;
}

namespace sv {

enum class EditKind : uint8_t {
    SetField = 0,
    Undo     = 1,
    Redo     = 2,
    Spawn    = 3,
    Despawn  = 4,
};

const char* editKindToString(EditKind k);

struct EditTransaction {
    uint64_t        txId            = 0;
    EditKind        kind            = EditKind::SetField;
    uint32_t        originClientId  = 0;
    PermissionScope requiredScope   = PermissionScope::Editor;
    uint32_t        entityId        = 0;
    uint32_t        typeNameHash    = 0;
    uint64_t        timestampMs     = 0;
    std::vector<uint8_t> payload;
};

// Wire overhead / raw NetTransform blob sizes. Declared as
// constexpr so the server's buffer sizing and the tests can both
// static_assert against them without reaching into the .cpp.
constexpr size_t kEditTransactionHeaderSize = 33;
constexpr size_t kNetTransformWireSize      = 28;
constexpr size_t kSpawnPayloadSize          = kNetTransformWireSize + 4;

// ── Wire encode / decode ────────────────────────────────────────────

// Build the 33-byte header + append the already-populated payload
// onto `out`. `out` is cleared first. Returns false if payload size
// overflows the u16 ceiling (this never happens because the
// fixed payload sizes are tiny).
bool encodeEditTransaction(const EditTransaction& tx,
                           std::vector<uint8_t>&  out);

// Parse a single reliable-stream message that begins with
// kFrameEditTransaction. Returns nullopt on any of:
//   * data==nullptr or size below header
//   * leading byte is not kFrameEditTransaction (msgType = 3)
//   * declared payload length overruns the buffer
std::optional<EditTransaction> parseEditTransaction(const uint8_t* data,
                                                    size_t         size);

// ── Generic payload helpers ─────────────────────────────────────────
//
// These generalise the SetField / Spawn payload dispatch to any
// type registered via SV_REPLICATE. The helpers look up the
// ReplicationMeta by `meta` (for encode) or by `typeNameHash`
// against the global ReplicationRegistry (for decode), then walk
// the encodeSnapshot / decodeSnapshot path.
//
// Wire layout of the SetField payload (byte-for-byte what
// encodeSnapshot emits):
//
//   [u16 schemaVersion]
//   [byte-packed DirtyMask : ceil(fieldCount/8) bytes, LSB-first]
//   [dirty field values in declaration order]
//
// Wire layout of the Spawn payload:
//
//   [u32 ownerClientId]
//   [u16 schemaVersion]
//   [byte-packed DirtyMask]
//   [full-mask field values]
//
// Why the ownerClientId prefix? The server needs the owner field to
// populate its entity map at join-with-snapshot time without walking
// the field list — and the server's join-with-snapshot code path
// does NOT know the component type layout. Keeping the prefix means
// a single `readU32LE` is enough to recover the owner, then the
// remaining bytes flow through the generic decode path.

// Encode a SetField payload by dispatching through ReplicationMeta.
// `mask` must be the same size as meta.fieldCount(). Appends to
// `out` without clearing. Returns false on null instance, mask
// size mismatch, or unsupported field type (encodeSnapshot already
// rejects those).
bool writeGenericSetFieldPayload(const ReplicationMeta& meta,
                                 const void*            instance,
                                 const DirtyMask&       mask,
                                 std::vector<uint8_t>&  out);

// Decode a SetField payload. Looks up the type by `typeNameHash`
// against the global ReplicationRegistry; if the type is not
// registered, returns false. On success fills `outInstance` and
// `outMask` per decodeSnapshot semantics. The caller is
// responsible for passing an `outInstance` matching the type.
bool readGenericSetFieldPayload(uint32_t         typeNameHash,
                                const uint8_t*   data,
                                size_t           size,
                                void*            outInstance,
                                DirtyMask&       outMask);

// Encode a full-mask Spawn payload: 4-byte ownerClientId prefix
// followed by the generic SetField payload for `instance`.
bool writeGenericSpawnPayload(const ReplicationMeta& meta,
                              const void*            instance,
                              uint32_t               ownerClientId,
                              std::vector<uint8_t>&  out);

// Decode a Spawn payload. Reads the ownerClientId prefix, then
// dispatches to readGenericSetFieldPayload for the remainder.
bool readGenericSpawnPayload(uint32_t         typeNameHash,
                             const uint8_t*   data,
                             size_t           size,
                             uint32_t&        outOwnerClientId,
                             void*            outInstance,
                             DirtyMask&       outMask);

// ── NetTransform payload helpers (legacy) ───────────────────────────
//
// The original code shipped NetTransform-specific raw helpers; the
// current code keeps them for the wire-layout tests and as reference
// codecs, but the server and lab client now dispatch through the
// generic path above.
// These helpers write a fixed 28-byte NetTransform blob without any
// schema version or dirty mask — they are NOT compatible with the
// generic payload format.

// Serialize a NetTransform as 28 bytes of little-endian IEEE-754
// floats (posX, posY, posZ, rotX, rotY, rotZ, rotW). Appends to
// `out` without clearing.
void writeNetTransformLE(const NetTransform& t, std::vector<uint8_t>& out);

// Parse 28 bytes starting at `data`. Returns nullopt if the buffer
// is too short. Endianness matches writeNetTransformLE.
std::optional<NetTransform>
readNetTransformLE(const uint8_t* data, size_t size);

// Spawn payload: 28 bytes NetTransform + 4 bytes u32 ownerClientId,
// little-endian.
void writeSpawnPayload(const NetTransform&    initialState,
                       uint32_t               ownerClientId,
                       std::vector<uint8_t>&  out);

// Parse a spawn payload. Returns false on short buffer.
bool readSpawnPayload(const uint8_t* data,
                      size_t         size,
                      NetTransform&  outState,
                      uint32_t&      outOwnerClientId);

} // namespace sv
