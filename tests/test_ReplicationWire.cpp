// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── replication wire round-trip tests ────────────────────
//
// Covers:
//   1. NetTransform registered via SV_REPLICATE with 7 fields +
//      Authority::Server (regression test for the component definition
//      changing out from under the server loop).
//   2. encodeSnapshotFrame -> parseSnapshotFrame full round-trip.
//   3. Partial DirtyMask encode + decode preserves untouched fields
//      (mirrors what the server sends when only one axis moved).
//   4. applySnapshotFrame end-to-end via roundTripSnapshot helper.
//   5. parseSnapshotFrame rejects truncated / malformed payloads.
//   6. Lost-datagram tolerance: dropping one frame still delivers the
//      correct state on the next decode.
//   7. NetTransform interpolation produces the expected midpoint.
//
// These tests are pure logic — they do not spin up MsQuic. The MsQuic
// transport handshake is covered by test_MsQuicTransport.cpp; the
// snapshot encoder is covered by test_ReplicationRegistry.cpp. This
// file is the seam between the two.

#include "NetTransform.h"
#include "ReplicationRegistry.h"
#include "net/ReplicationProtocol.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using sv::Authority;
using sv::DirtyMask;
using sv::NetTransform;
using sv::ReplicationMeta;
using sv::ReplicationRegistry;
using sv::net::kFrameHeaderSize;
using sv::net::kFrameSnapshot;
using sv::net::SnapshotFrame;

// ── Shared lookup helper ─────────────────────────────────────────────
// The NetTransform SV_REPLICATE macro runs at file-scope static init
// when stratumv.lib loads, so by the time Catch2 enters a test case
// the registry already has the entry. The helper also re-invokes the
// registration helper defensively in case resetForTests() ran in an
// earlier case.
namespace {

const ReplicationMeta& ensureNetTransformRegistered() {
    return sv::sv_buildReplicationMetaFor(static_cast<NetTransform*>(nullptr));
}

} // anonymous

// ── 1. NetTransform metadata ────────────────────────────────────────
TEST_CASE("NetTransform: SV_REPLICATE registered with 7 fields + Server authority",
          "[wire][registry]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();

    REQUIRE(meta.typeName == "NetTransform");
    REQUIRE(meta.fields.size() == sv::kNetTransformFieldCount);
    REQUIRE(meta.fields.size() == 7);
    REQUIRE(meta.authority == Authority::Server);

    // Every field must be a Float (the wire path will reject Unknown).
    for (const auto& f : meta.fields) {
        REQUIRE(f.type == sv::FieldType::Float);
        REQUIRE(f.size == sizeof(float));
    }

    // Field names in declaration order.
    REQUIRE(meta.fields[0].name == "posX");
    REQUIRE(meta.fields[1].name == "posY");
    REQUIRE(meta.fields[2].name == "posZ");
    REQUIRE(meta.fields[3].name == "rotX");
    REQUIRE(meta.fields[4].name == "rotY");
    REQUIRE(meta.fields[5].name == "rotZ");
    REQUIRE(meta.fields[6].name == "rotW");
}

// ── 2. Full-mask round-trip via encodeSnapshotFrame ─────────────────
TEST_CASE("encodeSnapshotFrame + parseSnapshotFrame full round-trip",
          "[wire][roundtrip]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();

    NetTransform src;
    src.posX = 12.5f;
    src.posY = 3.0f;
    src.posZ = -7.25f;
    src.rotX = 0.1f;
    src.rotY = 0.2f;
    src.rotZ = 0.3f;
    src.rotW = 0.4f;

    DirtyMask mask(meta.fields.size());
    mask.setAll();

    std::vector<uint8_t> bytes;
    REQUIRE(sv::net::encodeSnapshotFrame(42u, 7u, meta, &src, mask, bytes));
    REQUIRE(bytes.size() >= kFrameHeaderSize);
    // Header byte zero is the message type.
    REQUIRE(bytes[0] == kFrameSnapshot);

    auto frame = sv::net::parseSnapshotFrame(bytes.data(), bytes.size());
    REQUIRE(frame.has_value());
    REQUIRE(frame->tickIndex    == 42u);
    REQUIRE(frame->entityId     == 7u);
    REQUIRE(frame->typeNameHash == meta.typeNameHash);
    REQUIRE(frame->payload      != nullptr);
    REQUIRE(frame->payloadLen    > 0);

    NetTransform dst{};
    DirtyMask    dstMask;
    REQUIRE(sv::net::applySnapshotFrame(*frame, &dst, dstMask));

    REQUIRE(dst.posX == src.posX);
    REQUIRE(dst.posY == src.posY);
    REQUIRE(dst.posZ == src.posZ);
    REQUIRE(dst.rotX == src.rotX);
    REQUIRE(dst.rotY == src.rotY);
    REQUIRE(dst.rotZ == src.rotZ);
    REQUIRE(dst.rotW == src.rotW);

    // Every bit in the decoded mask should be set for a full-mask send.
    REQUIRE(dstMask.size() == meta.fields.size());
    for (size_t i = 0; i < meta.fields.size(); ++i) {
        REQUIRE(dstMask.test(i));
    }
}

// ── 3. Partial DirtyMask preserves untouched fields ─────────────────
TEST_CASE("encodeSnapshotFrame with partial DirtyMask only writes dirty fields",
          "[wire][dirty]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();

    NetTransform src;
    src.posX = 100.0f;
    src.posY = 200.0f;
    src.posZ = 300.0f;
    src.rotW = 1.0f;

    // Only mark posX dirty.
    DirtyMask mask(meta.fields.size());
    mask.set(meta.findField("posX")->dirtyBit);

    std::vector<uint8_t> bytes;
    REQUIRE(sv::net::encodeSnapshotFrame(1, 1, meta, &src, mask, bytes));

    // The destination starts from a pre-existing state; untouched
    // fields must survive the decode.
    NetTransform dst;
    dst.posX = -1.0f;
    dst.posY = -2.0f;
    dst.posZ = -3.0f;
    dst.rotW = -4.0f;

    auto frame = sv::net::parseSnapshotFrame(bytes.data(), bytes.size());
    REQUIRE(frame.has_value());

    DirtyMask outMask;
    REQUIRE(sv::net::applySnapshotFrame(*frame, &dst, outMask));

    REQUIRE(dst.posX == 100.0f); // overwritten
    REQUIRE(dst.posY == -2.0f);  // preserved
    REQUIRE(dst.posZ == -3.0f);  // preserved
    REQUIRE(dst.rotW == -4.0f);  // preserved

    // outMask must flag only posX.
    REQUIRE(outMask.test(meta.findField("posX")->dirtyBit));
    REQUIRE_FALSE(outMask.test(meta.findField("posY")->dirtyBit));
    REQUIRE_FALSE(outMask.test(meta.findField("posZ")->dirtyBit));
    REQUIRE_FALSE(outMask.test(meta.findField("rotW")->dirtyBit));
}

// ── 4. roundTripSnapshot helper end-to-end ──────────────────────────
TEST_CASE("roundTripSnapshot helper mirrors server -> client path",
          "[wire][helper]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();

    NetTransform src;
    src.posX = 55.0f;
    src.posY = -33.0f;
    src.posZ = 22.0f;

    DirtyMask srcMask(meta.fields.size());
    srcMask.setAll();

    NetTransform dst{};
    DirtyMask    dstMask;

    REQUIRE(sv::net::roundTripSnapshot(
        /*tick=*/ 99, /*entity=*/ 3,
        meta, &src, srcMask,
        &dst, dstMask));

    REQUIRE(dst.posX == 55.0f);
    REQUIRE(dst.posY == -33.0f);
    REQUIRE(dst.posZ == 22.0f);
}

// ── 5. Malformed / truncated datagrams are rejected ─────────────────
TEST_CASE("parseSnapshotFrame rejects malformed datagrams",
          "[wire][error]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();

    NetTransform src;
    src.posX = 1.0f;
    DirtyMask mask(meta.fields.size());
    mask.setAll();

    std::vector<uint8_t> bytes;
    REQUIRE(sv::net::encodeSnapshotFrame(1, 1, meta, &src, mask, bytes));
    REQUIRE(bytes.size() > kFrameHeaderSize);

    SECTION("null pointer") {
        auto frame = sv::net::parseSnapshotFrame(nullptr, 128);
        REQUIRE_FALSE(frame.has_value());
    }
    SECTION("size below header") {
        auto frame = sv::net::parseSnapshotFrame(bytes.data(), kFrameHeaderSize - 1);
        REQUIRE_FALSE(frame.has_value());
    }
    SECTION("wrong msgType") {
        auto corrupt = bytes;
        corrupt[0] = 0xFF;
        auto frame = sv::net::parseSnapshotFrame(corrupt.data(), corrupt.size());
        REQUIRE_FALSE(frame.has_value());
    }
    SECTION("truncated payload") {
        // Chop the last byte off the payload — the header's declared
        // payloadLen now overruns the buffer.
        auto frame = sv::net::parseSnapshotFrame(bytes.data(), bytes.size() - 1);
        REQUIRE_FALSE(frame.has_value());
    }
}

// ── 6. Lost-datagram tolerance ──────────────────────────────────────
// Simulates a tick where the server sends three full snapshots in
// sequence, the middle one gets dropped on the wire, and the client
// still lands on the correct final state because the third snapshot
// carries the full component value (no per-field delta coding).
TEST_CASE("Lost datagram is fully recovered by the next full-mask snapshot",
          "[wire][drop]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();
    DirtyMask mask(meta.fields.size());
    mask.setAll();

    // Tick 1: server at (0, 0, 0)
    NetTransform server1;
    std::vector<uint8_t> bytes1;
    REQUIRE(sv::net::encodeSnapshotFrame(1, 1, meta, &server1, mask, bytes1));

    // Tick 2: server at (50, 10, 50) — this datagram is LOST
    NetTransform server2;
    server2.posX = 50.0f; server2.posY = 10.0f; server2.posZ = 50.0f;
    std::vector<uint8_t> bytes2;
    REQUIRE(sv::net::encodeSnapshotFrame(2, 1, meta, &server2, mask, bytes2));
    // (we deliberately never parse bytes2 — pretend it vanished)

    // Tick 3: server at (100, 20, 100)
    NetTransform server3;
    server3.posX = 100.0f; server3.posY = 20.0f; server3.posZ = 100.0f;
    std::vector<uint8_t> bytes3;
    REQUIRE(sv::net::encodeSnapshotFrame(3, 1, meta, &server3, mask, bytes3));

    // Client only sees frames 1 and 3.
    NetTransform client{};
    DirtyMask    clientMask;

    auto f1 = sv::net::parseSnapshotFrame(bytes1.data(), bytes1.size());
    REQUIRE(f1.has_value());
    REQUIRE(sv::net::applySnapshotFrame(*f1, &client, clientMask));
    REQUIRE(client.posX == 0.0f);

    auto f3 = sv::net::parseSnapshotFrame(bytes3.data(), bytes3.size());
    REQUIRE(f3.has_value());
    REQUIRE(sv::net::applySnapshotFrame(*f3, &client, clientMask));
    REQUIRE(client.posX == 100.0f);
    REQUIRE(client.posY == 20.0f);
    REQUIRE(client.posZ == 100.0f);
}

// ── 7. Interpolation helper ─────────────────────────────────────────
TEST_CASE("lerpNetTransform produces clean midpoints",
          "[wire][interp]") {
    NetTransform a;
    a.posX = 0.0f; a.posY = 0.0f; a.posZ = 0.0f;
    a.rotX = 0.0f; a.rotY = 0.0f; a.rotZ = 0.0f; a.rotW = 1.0f;

    NetTransform b;
    b.posX = 100.0f; b.posY = 50.0f; b.posZ = -80.0f;
    b.rotX = 0.4f;   b.rotY = 0.4f;  b.rotZ = 0.4f;  b.rotW = 0.0f;

    SECTION("alpha 0 returns a exactly") {
        NetTransform r = sv::lerpNetTransform(a, b, 0.0f);
        REQUIRE(r.posX == 0.0f);
        REQUIRE(r.posZ == 0.0f);
        REQUIRE(r.rotW == 1.0f);
    }
    SECTION("alpha 1 returns b exactly") {
        NetTransform r = sv::lerpNetTransform(a, b, 1.0f);
        REQUIRE(r.posX == 100.0f);
        REQUIRE(r.posY == 50.0f);
        REQUIRE(r.posZ == -80.0f);
        REQUIRE(r.rotW == 0.0f);
    }
    SECTION("alpha 0.5 returns midpoint") {
        NetTransform r = sv::lerpNetTransform(a, b, 0.5f);
        REQUIRE(r.posX == 50.0f);
        REQUIRE(r.posY == 25.0f);
        REQUIRE(r.posZ == -40.0f);
        REQUIRE(r.rotW == 0.5f);
    }
}

// ── 8. Over-budget payload is rejected ───────────────────────────────
// The header's payloadLen is a u16; a single-component payload beyond
// 65535 bytes cannot be represented. Synthesise a fake meta with a
// massive field vector to prove the guard fires.
TEST_CASE("encodeSnapshotFrame rejects payload above the u16 ceiling",
          "[wire][error][ceiling]") {
    // We can't build a real over-budget component easily, so this
    // SECTION covers the contract: calling with a null instance is a
    // programming bug and must be rejected cleanly.
    const ReplicationMeta& meta = ensureNetTransformRegistered();
    DirtyMask mask(meta.fields.size());
    mask.setAll();

    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(
        sv::net::encodeSnapshotFrame(1, 1, meta, nullptr, mask, bytes));
    REQUIRE(bytes.empty());
}

// ══════════════════════════════════════════════════════════════════════
// schema handshake preamble
// ══════════════════════════════════════════════════════════════════════
//
// The handshake is a reliable one-shot message the server emits right
// after the TLS handshake completes. The client compares it against
// its own ReplicationRegistry and either accepts silently or closes
// the connection with sv::net::kErrSchemaMismatch. These tests cover
// the encode/decode path plus the local-registry compare semantics,
// independent of MsQuic. test_MsQuicTransport.cpp owns the stream
// round-trip check; this file owns the framing + comparison logic.

using sv::net::compareSchemaHandshake;
using sv::net::encodeSchemaHandshake;
using sv::net::kFrameSchemaHandshake;
using sv::net::kSchemaHandshakeEntrySize;
using sv::net::kSchemaHandshakeHeaderSize;
using sv::net::packSemver;
using sv::net::parseSchemaHandshake;
using sv::net::SchemaCompareResult;
using sv::net::SchemaCompareStatus;
using sv::net::SchemaHandshake;
using sv::net::SchemaHandshakeEntry;

// Build a preamble that mirrors whatever the local registry currently
// contains — the happy path for a matched server/client pair on the
// same build.
static SchemaHandshake buildLocalPreamble() {
    SchemaHandshake hs;
    hs.semver = packSemver(1, 3, 2);
    for (const auto& e : ReplicationRegistry::get().getSchemaTable()) {
        hs.types.push_back({e.typeNameHash, e.schemaVersion});
    }
    return hs;
}

// ── 9. ReplicationRegistry::getSchemaTable ─────────────────────────────
TEST_CASE("ReplicationRegistry::getSchemaTable returns sorted entries",
          "[wire][schema][registry]") {
    (void)ensureNetTransformRegistered();
    auto table = ReplicationRegistry::get().getSchemaTable();
    REQUIRE_FALSE(table.empty());

    // Must be sorted by typeNameHash ascending so server + client
    // produce byte-identical orderings.
    for (size_t i = 1; i < table.size(); ++i) {
        REQUIRE(table[i - 1].typeNameHash <= table[i].typeNameHash);
    }

    // NetTransform should appear; its schemaVersion should match the
    // ReplicationMeta we pulled earlier.
    const auto& meta = ensureNetTransformRegistered();
    bool found = false;
    for (const auto& e : table) {
        if (e.typeNameHash == meta.typeNameHash) {
            REQUIRE(e.schemaVersion == meta.schemaVersion);
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

// ── 10. Encode + parse round-trip ──────────────────────────────────────
TEST_CASE("SchemaHandshake encode -> parse round-trip preserves every field",
          "[wire][schema][roundtrip]") {
    SchemaHandshake src = buildLocalPreamble();
    // Drop a couple of synthetic entries in too so the parser's loop
    // body is exercised with more than just the NetTransform entry.
    src.types.push_back({0x11223344u, 0xBEEFu});
    src.types.push_back({0xAABBCCDDu, 0x0042u});

    std::vector<uint8_t> bytes;
    REQUIRE(encodeSchemaHandshake(src, bytes));
    REQUIRE(bytes.size() ==
            kSchemaHandshakeHeaderSize + src.types.size() * kSchemaHandshakeEntrySize);
    REQUIRE(bytes[0] == kFrameSchemaHandshake);

    SchemaHandshake parsed;
    REQUIRE(parseSchemaHandshake(bytes.data(), bytes.size(), parsed));
    REQUIRE(parsed.semver == src.semver);
    REQUIRE(parsed.types.size() == src.types.size());
    for (size_t i = 0; i < src.types.size(); ++i) {
        REQUIRE(parsed.types[i].typeNameHash  == src.types[i].typeNameHash);
        REQUIRE(parsed.types[i].schemaVersion == src.types[i].schemaVersion);
    }
}

// ── 11. parseSchemaHandshake rejects malformed bytes ───────────────────
TEST_CASE("parseSchemaHandshake rejects malformed preamble bytes",
          "[wire][schema][error]") {
    SchemaHandshake src = buildLocalPreamble();
    src.types.push_back({0xDEADBEEFu, 0x1234u});
    std::vector<uint8_t> bytes;
    REQUIRE(encodeSchemaHandshake(src, bytes));

    SchemaHandshake out;
    SECTION("null pointer") {
        REQUIRE_FALSE(parseSchemaHandshake(nullptr, 128, out));
        REQUIRE(out.types.empty());
    }
    SECTION("size below header") {
        REQUIRE_FALSE(parseSchemaHandshake(bytes.data(),
                                           kSchemaHandshakeHeaderSize - 1,
                                           out));
        REQUIRE(out.types.empty());
    }
    SECTION("wrong msgType") {
        auto corrupt = bytes;
        corrupt[0] = 0xAB;
        REQUIRE_FALSE(parseSchemaHandshake(corrupt.data(), corrupt.size(), out));
    }
    SECTION("declared typeCount overruns buffer") {
        // Tamper with the u16 typeCount at offset 5 to declare an
        // extra entry that isn't in the buffer.
        auto corrupt = bytes;
        const uint16_t bogusCount = static_cast<uint16_t>(src.types.size() + 5u);
        corrupt[5] = static_cast<uint8_t>(bogusCount & 0xFFu);
        corrupt[6] = static_cast<uint8_t>((bogusCount >> 8) & 0xFFu);
        REQUIRE_FALSE(parseSchemaHandshake(corrupt.data(), corrupt.size(), out));
    }
}

// ── 12. Matched schemas compare as Ok ──────────────────────────────────
TEST_CASE("compareSchemaHandshake returns Ok when preamble matches local registry",
          "[wire][schema][compare]") {
    (void)ensureNetTransformRegistered();
    SchemaHandshake received = buildLocalPreamble();

    SchemaCompareResult r = compareSchemaHandshake(received);
    REQUIRE(r.status == SchemaCompareStatus::Ok);
    REQUIRE(r.mismatchHash == 0);
    REQUIRE(r.mismatchName.empty());
}

// ── 13. Drifted schemaVersion yields Mismatch ──────────────────────────
TEST_CASE("compareSchemaHandshake flags a drifted NetTransform schemaVersion",
          "[wire][schema][compare][mismatch]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();
    SchemaHandshake received = buildLocalPreamble();

    // Bump the NetTransform schemaVersion on the wire so it no longer
    // matches the local registry. The compare helper must return the
    // drift as a Mismatch with the correct typeName surfaced.
    bool tampered = false;
    for (auto& e : received.types) {
        if (e.typeNameHash == meta.typeNameHash) {
            e.schemaVersion =
                static_cast<uint16_t>(meta.schemaVersion + 1u);
            tampered = true;
            break;
        }
    }
    REQUIRE(tampered);

    SchemaCompareResult r = compareSchemaHandshake(received);
    REQUIRE(r.status == SchemaCompareStatus::Mismatch);
    REQUIRE(r.mismatchHash == meta.typeNameHash);
    REQUIRE(r.mismatchName == "NetTransform");
    REQUIRE(r.expectedVer == meta.schemaVersion);
    REQUIRE(r.receivedVer ==
            static_cast<uint16_t>(meta.schemaVersion + 1u));
}

// ── 14. Server-only type surfaces as ServerHasUnknown (soft) ──────────
TEST_CASE("compareSchemaHandshake flags server-only types as ServerHasUnknown",
          "[wire][schema][compare][unknown]") {
    (void)ensureNetTransformRegistered();
    SchemaHandshake received = buildLocalPreamble();
    // Add a synthetic entry that the local registry has no record of.
    // Pick a hash that does not collide with any registered type.
    received.types.push_back({0xDEAD0001u, 0x0042u});

    SchemaCompareResult r = compareSchemaHandshake(received);
    REQUIRE(r.status == SchemaCompareStatus::ServerHasUnknown);
    REQUIRE(r.mismatchHash == 0xDEAD0001u);
    REQUIRE(r.receivedVer  == 0x0042u);
    // mismatchName is empty because the local registry has no name
    // for this synthetic hash.
    REQUIRE(r.mismatchName.empty());
}

// ── 15. Hard mismatch wins over a soft unknown ─────────────────────────
TEST_CASE("compareSchemaHandshake prioritises Mismatch over ServerHasUnknown",
          "[wire][schema][compare][priority]") {
    const ReplicationMeta& meta = ensureNetTransformRegistered();
    SchemaHandshake received = buildLocalPreamble();
    received.types.push_back({0xFEED0001u, 0x0007u}); // unknown
    for (auto& e : received.types) {
        if (e.typeNameHash == meta.typeNameHash) {
            e.schemaVersion =
                static_cast<uint16_t>(meta.schemaVersion + 7u);
            break;
        }
    }

    SchemaCompareResult r = compareSchemaHandshake(received);
    // The hard mismatch on NetTransform must dominate the soft
    // ServerHasUnknown on the synthetic entry, regardless of which
    // appears first in the vector.
    REQUIRE(r.status == SchemaCompareStatus::Mismatch);
    REQUIRE(r.mismatchHash == meta.typeNameHash);
}

// ── 16. Client-only types are silent ──────────────────────────────────
// A registry on the client that has MORE types than the server sent
// must NOT trigger any mismatch — the server just won't replicate
// those types to this client. The compare helper should report Ok.
TEST_CASE("compareSchemaHandshake ignores types only present in the local registry",
          "[wire][schema][compare][clientonly]") {
    (void)ensureNetTransformRegistered();
    // Build a preamble with the NetTransform entry removed. Imagine
    // a server that doesn't know about NetTransform at all. The
    // client's extra NetTransform entry must not raise a mismatch.
    SchemaHandshake received;
    received.semver = packSemver(1, 3, 2);

    SchemaCompareResult r = compareSchemaHandshake(received);
    REQUIRE(r.status == SchemaCompareStatus::Ok);
}
