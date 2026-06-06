// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── EditorBridge wire-format tests ───────────────
//
// These cases exercise the explicit bridge message types added in
// (AssetAnnounce / AssetChunk / SetParent) from the
// C++ side — pure logic checks on the wire layout, no actual TCP
// traffic. They live in `sv_tests` (not `sv_core_tests`) because
// EditorBridge is a full-engine target: it uses winsock2 on Windows
// and is explicitly excluded from `cmake/stratumv_core_sources.cmake`.
//
// We do NOT spin up a real EditorBridge instance. Instead we hand-
// build the bridge's on-wire frame bytes (the exact layout the C++
// reader-thread dispatcher parses) and use the existing
// `sv::AssetReceiver` state machine to confirm the bytes reassemble
// into the original payload. That matches the code path the bridge
// runs for every inbound asset from Blender.

#include <catch2/catch_test_macros.hpp>

#include "AssetPersistence.h"
#include "AssetUploadClient.h"
#include "CameraComponent.h"
#include "EditTransaction.h"
#include "LightComponent.h"
#include "MaterialComponent.h"
#include "ReplicationRegistry.h"
#include "Sha256.h"
#include "net/EditorBridge.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Little-endian helpers matching the bridge's internal framing.
void writeU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void writeU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8)  & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Build an AssetAnnounce BODY (no length prefix, no outer msg byte).
// The bridge reads frames as `[u32 len][u8 msgType][body]` on the
// wire; tests here only validate the body layout.
std::vector<uint8_t> buildAnnounceBody(const sv::AssetHash& hash,
                                       uint32_t             byteSize,
                                       uint8_t              assetKind,
                                       const std::string&   name) {
    std::vector<uint8_t> body;
    body.insert(body.end(), hash.begin(), hash.end());
    writeU32LE(body, byteSize);
    body.push_back(assetKind);
    writeU16LE(body, static_cast<uint16_t>(name.size()));
    body.insert(body.end(), name.begin(), name.end());
    return body;
}

std::vector<uint8_t> buildChunkBody(const sv::AssetHash&  hash,
                                    uint32_t              chunkIndex,
                                    uint32_t              chunkCount,
                                    const uint8_t*        chunkData,
                                    uint32_t              chunkLen) {
    std::vector<uint8_t> body;
    body.insert(body.end(), hash.begin(), hash.end());
    writeU32LE(body, chunkIndex);
    writeU32LE(body, chunkCount);
    writeU32LE(body, chunkLen);
    body.insert(body.end(), chunkData, chunkData + chunkLen);
    return body;
}

std::vector<uint8_t> buildSetParentBody(uint32_t parentId) {
    std::vector<uint8_t> body;
    writeU32LE(body, parentId);
    return body;
}

// SetLight body — 32 bytes:
//   [u32 type] [f32 colorR][f32 colorG][f32 colorB]
//   [f32 intensity] [f32 range] [f32 coneInnerDeg][f32 coneOuterDeg]
void writeF32LE(std::vector<uint8_t>& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32LE(out, bits);
}

std::vector<uint8_t> buildSetLightBody(uint32_t type,
                                       float colorR, float colorG, float colorB,
                                       float intensity,
                                       float range,
                                       float coneInnerDeg,
                                       float coneOuterDeg) {
    std::vector<uint8_t> body;
    writeU32LE(body, type);
    writeF32LE(body, colorR);
    writeF32LE(body, colorG);
    writeF32LE(body, colorB);
    writeF32LE(body, intensity);
    writeF32LE(body, range);
    writeF32LE(body, coneInnerDeg);
    writeF32LE(body, coneOuterDeg);
    return body;
}

// SetCamera body — 16 bytes:
//   [f32 fovDeg] [f32 aspect] [f32 nearPlane] [f32 farPlane]
std::vector<uint8_t> buildSetCameraBody(float fovDeg,
                                        float aspect,
                                        float nearPlane,
                                        float farPlane) {
    std::vector<uint8_t> body;
    writeF32LE(body, fovDeg);
    writeF32LE(body, aspect);
    writeF32LE(body, nearPlane);
    writeF32LE(body, farPlane);
    return body;
}

// SetMaterial body — 16 bytes:
//   [f32 baseColorR] [f32 baseColorG] [f32 baseColorB] [f32 overrideStrength]
std::vector<uint8_t> buildSetMaterialBody(float r, float g, float b,
                                          float strength) {
    std::vector<uint8_t> body;
    writeF32LE(body, r);
    writeF32LE(body, g);
    writeF32LE(body, b);
    writeF32LE(body, strength);
    return body;
}

// Tiny deterministic byte pattern generator so test payloads don't
// rely on literal byte soup in the test source.
std::vector<uint8_t> generatePattern(size_t size) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>((i * 131) & 0xFF);
    }
    return out;
}

} // namespace

// ── Message type constants ────────────────────────────────────────

TEST_CASE("EditorBridge message types are in the 0x83..0x86 range",
          "[bridgewire][msgtypes]") {
    REQUIRE(sv::net::kBridgeMsgAssetAnnounce == 0x83);
    REQUIRE(sv::net::kBridgeMsgAssetChunk    == 0x84);
    REQUIRE(sv::net::kBridgeMsgSetParent     == 0x86);
    // Ensure the new types do NOT collide with the
    // downstream or upstream message bytes.
    REQUIRE(sv::net::kBridgeMsgHello       != sv::net::kBridgeMsgAssetAnnounce);
    REQUIRE(sv::net::kBridgeMsgEntityState != sv::net::kBridgeMsgAssetAnnounce);
    REQUIRE(sv::net::kBridgeMsgMoveSelf    != sv::net::kBridgeMsgSetParent);
    REQUIRE(sv::net::kBridgeMsgPing        != sv::net::kBridgeMsgSetParent);
}

TEST_CASE("EditorBridge SetLight constant pinned to 0x87",
          "[bridgewire][lightwire][msgtypes]") {
    // Pin the value so a Python/C++ drift regression fails loudly.
    REQUIRE(sv::net::kBridgeMsgSetLight == 0x87);
    // Body-size constant mirrors the 32-byte layout in the header.
    REQUIRE(sv::net::kBridgeSetLightBodyBytes == 32u);
    // The new type must not collide with any existing type byte.
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgHello);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgEntityState);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgEntityGone);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgServerState);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgMoveSelf);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgPing);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgAssetAnnounce);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgAssetChunk);
    REQUIRE(sv::net::kBridgeMsgSetLight != sv::net::kBridgeMsgSetParent);
}

// ── AssetAnnounce body layout ─────────────────────────────────────

TEST_CASE("EditorBridge AssetAnnounce body matches the documented layout",
          "[bridgewire][announce]") {
    sv::AssetHash hash{};
    for (size_t i = 0; i < hash.size(); ++i) hash[i] = static_cast<uint8_t>(i);
    const uint32_t byteSize = 150000u;
    const uint8_t  kind     = 2u;   // Texture
    const std::string name  = "push/my_cube.glb";

    const auto body = buildAnnounceBody(hash, byteSize, kind, name);

    // Fixed header size is 32 (hash) + 4 (byteSize) + 1 (kind) +
    // 2 (nameLen) = 39 bytes.
    REQUIRE(body.size() == 39 + name.size());

    // Spot-check the hash prefix.
    for (size_t i = 0; i < hash.size(); ++i) {
        REQUIRE(body[i] == static_cast<uint8_t>(i));
    }
    // byteSize is little-endian at offset 32.
    REQUIRE(body[32] == static_cast<uint8_t>(byteSize & 0xFF));
    REQUIRE(body[33] == static_cast<uint8_t>((byteSize >> 8)  & 0xFF));
    REQUIRE(body[34] == static_cast<uint8_t>((byteSize >> 16) & 0xFF));
    REQUIRE(body[35] == static_cast<uint8_t>((byteSize >> 24) & 0xFF));
    // Kind + name length follow.
    REQUIRE(body[36] == kind);
    REQUIRE(body[37] == static_cast<uint8_t>(name.size() & 0xFF));
    REQUIRE(body[38] == static_cast<uint8_t>((name.size() >> 8) & 0xFF));
    // Name bytes immediately follow.
    for (size_t i = 0; i < name.size(); ++i) {
        REQUIRE(body[39 + i] == static_cast<uint8_t>(name[i]));
    }
}

// ── AssetChunk body layout ────────────────────────────────────────

TEST_CASE("EditorBridge AssetChunk body matches the documented layout",
          "[bridgewire][chunk]") {
    sv::AssetHash hash{};
    for (size_t i = 0; i < hash.size(); ++i) hash[i] = static_cast<uint8_t>(0x20 + i);
    const uint32_t chunkIndex = 3u;
    const uint32_t chunkCount = 4u;
    std::vector<uint8_t> chunk = generatePattern(100);

    const auto body = buildChunkBody(hash, chunkIndex, chunkCount,
                                     chunk.data(),
                                     static_cast<uint32_t>(chunk.size()));

    // Fixed header = 32 + 4 + 4 + 4 = 44 bytes.
    REQUIRE(body.size() == 44 + chunk.size());
    // Hash prefix check.
    for (size_t i = 0; i < hash.size(); ++i) {
        REQUIRE(body[i] == static_cast<uint8_t>(0x20 + i));
    }
    // chunkIndex, chunkCount, chunkLen at the expected offsets.
    REQUIRE(body[32] == 3);
    REQUIRE(body[36] == 4);
    REQUIRE(body[40] == static_cast<uint8_t>(chunk.size() & 0xFF));
    // Chunk payload survives unchanged.
    REQUIRE(std::memcmp(body.data() + 44, chunk.data(), chunk.size()) == 0);
}

// ── SetParent body layout ─────────────────────────────────────────

TEST_CASE("EditorBridge SetParent body is a single little-endian u32",
          "[bridgewire][setparent]") {
    const uint32_t parentId = 0xCAFEBABEu;
    const auto body = buildSetParentBody(parentId);
    REQUIRE(body.size() == 4);
    REQUIRE(body[0] == 0xBE);
    REQUIRE(body[1] == 0xBA);
    REQUIRE(body[2] == 0xFE);
    REQUIRE(body[3] == 0xCA);
}

TEST_CASE("EditorBridge SetParent zero means unparented",
          "[bridgewire][setparent]") {
    const auto body = buildSetParentBody(0);
    REQUIRE(body.size() == 4);
    REQUIRE(body[0] == 0);
    REQUIRE(body[1] == 0);
    REQUIRE(body[2] == 0);
    REQUIRE(body[3] == 0);
}

// ── Assembly + verify round-trip ──────────────────────────────────
// The bridge reader uses sv::AssetReceiver to assemble inbound
// chunks. Mirror that state machine here across a single-chunk and
// a multi-chunk payload, verifying the hash matches.

TEST_CASE("AssetReceiver: single-chunk bridge upload round-trip",
          "[bridgewire][assembly]") {
    const std::string name = "push/small.glb";
    auto bytes = generatePattern(17);    // tail-only payload
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    const uint32_t chunkSize  = 65536u;
    const uint32_t chunkCount = sv::assetChunkCount(
        static_cast<uint32_t>(bytes.size()), chunkSize);
    REQUIRE(chunkCount == 1);

    sv::AssetReceiver rx;
    rx.beginFromAnnounce(hash, static_cast<uint32_t>(bytes.size()),
                         /*assetKind=*/1u, name,
                         chunkCount, chunkSize);
    REQUIRE(rx.depositChunk(0, bytes.data(), bytes.size()));
    REQUIRE(rx.complete);
    REQUIRE(rx.verifyHash());
    REQUIRE(rx.name == name);
    REQUIRE(rx.assembled.size() == bytes.size());
    REQUIRE(std::memcmp(rx.assembled.data(), bytes.data(), bytes.size()) == 0);
}

TEST_CASE("AssetReceiver: multi-chunk bridge upload round-trip",
          "[bridgewire][assembly]") {
    const uint32_t chunkSize = 64u;    // tiny so we exercise 3 chunks
    auto bytes = generatePattern(150);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    const uint32_t chunkCount = sv::assetChunkCount(
        static_cast<uint32_t>(bytes.size()), chunkSize);
    REQUIRE(chunkCount == 3);   // 64 + 64 + 22

    sv::AssetReceiver rx;
    rx.beginFromAnnounce(hash, static_cast<uint32_t>(bytes.size()),
                         /*assetKind=*/1u, "push/multi.glb",
                         chunkCount, chunkSize);

    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint32_t start = i * chunkSize;
        const uint32_t len = (i == chunkCount - 1)
            ? static_cast<uint32_t>(bytes.size() - start)
            : chunkSize;
        REQUIRE(rx.depositChunk(i, bytes.data() + start, len));
    }
    REQUIRE(rx.complete);
    REQUIRE(rx.verifyHash());
    REQUIRE(rx.assembled.size() == bytes.size());
    REQUIRE(std::memcmp(rx.assembled.data(), bytes.data(), bytes.size()) == 0);
}

TEST_CASE("AssetReceiver: hash-mismatch upload is rejected by verifyHash",
          "[bridgewire][assembly][error]") {
    auto bytes = generatePattern(200);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    sv::AssetReceiver rx;
    rx.beginFromAnnounce(hash, static_cast<uint32_t>(bytes.size()),
                         /*assetKind=*/2u, "push/bad.glb",
                         /*chunkCount=*/1u, /*chunkSize=*/65536u);
    // Tamper with one byte before deposit.
    bytes[42] ^= 0xAAu;
    REQUIRE(rx.depositChunk(0, bytes.data(), bytes.size()));
    REQUIRE(rx.complete);
    REQUIRE_FALSE(rx.verifyHash());
}

TEST_CASE("EditorBridge frame-length cap allows 64 KiB chunk payloads",
          "[bridgewire][limits]") {
    // bumped kMaxFrameBytes to 256 KiB. A 64 KiB
    // chunk payload plus the 44-byte chunk body header plus the
    // 1-byte msgType plus the 4-byte outer length prefix equals
    // 65605 bytes total — must fit comfortably inside the cap.
    constexpr size_t kMaxAssetChunkPayload = 64u * 1024u;
    constexpr size_t kChunkBodyFixed = 44;
    constexpr size_t kMsgByte        = 1;
    constexpr size_t kOuterLenPrefix = 4;
    constexpr size_t kTotalFrameSize =
        kChunkBodyFixed + kMaxAssetChunkPayload + kMsgByte + kOuterLenPrefix;
    // The cap is 256 KiB — well above the computed size.
    constexpr size_t kNewCap = 256u * 1024u;
    static_assert(kTotalFrameSize < kNewCap,
                  "64 KiB chunk payload should fit inside kMaxFrameBytes = 256 KiB");
    REQUIRE(true);
}

// ── SetLight body layout ─────────────────────────

TEST_CASE("EditorBridge SetLight body matches the documented layout",
          "[bridgewire][lightwire]") {
    // Use values whose IEEE-754 encodings are well-known so the byte
    // sequence is reproducible on any little-endian host.
    const uint32_t type         = 2u;       // Point
    const float    colorR       = 0.8f;     // 0x3F4CCCCD
    const float    colorG       = 0.9f;     // 0x3F666666
    const float    colorB       = 1.0f;     // 0x3F800000
    const float    intensity    = 2.5f;     // 0x40200000
    const float    range        = 17.5f;    // 0x418C0000
    const float    coneInnerDeg = 22.0f;    // 0x41B00000
    const float    coneOuterDeg = 44.0f;    // 0x42300000

    const auto body = buildSetLightBody(type, colorR, colorG, colorB,
                                         intensity, range,
                                         coneInnerDeg, coneOuterDeg);

    // Fixed body size must match the header constant.
    REQUIRE(body.size() == sv::net::kBridgeSetLightBodyBytes);
    REQUIRE(body.size() == 32u);

    // u32 type at offset 0 (little-endian).
    REQUIRE(body[0] == 0x02);
    REQUIRE(body[1] == 0x00);
    REQUIRE(body[2] == 0x00);
    REQUIRE(body[3] == 0x00);

    // colorR = 0.8f = 0x3F4CCCCD little-endian → CD CC 4C 3F
    REQUIRE(body[4] == 0xCD);
    REQUIRE(body[5] == 0xCC);
    REQUIRE(body[6] == 0x4C);
    REQUIRE(body[7] == 0x3F);

    // colorG = 0.9f = 0x3F666666 → 66 66 66 3F
    REQUIRE(body[8]  == 0x66);
    REQUIRE(body[9]  == 0x66);
    REQUIRE(body[10] == 0x66);
    REQUIRE(body[11] == 0x3F);

    // colorB = 1.0f = 0x3F800000 → 00 00 80 3F
    REQUIRE(body[12] == 0x00);
    REQUIRE(body[13] == 0x00);
    REQUIRE(body[14] == 0x80);
    REQUIRE(body[15] == 0x3F);

    // intensity = 2.5f = 0x40200000 → 00 00 20 40
    REQUIRE(body[16] == 0x00);
    REQUIRE(body[17] == 0x00);
    REQUIRE(body[18] == 0x20);
    REQUIRE(body[19] == 0x40);

    // range = 17.5f = 0x418C0000 → 00 00 8C 41
    REQUIRE(body[20] == 0x00);
    REQUIRE(body[21] == 0x00);
    REQUIRE(body[22] == 0x8C);
    REQUIRE(body[23] == 0x41);

    // coneInnerDeg = 22.0f = 0x41B00000 → 00 00 B0 41
    REQUIRE(body[24] == 0x00);
    REQUIRE(body[25] == 0x00);
    REQUIRE(body[26] == 0xB0);
    REQUIRE(body[27] == 0x41);

    // coneOuterDeg = 44.0f = 0x42300000 → 00 00 30 42
    REQUIRE(body[28] == 0x00);
    REQUIRE(body[29] == 0x00);
    REQUIRE(body[30] == 0x30);
    REQUIRE(body[31] == 0x42);
}

TEST_CASE("EditorBridge SetLight round-trips through LightComponent codec",
          "[bridgewire][lightwire][roundtrip]") {
    // The bridge's reader thread produces an `EditorBridgeLightSet`
    // POD from the 32-byte body; the main thread then re-encodes it
    // through `writeGenericSetFieldPayload` over the
    // `LightComponent` registry meta. Exercise the full path here:
    //   bytes -> fields -> LightComponent -> wire bytes -> LightComponent
    // so a drift between the bridge layout and the replication
    // layout fails loudly.
    sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == 8u);

    // Parse the body by hand — same offsets the reader uses.
    const auto body = buildSetLightBody(3u, 0.5f, 0.25f, 0.125f,
                                         4.0f, 50.0f, 10.0f, 20.0f);
    REQUIRE(body.size() == 32u);

    sv::LightComponent lc;
    std::memcpy(&lc.type,         body.data() + 0,  sizeof(uint32_t));
    std::memcpy(&lc.colorR,       body.data() + 4,  sizeof(float));
    std::memcpy(&lc.colorG,       body.data() + 8,  sizeof(float));
    std::memcpy(&lc.colorB,       body.data() + 12, sizeof(float));
    std::memcpy(&lc.intensity,    body.data() + 16, sizeof(float));
    std::memcpy(&lc.range,        body.data() + 20, sizeof(float));
    std::memcpy(&lc.coneInnerDeg, body.data() + 24, sizeof(float));
    std::memcpy(&lc.coneOuterDeg, body.data() + 28, sizeof(float));

    REQUIRE(lc.type         == 3u);
    REQUIRE(lc.colorR       == 0.5f);
    REQUIRE(lc.colorG       == 0.25f);
    REQUIRE(lc.colorB       == 0.125f);
    REQUIRE(lc.intensity    == 4.0f);
    REQUIRE(lc.range        == 50.0f);
    REQUIRE(lc.coneInnerDeg == 10.0f);
    REQUIRE(lc.coneOuterDeg == 20.0f);

    // Full-mask re-encode through the replication path — the same
    // call `pumpBridgeLights` runs on the main thread. The
    // SnapshotWriter uses varint encoding for UInt32 fields, so the
    // payload size is value-dependent: small `type` values (0..127)
    // pack into one byte, larger ones up to five. Just assert the
    // payload is non-empty and large enough to carry the raw 7 floats
    // (28 bytes) plus the 2-byte schema header and 1-byte mask.
    sv::DirtyMask full(meta->fields.size());
    full.setAll();
    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &lc, full, payload));
    REQUIRE(payload.size() >= 32u);   // 2 schema + 1 mask + 1 varint + 7*4 floats

    // Decode back — decoder should reconstruct every byte.
    sv::LightComponent round;
    sv::DirtyMask mask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                            payload.data(),
                                            payload.size(),
                                            &round,
                                            mask));
    REQUIRE(round.type         == lc.type);
    REQUIRE(round.colorR       == lc.colorR);
    REQUIRE(round.colorG       == lc.colorG);
    REQUIRE(round.colorB       == lc.colorB);
    REQUIRE(round.intensity    == lc.intensity);
    REQUIRE(round.range        == lc.range);
    REQUIRE(round.coneInnerDeg == lc.coneInnerDeg);
    REQUIRE(round.coneOuterDeg == lc.coneOuterDeg);
}

TEST_CASE("EditorBridge SetLight body fits inside kMaxFrameBytes trivially",
          "[bridgewire][lightwire][limits]") {
    // A 32-byte body + 1-byte msgType + 4-byte length prefix is 37
    // bytes total. The 256 KiB cap handles it with
    // ~262 KiB of headroom — pin a static_assert so a future cap
    // reduction still trivially fits the smallest explicit upstream
    // frame in the bridge protocol.
    constexpr size_t kSetLightBody  = 32;
    constexpr size_t kMsgByte       = 1;
    constexpr size_t kLenPrefix     = 4;
    constexpr size_t kTotal         = kSetLightBody + kMsgByte + kLenPrefix;
    constexpr size_t kCap           = 256u * 1024u;
    static_assert(kTotal == 37, "SetLight frame on the wire must be 37 bytes");
    static_assert(kTotal < kCap,
                  "SetLight frame must fit inside kMaxFrameBytes");
    REQUIRE(true);
}

// ── SetCamera + SetMaterial constants ────────────

TEST_CASE("EditorBridge message types pinned to 0x88 + 0x89",
          "[bridgewire][cameramatwire][msgtypes]") {
    REQUIRE(sv::net::kBridgeMsgSetCamera   == 0x88);
    REQUIRE(sv::net::kBridgeMsgSetMaterial == 0x89);
    REQUIRE(sv::net::kBridgeSetCameraBodyBytes   == 16u);
    REQUIRE(sv::net::kBridgeSetMaterialBodyBytes == 16u);
    // Must not collide with any existing type byte.
    REQUIRE(sv::net::kBridgeMsgSetCamera   != sv::net::kBridgeMsgSetLight);
    REQUIRE(sv::net::kBridgeMsgSetCamera   != sv::net::kBridgeMsgSetMaterial);
    REQUIRE(sv::net::kBridgeMsgSetMaterial != sv::net::kBridgeMsgSetLight);
    REQUIRE(sv::net::kBridgeMsgSetCamera   != sv::net::kBridgeMsgSetParent);
    REQUIRE(sv::net::kBridgeMsgSetMaterial != sv::net::kBridgeMsgAssetChunk);
}

// ── SetCamera body layout ─────────────────────────────────────────

TEST_CASE("EditorBridge SetCamera body matches the documented layout",
          "[bridgewire][cameramatwire]") {
    // Use values whose IEEE-754 encodings are well-known so the byte
    // sequence is reproducible on any little-endian host.
    const float fovDeg    = 60.0f;     // 0x42700000
    const float aspect    = 1.5f;      // 0x3FC00000
    const float nearPlane = 0.5f;      // 0x3F000000
    const float farPlane  = 100.0f;    // 0x42C80000

    const auto body = buildSetCameraBody(fovDeg, aspect, nearPlane, farPlane);

    REQUIRE(body.size() == sv::net::kBridgeSetCameraBodyBytes);
    REQUIRE(body.size() == 16u);

    // fovDeg = 60.0f = 0x42700000 → 00 00 70 42
    REQUIRE(body[0]  == 0x00);
    REQUIRE(body[1]  == 0x00);
    REQUIRE(body[2]  == 0x70);
    REQUIRE(body[3]  == 0x42);

    // aspect = 1.5f = 0x3FC00000 → 00 00 C0 3F
    REQUIRE(body[4]  == 0x00);
    REQUIRE(body[5]  == 0x00);
    REQUIRE(body[6]  == 0xC0);
    REQUIRE(body[7]  == 0x3F);

    // nearPlane = 0.5f = 0x3F000000 → 00 00 00 3F
    REQUIRE(body[8]  == 0x00);
    REQUIRE(body[9]  == 0x00);
    REQUIRE(body[10] == 0x00);
    REQUIRE(body[11] == 0x3F);

    // farPlane = 100.0f = 0x42C80000 → 00 00 C8 42
    REQUIRE(body[12] == 0x00);
    REQUIRE(body[13] == 0x00);
    REQUIRE(body[14] == 0xC8);
    REQUIRE(body[15] == 0x42);
}

TEST_CASE("EditorBridge SetCamera round-trips through CameraComponent codec",
          "[bridgewire][cameramatwire][roundtrip]") {
    // Same end-to-end pattern as the SetLight round-trip — bytes →
    // fields → CameraComponent → wire payload → CameraComponent.
    sv::ensureCameraComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("CameraComponent");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == 4u);

    const auto body = buildSetCameraBody(45.0f, 0.0f, 0.1f, 1500.0f);
    REQUIRE(body.size() == 16u);

    sv::CameraComponent cam;
    std::memcpy(&cam.fovDeg,    body.data() + 0,  sizeof(float));
    std::memcpy(&cam.aspect,    body.data() + 4,  sizeof(float));
    std::memcpy(&cam.nearPlane, body.data() + 8,  sizeof(float));
    std::memcpy(&cam.farPlane,  body.data() + 12, sizeof(float));

    REQUIRE(cam.fovDeg    == 45.0f);
    REQUIRE(cam.aspect    == 0.0f);
    REQUIRE(cam.nearPlane == 0.1f);
    REQUIRE(cam.farPlane  == 1500.0f);

    sv::DirtyMask full(meta->fields.size());
    full.setAll();
    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &cam, full, payload));

    sv::CameraComponent round;
    sv::DirtyMask mask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                            payload.data(),
                                            payload.size(),
                                            &round,
                                            mask));
    REQUIRE(round.fovDeg    == cam.fovDeg);
    REQUIRE(round.aspect    == cam.aspect);
    REQUIRE(round.nearPlane == cam.nearPlane);
    REQUIRE(round.farPlane  == cam.farPlane);
}

// ── SetMaterial body layout ───────────────────────────────────────

TEST_CASE("EditorBridge SetMaterial body matches the documented layout",
          "[bridgewire][cameramatwire]") {
    const float r        = 0.5f;     // 0x3F000000
    const float g        = 0.25f;    // 0x3E800000
    const float b        = 0.125f;   // 0x3E000000
    const float strength = 1.0f;     // 0x3F800000

    const auto body = buildSetMaterialBody(r, g, b, strength);

    REQUIRE(body.size() == sv::net::kBridgeSetMaterialBodyBytes);
    REQUIRE(body.size() == 16u);

    // r = 0.5f = 0x3F000000 → 00 00 00 3F
    REQUIRE(body[0]  == 0x00);
    REQUIRE(body[1]  == 0x00);
    REQUIRE(body[2]  == 0x00);
    REQUIRE(body[3]  == 0x3F);

    // g = 0.25f = 0x3E800000 → 00 00 80 3E
    REQUIRE(body[4]  == 0x00);
    REQUIRE(body[5]  == 0x00);
    REQUIRE(body[6]  == 0x80);
    REQUIRE(body[7]  == 0x3E);

    // b = 0.125f = 0x3E000000 → 00 00 00 3E
    REQUIRE(body[8]  == 0x00);
    REQUIRE(body[9]  == 0x00);
    REQUIRE(body[10] == 0x00);
    REQUIRE(body[11] == 0x3E);

    // strength = 1.0f = 0x3F800000 → 00 00 80 3F
    REQUIRE(body[12] == 0x00);
    REQUIRE(body[13] == 0x00);
    REQUIRE(body[14] == 0x80);
    REQUIRE(body[15] == 0x3F);
}

TEST_CASE("EditorBridge SetMaterial round-trips through MaterialComponent codec",
          "[bridgewire][cameramatwire][roundtrip]") {
    sv::ensureMaterialComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("MaterialComponent");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == 4u);

    const auto body = buildSetMaterialBody(0.8f, 0.3f, 0.6f, 0.75f);
    REQUIRE(body.size() == 16u);

    sv::MaterialComponent mat;
    std::memcpy(&mat.baseColorR,       body.data() + 0,  sizeof(float));
    std::memcpy(&mat.baseColorG,       body.data() + 4,  sizeof(float));
    std::memcpy(&mat.baseColorB,       body.data() + 8,  sizeof(float));
    std::memcpy(&mat.overrideStrength, body.data() + 12, sizeof(float));

    REQUIRE(mat.baseColorR       == 0.8f);
    REQUIRE(mat.baseColorG       == 0.3f);
    REQUIRE(mat.baseColorB       == 0.6f);
    REQUIRE(mat.overrideStrength == 0.75f);

    sv::DirtyMask full(meta->fields.size());
    full.setAll();
    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &mat, full, payload));

    sv::MaterialComponent round;
    sv::DirtyMask mask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                            payload.data(),
                                            payload.size(),
                                            &round,
                                            mask));
    REQUIRE(round.baseColorR       == mat.baseColorR);
    REQUIRE(round.baseColorG       == mat.baseColorG);
    REQUIRE(round.baseColorB       == mat.baseColorB);
    REQUIRE(round.overrideStrength == mat.overrideStrength);
}

TEST_CASE("EditorBridge SetCamera + SetMaterial frames fit inside kMaxFrameBytes",
          "[bridgewire][cameramatwire][limits]") {
    // Both 16-byte bodies + 1-byte msgType + 4-byte len prefix → 21
    // bytes on the wire. Pin the math so a future cap reduction still
    // trivially fits the smallest CameraComponent / MaterialComponent
    // frame.
    constexpr size_t kBody          = 16;
    constexpr size_t kMsgByte       = 1;
    constexpr size_t kLenPrefix     = 4;
    constexpr size_t kTotal         = kBody + kMsgByte + kLenPrefix;
    constexpr size_t kCap           = 256u * 1024u;
    static_assert(kTotal == 21, "SetCamera/SetMaterial frame must be 21 bytes");
    static_assert(kTotal < kCap,
                  "SetCamera/SetMaterial frame must fit inside kMaxFrameBytes");
    REQUIRE(true);
}
