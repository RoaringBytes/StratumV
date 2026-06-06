// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── EditTransaction implementation ─────────────────────────────────
// Pure logic, no MsQuic, no Vulkan. Lives in the stratumv_core
// subset so both the full engine library and the Linux-or-Windows
// headless dedicated-server carve-out can compile this.
//
// See EditTransaction.h for the wire format description and the
// per-kind payload tables.

#include "EditTransaction.h"

#include "ReplicationRegistry.h"

#include <cstring>

namespace sv {

namespace {

// Little-endian primitive writers. These mirror the helpers in
// net/ReplicationProtocol.cpp — the two TUs intentionally keep
// independent copies so neither has to depend on the other's
// header surface.
inline void writeU8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}
inline void writeU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>( v        & 0xFF));
    out.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
}
inline void writeU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>( v        & 0xFF));
    out.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
inline void writeU64LE(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}
inline void writeFloatLE(std::vector<uint8_t>& out, float v) {
    static_assert(sizeof(float) == sizeof(uint32_t),
                  "IEEE-754 single-precision float expected");
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32LE(out, bits);
}

inline uint16_t readU16LE(const uint8_t* p) {
    return  static_cast<uint16_t>(p[0])
         | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t readU32LE(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) <<  8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t readU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}
inline float readFloatLE(const uint8_t* p) {
    const uint32_t bits = readU32LE(p);
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Must match net::kFrameEditTransaction in ReplicationProtocol.h.
// Duplicated as a private constant here to keep this TU independent
// of the net/ header — the value is validated by the boundary
// test_EditTransaction.cpp cases.
constexpr uint8_t kEditTransactionMsgType = 3;

} // namespace

// ── EditKind diagnostics ────────────────────────────────────────────

const char* editKindToString(EditKind k) {
    switch (k) {
        case EditKind::SetField: return "SetField";
        case EditKind::Undo:     return "Undo";
        case EditKind::Redo:     return "Redo";
        case EditKind::Spawn:    return "Spawn";
        case EditKind::Despawn:  return "Despawn";
    }
    return "Unknown";
}

// ── NetTransform wire helpers ───────────────────────────────────────

void writeNetTransformLE(const NetTransform& t, std::vector<uint8_t>& out) {
    writeFloatLE(out, t.posX);
    writeFloatLE(out, t.posY);
    writeFloatLE(out, t.posZ);
    writeFloatLE(out, t.rotX);
    writeFloatLE(out, t.rotY);
    writeFloatLE(out, t.rotZ);
    writeFloatLE(out, t.rotW);
}

std::optional<NetTransform>
readNetTransformLE(const uint8_t* data, size_t size) {
    if (!data || size < kNetTransformWireSize) return std::nullopt;
    NetTransform t;
    t.posX = readFloatLE(data +  0);
    t.posY = readFloatLE(data +  4);
    t.posZ = readFloatLE(data +  8);
    t.rotX = readFloatLE(data + 12);
    t.rotY = readFloatLE(data + 16);
    t.rotZ = readFloatLE(data + 20);
    t.rotW = readFloatLE(data + 24);
    return t;
}

void writeSpawnPayload(const NetTransform&    initialState,
                       uint32_t               ownerClientId,
                       std::vector<uint8_t>&  out) {
    writeNetTransformLE(initialState, out);
    writeU32LE(out, ownerClientId);
}

bool readSpawnPayload(const uint8_t* data,
                      size_t         size,
                      NetTransform&  outState,
                      uint32_t&      outOwnerClientId) {
    if (!data || size < kSpawnPayloadSize) return false;
    auto t = readNetTransformLE(data, size);
    if (!t) return false;
    outState         = *t;
    outOwnerClientId = readU32LE(data + kNetTransformWireSize);
    return true;
}

// ── Generic payload helpers ─────────────────────────────────────────
//
// Dispatch through the ReplicationRegistry's encodeSnapshot/
// decodeSnapshot walkers so any SV_REPLICATE'd type can flow on the
// SetField/Spawn wire without hand-written raw codecs. The SetField
// payload is byte-for-byte what encodeSnapshot emits. The Spawn
// payload prefixes it with a 4-byte ownerClientId so the server can
// pluck the owner without dispatching through the field list.

bool writeGenericSetFieldPayload(const ReplicationMeta& meta,
                                 const void*            instance,
                                 const DirtyMask&       mask,
                                 std::vector<uint8_t>&  out) {
    if (!instance) return false;
    SnapshotWriter writer;
    if (!encodeSnapshot(meta, instance, mask, writer)) return false;
    const std::vector<uint8_t>& buf = writer.buffer();
    out.insert(out.end(), buf.begin(), buf.end());
    return true;
}

bool readGenericSetFieldPayload(uint32_t         typeNameHash,
                                const uint8_t*   data,
                                size_t           size,
                                void*            outInstance,
                                DirtyMask&       outMask) {
    if (!outInstance || !data || size == 0) return false;
    const ReplicationMeta* meta =
        ReplicationRegistry::get().findByHash(typeNameHash);
    if (!meta) return false;
    SnapshotReader reader(data, size);
    return decodeSnapshot(*meta, outInstance, outMask, reader);
}

bool writeGenericSpawnPayload(const ReplicationMeta& meta,
                              const void*            instance,
                              uint32_t               ownerClientId,
                              std::vector<uint8_t>&  out) {
    if (!instance) return false;
    // Spawn payload needs a full-mask snapshot so the receiving side
    // sees every field populated. Build a DirtyMask sized to the
    // meta's field count and set every bit.
    DirtyMask fullMask(meta.fields.size());
    fullMask.setAll();
    writeU32LE(out, ownerClientId);
    return writeGenericSetFieldPayload(meta, instance, fullMask, out);
}

bool readGenericSpawnPayload(uint32_t         typeNameHash,
                             const uint8_t*   data,
                             size_t           size,
                             uint32_t&        outOwnerClientId,
                             void*            outInstance,
                             DirtyMask&       outMask) {
    if (!data || size < sizeof(uint32_t)) return false;
    outOwnerClientId = readU32LE(data);
    return readGenericSetFieldPayload(typeNameHash,
                                      data + sizeof(uint32_t),
                                      size - sizeof(uint32_t),
                                      outInstance,
                                      outMask);
}

// ── EditTransaction wire encode / decode ────────────────────────────

bool encodeEditTransaction(const EditTransaction& tx,
                           std::vector<uint8_t>&  out) {
    out.clear();
    if (tx.payload.size() > 0xFFFFu) {
        // Would overflow the u16 length field. This never happens
        // (the fixed NetTransform payloads are tiny) but the
        // guard is load-bearing once the wire layer generalizes.
        return false;
    }

    out.reserve(kEditTransactionHeaderSize + tx.payload.size());
    writeU8   (out, kEditTransactionMsgType);
    writeU8   (out, static_cast<uint8_t>(tx.kind));
    writeU64LE(out, tx.txId);
    writeU32LE(out, tx.originClientId);
    writeU8   (out, static_cast<uint8_t>(tx.requiredScope));
    writeU32LE(out, tx.entityId);
    writeU32LE(out, tx.typeNameHash);
    writeU64LE(out, tx.timestampMs);
    writeU16LE(out, static_cast<uint16_t>(tx.payload.size()));
    out.insert(out.end(), tx.payload.begin(), tx.payload.end());
    return true;
}

std::optional<EditTransaction> parseEditTransaction(const uint8_t* data,
                                                    size_t         size) {
    if (!data || size < kEditTransactionHeaderSize) return std::nullopt;
    if (data[0] != kEditTransactionMsgType)         return std::nullopt;

    EditTransaction tx;
    tx.kind           = static_cast<EditKind>(data[1]);
    tx.txId           = readU64LE(data + 2);
    tx.originClientId = readU32LE(data + 10);
    tx.requiredScope  = permissionScopeFromByte(data[14]);
    tx.entityId       = readU32LE(data + 15);
    tx.typeNameHash   = readU32LE(data + 19);
    tx.timestampMs    = readU64LE(data + 23);
    const uint16_t payloadLen = readU16LE(data + 31);

    if (static_cast<size_t>(kEditTransactionHeaderSize) + payloadLen > size) {
        return std::nullopt;
    }
    tx.payload.assign(data + kEditTransactionHeaderSize,
                      data + kEditTransactionHeaderSize + payloadLen);
    return tx;
}

} // namespace sv
