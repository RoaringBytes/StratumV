// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── ReplicationProtocol implementation ──────────────────────────────
// See ReplicationProtocol.h for the wire format + design choices.
// This TU is pure logic — no MsQuic, no Vulkan. Tests can link it
// without pulling in the transport, which is what test_ReplicationWire.cpp
// relies on for the round-trip cases.

#include "ReplicationProtocol.h"

#include "../ReplicationRegistry.h"

#include <cstring>

namespace sv::net {

namespace {

// Little-endian write helpers. Keep them scoped to this TU so the
// SnapshotWriter / SnapshotReader primitives in ReplicationRegistry
// remain the canonical "byte buffer" API — these are header-only
// serialisers for the fixed-width fields in kFrameHeaderSize.
inline void writeU8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}
inline void writeU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void writeU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0])
         | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

bool encodeSnapshotFrame(uint32_t               tickIndex,
                         uint32_t               entityId,
                         const ReplicationMeta& meta,
                         const void*            instance,
                         const DirtyMask&       mask,
                         std::vector<uint8_t>&  out) {
    out.clear();
    if (!instance) return false;

    // Step 1: encode the snapshot payload into a scratch writer. This
    // is the varint/zigzag/float/quant
    // dispatch against the ReplicationMeta's field vector.
    SnapshotWriter payloadWriter;
    if (!encodeSnapshot(meta, instance, mask, payloadWriter)) {
        return false;
    }

    const std::vector<uint8_t>& payload = payloadWriter.buffer();
    if (payload.size() > 0xFFFFu) {
        // Overflow the u16 payload length — caller must split.
        // This never trips for a tiny component like NetTransform, but
        // the guard is load-bearing once component sets grow.
        return false;
    }

    // Step 2: build the 15-byte header + append the payload.
    out.reserve(kFrameHeaderSize + payload.size());
    writeU8  (out, kFrameSnapshot);
    writeU32LE(out, tickIndex);
    writeU32LE(out, entityId);
    writeU32LE(out, meta.typeNameHash);
    writeU16LE(out, static_cast<uint16_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

std::optional<SnapshotFrame> parseSnapshotFrame(const uint8_t* data, size_t size) {
    if (!data || size < kFrameHeaderSize) return std::nullopt;
    if (data[0] != kFrameSnapshot)        return std::nullopt;

    SnapshotFrame f;
    f.tickIndex    = readU32LE(data + 1);
    f.entityId     = readU32LE(data + 5);
    f.typeNameHash = readU32LE(data + 9);
    f.payloadLen   = readU16LE(data + 13);

    // Bounds check: declared payload length must fit within the
    // supplied datagram.
    if (static_cast<size_t>(kFrameHeaderSize) + f.payloadLen > size) {
        return std::nullopt;
    }

    f.payload = data + kFrameHeaderSize;
    return f;
}

bool applySnapshotFrame(const SnapshotFrame& frame,
                        void*                outInstance,
                        DirtyMask&           outMask) {
    if (!outInstance || !frame.payload) return false;

    // Look up the component type via the registered hash. Unknown
    // types are a soft error — the client logs and drops the frame
    // without crashing on a stale or malicious header.
    const ReplicationMeta* meta =
        ReplicationRegistry::get().findByHash(frame.typeNameHash);
    if (!meta) return false;

    SnapshotReader reader(frame.payload, frame.payloadLen);
    return decodeSnapshot(*meta, outInstance, outMask, reader);
}

bool roundTripSnapshot(uint32_t               tickIndex,
                       uint32_t               entityId,
                       const ReplicationMeta& meta,
                       const void*            srcInstance,
                       const DirtyMask&       srcMask,
                       void*                  dstInstance,
                       DirtyMask&             dstMask) {
    std::vector<uint8_t> bytes;
    if (!encodeSnapshotFrame(tickIndex, entityId, meta,
                             srcInstance, srcMask, bytes)) {
        return false;
    }
    auto frame = parseSnapshotFrame(bytes.data(), bytes.size());
    if (!frame) return false;
    if (frame->tickIndex != tickIndex) return false;
    if (frame->entityId  != entityId)  return false;
    return applySnapshotFrame(*frame, dstInstance, dstMask);
}

// ── Schema handshake ──────────────────────────────────────────────────

const char* SchemaCompareResult::statusString() const {
    switch (status) {
        case SchemaCompareStatus::Ok:                return "Ok";
        case SchemaCompareStatus::Mismatch:          return "Mismatch";
        case SchemaCompareStatus::ServerHasUnknown:  return "ServerHasUnknown";
    }
    return "Unknown";
}

bool encodeSchemaHandshake(const SchemaHandshake& hs,
                           std::vector<uint8_t>&  out) {
    out.clear();
    // The typeCount field is a u16 — reject a handshake that cannot
    // be represented. With a single replicated type this is purely
    // defensive, but it is load-bearing once the registry grows.
    if (hs.types.size() > 0xFFFFu) return false;

    const size_t total =
        kSchemaHandshakeHeaderSize
        + hs.types.size() * kSchemaHandshakeEntrySize;
    out.reserve(total);

    writeU8  (out, kFrameSchemaHandshake);
    writeU32LE(out, hs.semver);
    writeU16LE(out, static_cast<uint16_t>(hs.types.size()));
    for (const SchemaHandshakeEntry& e : hs.types) {
        writeU32LE(out, e.typeNameHash);
        writeU16LE(out, e.schemaVersion);
    }
    return true;
}

bool parseSchemaHandshake(const uint8_t*    data,
                          size_t            size,
                          SchemaHandshake&  out) {
    out = SchemaHandshake{};
    if (!data || size < kSchemaHandshakeHeaderSize) return false;
    if (data[0] != kFrameSchemaHandshake)           return false;

    const uint32_t semver    = readU32LE(data + 1);
    const uint16_t typeCount = readU16LE(data + 5);

    // Bounds check: declared entry count must fit inside the buffer.
    const size_t needed =
        kSchemaHandshakeHeaderSize
        + static_cast<size_t>(typeCount) * kSchemaHandshakeEntrySize;
    if (size < needed) return false;

    out.semver = semver;
    out.types.reserve(typeCount);
    const uint8_t* cursor = data + kSchemaHandshakeHeaderSize;
    for (uint16_t i = 0; i < typeCount; ++i) {
        SchemaHandshakeEntry e{};
        e.typeNameHash  = readU32LE(cursor + 0);
        e.schemaVersion = readU16LE(cursor + 4);
        out.types.push_back(e);
        cursor += kSchemaHandshakeEntrySize;
    }
    return true;
}

// ── Welcome message encode / parse ──────────────────────────────────

bool encodeWelcomeMessage(const WelcomeMessage& w,
                          std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(kWelcomeMessageSize);
    writeU8  (out, kFrameWelcome);
    writeU32LE(out, w.clientId);
    writeU8  (out, w.scope);
    writeU32LE(out, w.avatarEntityId);
    return true;
}

bool parseWelcomeMessage(const uint8_t* data, size_t size, WelcomeMessage& out) {
    out = WelcomeMessage{};
    if (!data || size < kWelcomeMessageSize) return false;
    if (data[0] != kFrameWelcome)            return false;
    out.clientId       = readU32LE(data + 1);
    out.scope          = data[5];
    out.avatarEntityId = readU32LE(data + 6);
    return true;
}

// ── Asset sync encode / parse ───────────────────────────────────────

bool encodeAssetAnnounce(const AssetAnnounceMessage& a,
                         std::vector<uint8_t>&       out) {
    out.clear();
    if (a.name.size() > 0xFFFFu) return false;
    const size_t total = kAssetAnnounceHeader + a.name.size();
    out.reserve(total);
    writeU8  (out, kFrameAssetAnnounce);
    out.insert(out.end(), a.hash.begin(), a.hash.end());
    writeU32LE(out, a.byteSize);
    writeU8  (out, a.assetKind);
    writeU16LE(out, static_cast<uint16_t>(a.name.size()));
    if (!a.name.empty()) {
        out.insert(out.end(), a.name.begin(), a.name.end());
    }
    return true;
}

bool parseAssetAnnounce(const uint8_t*        data,
                        size_t                size,
                        AssetAnnounceMessage& out) {
    out = AssetAnnounceMessage{};
    if (!data || size < kAssetAnnounceHeader) return false;
    if (data[0] != kFrameAssetAnnounce) return false;
    for (size_t i = 0; i < 32; ++i) out.hash[i] = data[1 + i];
    out.byteSize  = readU32LE(data + 33);
    out.assetKind = data[37];
    const uint16_t nameLen = readU16LE(data + 38);
    if (kAssetAnnounceHeader + nameLen > size) {
        out = AssetAnnounceMessage{};
        return false;
    }
    if (nameLen > 0) {
        out.name.assign(reinterpret_cast<const char*>(data + kAssetAnnounceHeader), nameLen);
    }
    return true;
}

bool encodeAssetAck(const AssetAckMessage& ack,
                    std::vector<uint8_t>&  out) {
    out.clear();
    out.reserve(kAssetAckSize);
    writeU8(out, kFrameAssetAck);
    out.insert(out.end(), ack.hash.begin(), ack.hash.end());
    writeU8(out, static_cast<uint8_t>(ack.status));
    return true;
}

bool parseAssetAck(const uint8_t*   data,
                   size_t           size,
                   AssetAckMessage& out) {
    out = AssetAckMessage{};
    if (!data || size < kAssetAckSize) return false;
    if (data[0] != kFrameAssetAck) return false;
    for (size_t i = 0; i < 32; ++i) out.hash[i] = data[1 + i];
    const uint8_t raw = data[33];
    // Accept only known status bytes; unknown values downgrade to
    // NeedChunks so an out-of-date client can still complete the
    // upload rather than wedging.
    out.status = (raw == static_cast<uint8_t>(AssetAckStatus::HaveIt))
                   ? AssetAckStatus::HaveIt
                   : AssetAckStatus::NeedChunks;
    return true;
}

bool encodeAssetChunk(const AssetChunkMessage& c,
                      std::vector<uint8_t>&    out) {
    out.clear();
    if (c.chunkLen > 0 && !c.chunk) return false;
    if (c.chunkIndex >= c.chunkCount && c.chunkCount != 0) return false;
    // u32 chunkLen already caps at 4 GiB; no extra guard needed.
    const size_t total = kAssetChunkHeaderSize + c.chunkLen;
    out.reserve(total);
    writeU8(out, kFrameAssetChunk);
    out.insert(out.end(), c.hash.begin(), c.hash.end());
    writeU32LE(out, c.chunkIndex);
    writeU32LE(out, c.chunkCount);
    writeU32LE(out, c.chunkLen);
    if (c.chunkLen > 0) {
        out.insert(out.end(), c.chunk, c.chunk + c.chunkLen);
    }
    return true;
}

bool parseAssetChunk(const uint8_t*    data,
                     size_t            size,
                     AssetChunkMessage& out) {
    out = AssetChunkMessage{};
    if (!data || size < kAssetChunkHeaderSize) return false;
    if (data[0] != kFrameAssetChunk) return false;
    for (size_t i = 0; i < 32; ++i) out.hash[i] = data[1 + i];
    out.chunkIndex = readU32LE(data + 33);
    out.chunkCount = readU32LE(data + 37);
    out.chunkLen   = readU32LE(data + 41);
    // Bounds: chunkIndex within chunkCount (unless chunkCount == 0,
    // in which case chunkIndex must also be 0 — we treat an empty
    // asset as "one zero-byte chunk" rather than "zero chunks" at
    // the AssetUploadClient layer, but we don't enforce it here).
    if (out.chunkCount > 0 && out.chunkIndex >= out.chunkCount) {
        out = AssetChunkMessage{};
        return false;
    }
    if (kAssetChunkHeaderSize + static_cast<size_t>(out.chunkLen) > size) {
        out = AssetChunkMessage{};
        return false;
    }
    out.chunk = (out.chunkLen > 0) ? (data + kAssetChunkHeaderSize) : nullptr;
    return true;
}

SchemaCompareResult compareSchemaHandshake(const SchemaHandshake& received) {
    SchemaCompareResult result;
    const ReplicationRegistry& reg = ReplicationRegistry::get();

    // First pass: look for a hard drift. If nothing drifts, a later
    // pass records the first server-has-unknown as a soft result.
    const SchemaHandshakeEntry* firstUnknown = nullptr;
    for (const SchemaHandshakeEntry& e : received.types) {
        const ReplicationMeta* local = reg.findByHash(e.typeNameHash);
        if (!local) {
            if (!firstUnknown) firstUnknown = &e;
            continue;
        }
        if (local->schemaVersion != e.schemaVersion) {
            result.status       = SchemaCompareStatus::Mismatch;
            result.mismatchHash = e.typeNameHash;
            result.expectedVer  = local->schemaVersion;
            result.receivedVer  = e.schemaVersion;
            result.mismatchName = local->typeName;
            return result;
        }
    }

    if (firstUnknown) {
        result.status       = SchemaCompareStatus::ServerHasUnknown;
        result.mismatchHash = firstUnknown->typeNameHash;
        result.expectedVer  = 0;
        result.receivedVer  = firstUnknown->schemaVersion;
        // mismatchName is intentionally empty — the client registry
        // has no name for this hash.
        return result;
    }

    result.status = SchemaCompareStatus::Ok;
    return result;
}

} // namespace sv::net
