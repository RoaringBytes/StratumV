// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── ReplicationProtocol ───────────────────────────────────────────────
// Minimal wire framing for datagram-based component replication.
// Sits above the MsQuic transport (sv::net::Connection::sendDatagram)
// and below the ReplicationRegistry snapshot encoder. Defines a single
// frame type — SNAPSHOT — that carries one full (or delta-dirty)
// encodeSnapshot payload per datagram. Bigger message types (edit
// transactions, reliable join snapshots, asset chunks) build on top.
//
// ── Frame layout (little-endian, fits in one QUIC datagram) ──────────
//
//   Offset  Size  Field
//   0       1     msgType  (u8)     — kFrameSnapshot
//   1       4     tickIndex (u32)   — monotonic server tick counter
//   5       4     entityId (u32)    — caller-defined id (0..N-1)
//   9       4     typeNameHash(u32) — ReplicationMeta::typeNameHash
//   13      2     payloadLen (u16)  — encodeSnapshot output size in bytes
//   15      P     payload  (u8[P])  — encodeSnapshot buffer (schemaVersion +
//                                     mask + dirty fields)
//
// Total overhead: 15 bytes. Max payload: 65535 (u16), well above what
// QUIC datagrams can carry in practice — MsQuic negotiates ~1200-1350
// per datagram on loopback, and practical NetTransform payloads are
// ~30-40 bytes.
//
// ── Design choices ───────────────────────────────────────────────────
//
// Why entity id on the wire? Even when only one entity is replicated
// (e.g. a server-owned cube), the protocol header already carries it
// so clients can branch on "is this the cube I'm tracking" without
// changing the header format when multiple entities start flowing.
//
// Why typeNameHash on the wire? So the client can (a) reject frames
// for unknown components and (b) look up the ReplicationMeta for
// decodeSnapshot. 32-bit collision risk is negligible with <1000
// distinct component types per session.
//
// Why one component per frame instead of batching? Simpler wire
// format, simpler lost-datagram tolerance. A lost batch loses every
// entity in it; one-per-datagram isolates losses. A future revision
// can batch multiple components per datagram once interest management
// lands.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sv {

// Forward declarations. NOTE: `ReplicationMeta` is declared as a
// `struct` in ReplicationRegistry.h, `DirtyMask` as a `class` —
// MSVC's C++ ABI mangles the struct/class tag into the decorated
// name, so a mismatch here silently breaks the linker on that
// compiler. Keep these in sync with ReplicationRegistry.h.
struct ReplicationMeta;
class  DirtyMask;

namespace net {

// ── Wire message types ────────────────────────────────────────────────
// `kFrameSnapshot` carries datagram snapshots. `kFrameSchemaHandshake`
// is sent once per accepted connection on a reliable QUIC stream right
// after the TLS handshake completes. `kFrameEditTransaction` carries
// collaborative editing transactions on the reliable stream, one
// message per transaction, and `kFrameWelcome` is a tiny per-connection
// "here is your identity" message that carries the server-assigned
// clientId, permission scope, and avatar entity id. Additional message
// types (JOIN_SNAPSHOT, HEARTBEAT) are reserved for future growth.
enum : uint8_t {
    kFrameSnapshot        = 1,   // datagram snapshot
    kFrameSchemaHandshake = 2,   // stream preamble
    kFrameEditTransaction = 3,   // collaborative edit tx
    kFrameWelcome         = 4,   // per-connection identity
    kFrameAssetAnnounce   = 5,   // asset sync announce
    kFrameAssetChunk      = 6,   // asset sync chunk payload
    kFrameAssetAck        = 7,   // asset sync have/need ack
};

// ── Application-level QUIC close codes ───────────────────────────────
// Passed to Connection::shutdown(errorCode) and surfaced on the peer
// via SHUTDOWN_INITIATED_BY_PEER. Keep the namespace small and stable
// so consumer apps can branch on them without parsing strings.
constexpr uint64_t kErrSchemaMismatch = 1001;
constexpr uint64_t kErrScopeDenied    = 1002;  // reserved — server
                                                // logs + ignores today

// Wire overhead in bytes — used to size send buffers and to validate
// incoming datagrams before the payload walk.
constexpr size_t kFrameHeaderSize = 15;

// Parsed header view — the byte ranges inside an inbound datagram.
// parseSnapshotFrame fills one of these; callers then walk the
// payload via sv::decodeSnapshot.
struct SnapshotFrame {
    uint32_t       tickIndex    = 0;
    uint32_t       entityId     = 0;
    uint32_t       typeNameHash = 0;
    uint16_t       payloadLen   = 0;
    const uint8_t* payload      = nullptr;   // borrows from input buffer
};

// ── Encode ────────────────────────────────────────────────────────────
// Build a SNAPSHOT frame by invoking the snapshot encoder on `instance`
// with `mask`, then prepending the 15-byte header. On success, `out`
// holds the full framed payload (header + snapshot bytes) and the
// function returns true. On failure (null instance, null meta,
// encodeSnapshot refused the DirtyMask size, payload > 65535 bytes)
// `out` is left empty and the function returns false.
//
// The caller passes the finished buffer straight to
// Connection::sendDatagram(out.data(), out.size()).
bool encodeSnapshotFrame(uint32_t                 tickIndex,
                         uint32_t                 entityId,
                         const ReplicationMeta&   meta,
                         const void*              instance,
                         const DirtyMask&         mask,
                         std::vector<uint8_t>&    out);

// ── Decode ────────────────────────────────────────────────────────────
// Parse a raw datagram into a SnapshotFrame header view. Returns the
// parsed header on success, std::nullopt if:
//   * size < kFrameHeaderSize
//   * first byte is not kFrameSnapshot
//   * declared payloadLen overruns the input buffer
//
// The SnapshotFrame::payload pointer borrows from `data`; callers that
// want to keep the bytes beyond the lifetime of the datagram must
// copy them out. The stratumv_server / lab client paths decode
// immediately on the main thread, so borrowing is safe.
std::optional<SnapshotFrame> parseSnapshotFrame(const uint8_t* data,
                                                size_t          size);

// ── Apply to a live component ────────────────────────────────────────
// Convenience wrapper: parse the frame, look up the ReplicationMeta
// by typeNameHash, and call decodeSnapshot into `outInstance`. Returns
// true on full success, false on any of: header parse fail, unknown
// type hash, schema version mismatch, decodeSnapshot EOF.
//
// The caller is responsible for passing an outInstance that matches
// the frame's typeNameHash — the helper does not introspect the
// instance, it only walks the registered meta. If the mismatch is
// possible (multiple component types on one connection), gate the
// call on SnapshotFrame::typeNameHash first.
bool applySnapshotFrame(const SnapshotFrame& frame,
                        void*                outInstance,
                        DirtyMask&           outMask);

// ── Round-trip helper (tests + lab harness) ──────────────────────────
// Packs a snapshot, then immediately parses it back — useful for
// single-process round-trip tests that don't need MsQuic. Returns
// false on any intermediate failure.
bool roundTripSnapshot(uint32_t               tickIndex,
                       uint32_t               entityId,
                       const ReplicationMeta& meta,
                       const void*            srcInstance,
                       const DirtyMask&       srcMask,
                       void*                  dstInstance,
                       DirtyMask&             dstMask);

// ── Schema handshake preamble ────────────────────────────────────────
// Sent once per accepted connection on a reliable QUIC unidirectional
// stream right after the TLS handshake completes. The server emits it;
// the client parses it, compares against its local ReplicationRegistry,
// and closes the connection with `kErrSchemaMismatch` on any type that
// is present in both registries with a different schemaVersion.
//
// Wire layout (little-endian, exactly one stream message, no FIN marker
// on the bytes — the stream's PEER_SEND_SHUTDOWN event signals end):
//
//   Offset  Size   Field
//   0       1      msgType   (u8)    — kFrameSchemaHandshake
//   1       4      semver    (u32)   — (major<<16)|(minor<<8)|patch
//   5       2      typeCount (u16)   — number of entries
//   7       6*N    entries           — repeat of:
//                                        [u32 typeNameHash]
//                                        [u16 schemaVersion]
//
// The frame's total byte count is 7 + 6 * typeCount. For a registry
// with one NetTransform type that's 13 bytes, which is well under
// MsQuic's default stream window.
//
// Why stream, not datagram? QUIC datagrams are unreliable and can
// reorder with snapshot datagrams; the preamble must arrive intact,
// and losing it must not silently break schema validation. Streams
// are reliable + ordered within themselves. One stream per preamble
// keeps the wire format trivially framed (PEER_SEND_SHUTDOWN = "all
// bytes delivered") without any length prefix inside the stream.
//
// Why schemaVersion and not a stronger hash? The registry's
// schemaVersion is a 16-bit FNV fold over field name+type sequence —
// enough to detect meaningful drift (rename, add, delete, retype)
// with negligible collision risk for the <1000 component types any
// one session ever has. The full 32-bit typeNameHash disambiguates
// the type identity; schemaVersion disambiguates the type shape.

struct SchemaHandshakeEntry {
    uint32_t typeNameHash  = 0;
    uint16_t schemaVersion = 0;
};

struct SchemaHandshake {
    // Packed StratumV semver at the sender: (major<<16)|(minor<<8)|patch.
    // Informational — the preamble comparison does NOT use semver for
    // accept/reject, because two engine versions can ship matched
    // schemas, and one engine version can ship drifted schemas if a
    // developer renames a field without a rebuild. The schema table is
    // the authoritative check.
    uint32_t semver = 0;

    // Sorted by typeNameHash ascending. See
    // ReplicationRegistry::getSchemaTable().
    std::vector<SchemaHandshakeEntry> types;
};

// Total on-wire size for a handshake with `typeCount` entries.
// Header overhead is 7 bytes; each entry is 6 bytes.
constexpr size_t kSchemaHandshakeHeaderSize = 7;
constexpr size_t kSchemaHandshakeEntrySize  = 6;
inline constexpr size_t schemaHandshakeSize(size_t typeCount) {
    return kSchemaHandshakeHeaderSize + typeCount * kSchemaHandshakeEntrySize;
}

// Build the preamble bytes from a SchemaHandshake value. Returns true
// on success, false if the type count would overflow the u16 header.
// On failure `out` is left empty.
bool encodeSchemaHandshake(const SchemaHandshake& hs,
                           std::vector<uint8_t>&  out);

// Parse preamble bytes. Returns true on success and fills `out`.
// Returns false if the message is malformed:
//   * null pointer / size less than kSchemaHandshakeHeaderSize
//   * leading byte is not kFrameSchemaHandshake
//   * typeCount * entry size overruns the buffer
// On failure `out` is left in its default-initialised state.
bool parseSchemaHandshake(const uint8_t*    data,
                          size_t            size,
                          SchemaHandshake&  out);

// Pack a SemVer triple into the u32 field used by SchemaHandshake::semver.
inline constexpr uint32_t packSemver(uint32_t major,
                                     uint32_t minor,
                                     uint32_t patch) {
    return ((major & 0xFFu) << 16) | ((minor & 0xFFu) << 8) | (patch & 0xFFu);
}

inline constexpr uint32_t semverMajor(uint32_t packed) { return (packed >> 16) & 0xFFu; }
inline constexpr uint32_t semverMinor(uint32_t packed) { return (packed >>  8) & 0xFFu; }
inline constexpr uint32_t semverPatch(uint32_t packed) { return  packed        & 0xFFu; }

// Result of comparing a received preamble against the local registry.
// Produced by compareSchemaHandshake.
enum class SchemaCompareStatus : uint8_t {
    Ok               = 0,    // every type in preamble matches local registry
    Mismatch         = 1,    // at least one type has a drifted schemaVersion
    ServerHasUnknown = 2,    // preamble has a type the local registry does not
                             //   (soft — diagnostic only, caller may still accept)
};

struct SchemaCompareResult {
    SchemaCompareStatus status         = SchemaCompareStatus::Ok;
    uint32_t            mismatchHash   = 0;  // typeNameHash of the first
                                              //   drifted / unknown entry
    uint16_t            expectedVer    = 0;  // local registry's version
    uint16_t            receivedVer    = 0;  // preamble's version
    std::string         mismatchName;        // local registry's type name
                                              //   (empty on ServerHasUnknown)

    // For logging / UI convenience.
    const char* statusString() const;
};

// ── Welcome message ──────────────────────────────────────────────────
// Tiny per-connection identity message sent by the server on the
// reliable stream, once per accepted connection, immediately after
// the schema handshake preamble. Tells the client three things it
// cannot derive locally:
//
//   - its own clientId (server-assigned monotonic id so clients
//     can recognise their own transactions in broadcasts)
//   - its permission scope (Spectator/Player/Editor/Admin)
//   - the entityId of its server-spawned avatar (so the client
//     knows which entity to drive via edit transactions)
//
// Wire layout (little-endian, exactly one reliable stream message):
//
//   Offset  Size  Field
//   0       1     msgType        (u8)  — kFrameWelcome (= 4)
//   1       4     clientId       (u32)
//   5       1     scope          (u8)  — PermissionScope raw byte
//   6       4     avatarEntityId (u32)
//
// Total: 10 bytes. Fits trivially inside MsQuic's default stream
// window. No per-type dispatch because this message is
// connection-level metadata, not component replication.
//
// The scope byte is parsed through `permissionScopeFromByte` on
// the receiving side so an out-of-range value downgrades the
// client to Spectator rather than elevating it.

constexpr size_t kWelcomeMessageSize = 10;

struct WelcomeMessage {
    uint32_t clientId       = 0;
    uint8_t  scope          = 0;   // PermissionScope raw byte (avoids
                                    // a new include in this header)
    uint32_t avatarEntityId = 0;
};

bool encodeWelcomeMessage(const WelcomeMessage& w,
                          std::vector<uint8_t>& out);

bool parseWelcomeMessage(const uint8_t*    data,
                         size_t            size,
                         WelcomeMessage&   out);

// ── Asset sync wire messages ─────────────────────────────────────────
// Three message types carry binary assets between editor-scope
// clients and the server across the reliable QUIC stream. All three
// are one-shot reliable-stream messages, same delivery contract as
// `kFrameSchemaHandshake` / `kFrameWelcome` / `kFrameEditTransaction`.
//
// ── Upload flow (client → server) ──────────────────────────────────
//
//   1. Client hashes asset bytes with SHA-256 → 32-byte digest.
//   2. Client sends `AssetAnnounce(hash, byteSize, kind, name)`.
//   3. Server looks up `hash` in its content-addressable store:
//        - Hit:  sends `AssetAck(hash, HaveIt)` back to sender.
//                Broadcast is triggered immediately using the
//                already-cached bytes.
//        - Miss: sends `AssetAck(hash, NeedChunks)` back to sender.
//                Sender then streams `AssetChunk(hash, i, N, len, bytes)`
//                messages in order.
//   4. Once all chunks are received, server verifies the digest,
//      persists to `<server-data>/assets/<2hex>/<rest>.bin` and
//      broadcasts `AssetAnnounce + AssetChunk*` to every other
//      welcomed client.
//
// ── Broadcast flow (server → other clients) ────────────────────────
// The server-side broadcast path always pushes `Announce + Chunks`
// without waiting for an ack — the bytes are already in memory and
// the reliable stream is the cheap shared channel. Receiving clients
// dedup silently on hash hit (they already have the asset in their
// local cache).
//
// ── Wire layout (little-endian) ────────────────────────────────────
//
//   AssetAnnounce (variable size; header is 40 bytes):
//     Offset  Size  Field
//     0       1     msgType      = kFrameAssetAnnounce
//     1       32    sha256       (content hash)
//     33      4     byteSize     (u32 — full asset byte length)
//     37      1     assetKind    (u8 — sv::AssetKind raw byte or 0/"Other")
//     38      2     nameLen      (u16 — bytes of the name field)
//     40      nameLen  name      (UTF-8, forward slashes — e.g. "textures/stone.png")
//
//   AssetAck (34 bytes fixed):
//     Offset  Size  Field
//     0       1     msgType      = kFrameAssetAck
//     1       32    sha256
//     33      1     status       (u8 — AssetAckStatus raw byte)
//
//   AssetChunk (variable size; header is 45 bytes):
//     Offset  Size  Field
//     0       1     msgType      = kFrameAssetChunk
//     1       32    sha256
//     33      4     chunkIndex   (u32 — 0..chunkCount-1)
//     37      4     chunkCount   (u32 — total chunks in this asset)
//     41      4     chunkLen     (u32 — this chunk's byte count)
//     45      chunkLen  chunk    (raw bytes)
//
// ── Chunk size + limits ────────────────────────────────────────────
// `kAssetChunkSize = 65536` (64 KiB) is the default chunk size used
// by AssetUploadClient. Assets larger than `kAssetByteLimit` are
// rejected before upload; production deployments can raise the limit
// via config.

using AssetHashBytes = std::array<uint8_t, 32>;

constexpr uint32_t kAssetChunkSize       = 65536;    // 64 KiB default
constexpr uint32_t kAssetByteLimit       = 64 * 1024 * 1024; // 64 MiB per asset
constexpr size_t   kAssetAnnounceHeader  = 40;       // fixed part of Announce
constexpr size_t   kAssetChunkHeaderSize = 45;       // fixed part of Chunk
constexpr size_t   kAssetAckSize         = 34;       // fixed size Ack

struct AssetAnnounceMessage {
    AssetHashBytes hash{};
    uint32_t       byteSize  = 0;
    uint8_t        assetKind = 0;
    std::string    name;
};

enum class AssetAckStatus : uint8_t {
    NeedChunks = 0,   // sender must stream chunks
    HaveIt     = 1,   // receiver already has this hash cached
};

struct AssetAckMessage {
    AssetHashBytes hash{};
    AssetAckStatus status = AssetAckStatus::NeedChunks;
};

// ChunkMessage borrows `chunk` from the caller's buffer on parse.
// On encode the caller fills the span and the encoder copies into
// the output vector.
struct AssetChunkMessage {
    AssetHashBytes hash{};
    uint32_t       chunkIndex = 0;
    uint32_t       chunkCount = 0;
    uint32_t       chunkLen   = 0;
    const uint8_t* chunk      = nullptr;   // borrows
};

bool encodeAssetAnnounce(const AssetAnnounceMessage& a,
                         std::vector<uint8_t>&       out);
bool parseAssetAnnounce (const uint8_t*              data,
                         size_t                      size,
                         AssetAnnounceMessage&       out);

bool encodeAssetAck(const AssetAckMessage& ack,
                    std::vector<uint8_t>&  out);
bool parseAssetAck (const uint8_t*         data,
                    size_t                 size,
                    AssetAckMessage&       out);

bool encodeAssetChunk(const AssetChunkMessage& c,
                      std::vector<uint8_t>&    out);
bool parseAssetChunk (const uint8_t*           data,
                      size_t                   size,
                      AssetChunkMessage&       out);

// Compare a parsed preamble against the ReplicationRegistry singleton.
// Semantics:
//   1. For every entry in `received`:
//        - If local registry has the type (findByHash) AND the
//          schemaVersions disagree: set status=Mismatch and return
//          IMMEDIATELY with the first drift recorded. The server cannot
//          be trusted for this component type and the connection must
//          be closed with kErrSchemaMismatch.
//        - If local registry does NOT have the type: set status=
//          ServerHasUnknown and continue scanning in case a later
//          entry produces a hard Mismatch.
//   2. Entries present in the local registry but absent from `received`
//      are silently ignored — client-only types are fine, the server
//      simply won't replicate them to this client.
//
// Return value: the first `Mismatch` seen if any, else the first
// `ServerHasUnknown` if any, else `Ok`. The mismatchName field is
// looked up via ReplicationRegistry::findByHash on the client side.
SchemaCompareResult compareSchemaHandshake(const SchemaHandshake& received);

} // namespace net
} // namespace sv
