// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── EditTransaction + UndoLog + WelcomeMessage tests ──────
// Pure logic — these live in sv_core_tests (the Layer 4 subset) so
// they run identically on full Windows builds and on the Linux
// core-only carve-out. Tagged [edit] with sub-tags per area.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "EditTransaction.h"
#include "NetTransform.h"
#include "ParentLink.h"
#include "PermissionScope.h"
#include "ReplicationRegistry.h"
#include "UndoLog.h"
#include "net/ReplicationProtocol.h"

#include <cstring>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

// Fresh NetTransform builder so tests don't rely on default-init.
sv::NetTransform makeTransform(float base) {
    sv::NetTransform t;
    t.posX = base;
    t.posY = base + 1.0f;
    t.posZ = base + 2.0f;
    t.rotX = 0.1f;
    t.rotY = 0.2f;
    t.rotZ = 0.3f;
    t.rotW = 0.9f;
    return t;
}

// Assertion helper: every float matches within a tight tolerance.
void requireTransformsEqual(const sv::NetTransform& a, const sv::NetTransform& b) {
    REQUIRE(a.posX == Approx(b.posX));
    REQUIRE(a.posY == Approx(b.posY));
    REQUIRE(a.posZ == Approx(b.posZ));
    REQUIRE(a.rotX == Approx(b.rotX));
    REQUIRE(a.rotY == Approx(b.rotY));
    REQUIRE(a.rotZ == Approx(b.rotZ));
    REQUIRE(a.rotW == Approx(b.rotW));
}

} // namespace

// ── PermissionScope ladder ─────────────────────────────────────────

TEST_CASE("PermissionScope: ladder comparison", "[edit][scope]") {
    using S = sv::PermissionScope;
    REQUIRE(S::Admin  >= S::Editor);
    REQUIRE(S::Editor >= S::Player);
    REQUIRE(S::Player >= S::Spectator);
    REQUIRE_FALSE(S::Spectator >= S::Player);
    REQUIRE_FALSE(S::Editor    >  S::Editor);
    REQUIRE(S::Editor <= S::Editor);
    REQUIRE(S::Spectator < S::Admin);
}

TEST_CASE("PermissionScope: toString", "[edit][scope]") {
    using S = sv::PermissionScope;
    REQUIRE(std::string("Spectator") == sv::permissionScopeToString(S::Spectator));
    REQUIRE(std::string("Player")    == sv::permissionScopeToString(S::Player));
    REQUIRE(std::string("Editor")    == sv::permissionScopeToString(S::Editor));
    REQUIRE(std::string("Admin")     == sv::permissionScopeToString(S::Admin));
}

TEST_CASE("PermissionScope: fromByte narrows out-of-range to Spectator",
          "[edit][scope]") {
    REQUIRE(sv::permissionScopeFromByte(0)   == sv::PermissionScope::Spectator);
    REQUIRE(sv::permissionScopeFromByte(1)   == sv::PermissionScope::Player);
    REQUIRE(sv::permissionScopeFromByte(2)   == sv::PermissionScope::Editor);
    REQUIRE(sv::permissionScopeFromByte(3)   == sv::PermissionScope::Admin);
    // Anything outside 0..3 must NOT elevate.
    REQUIRE(sv::permissionScopeFromByte(4)   == sv::PermissionScope::Spectator);
    REQUIRE(sv::permissionScopeFromByte(255) == sv::PermissionScope::Spectator);
}

// ── EditKind diagnostics ───────────────────────────────────────────

TEST_CASE("EditKind: toString covers every value", "[edit][kind]") {
    REQUIRE(std::string("SetField") == sv::editKindToString(sv::EditKind::SetField));
    REQUIRE(std::string("Undo")     == sv::editKindToString(sv::EditKind::Undo));
    REQUIRE(std::string("Redo")     == sv::editKindToString(sv::EditKind::Redo));
    REQUIRE(std::string("Spawn")    == sv::editKindToString(sv::EditKind::Spawn));
    REQUIRE(std::string("Despawn")  == sv::editKindToString(sv::EditKind::Despawn));
}

// ── NetTransform wire layout ───────────────────────────────────────

TEST_CASE("NetTransform wire: round-trip preserves every field", "[edit][wire]") {
    const sv::NetTransform src = makeTransform(123.456f);
    std::vector<uint8_t> buf;
    sv::writeNetTransformLE(src, buf);

    REQUIRE(buf.size() == sv::kNetTransformWireSize);

    auto parsed = sv::readNetTransformLE(buf.data(), buf.size());
    REQUIRE(parsed.has_value());
    requireTransformsEqual(src, *parsed);
}

TEST_CASE("NetTransform wire: short buffer returns nullopt", "[edit][wire]") {
    uint8_t tiny[1] = {0};
    REQUIRE_FALSE(sv::readNetTransformLE(tiny, sizeof(tiny)).has_value());
    REQUIRE_FALSE(sv::readNetTransformLE(nullptr, 1000).has_value());
}

// ── Spawn payload ──────────────────────────────────────────────────

TEST_CASE("Spawn payload: round-trip", "[edit][wire][spawn]") {
    const sv::NetTransform src = makeTransform(-5.0f);
    const uint32_t owner = 42u;
    std::vector<uint8_t> buf;
    sv::writeSpawnPayload(src, owner, buf);

    REQUIRE(buf.size() == sv::kSpawnPayloadSize);

    sv::NetTransform out;
    uint32_t outOwner = 0;
    REQUIRE(sv::readSpawnPayload(buf.data(), buf.size(), out, outOwner));
    requireTransformsEqual(src, out);
    REQUIRE(outOwner == owner);
}

TEST_CASE("Spawn payload: short buffer rejected", "[edit][wire][spawn]") {
    uint8_t tiny[sv::kNetTransformWireSize] = {0}; // one byte short of kSpawnPayloadSize
    sv::NetTransform out;
    uint32_t outOwner = 0;
    REQUIRE_FALSE(sv::readSpawnPayload(tiny, sizeof(tiny), out, outOwner));
}

// ── EditTransaction wire header ────────────────────────────────────

TEST_CASE("EditTransaction: SetField round-trip preserves header + payload",
          "[edit][wire][roundtrip]") {
    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::SetField;
    tx.txId           = 0x0123456789ABCDEFu;
    tx.originClientId = 7u;
    tx.requiredScope  = sv::PermissionScope::Editor;
    tx.entityId       = 105u;
    tx.typeNameHash   = sv::fnv1a32("NetTransform");
    tx.timestampMs    = 1234567890ull;
    sv::writeNetTransformLE(makeTransform(10.0f), tx.payload);

    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeEditTransaction(tx, bytes));
    REQUIRE(bytes.size() == sv::kEditTransactionHeaderSize + sv::kNetTransformWireSize);
    // First byte must be the wire message type so parseEditTransaction
    // can discriminate on the reliable stream.
    REQUIRE(bytes[0] == sv::net::kFrameEditTransaction);

    auto parsed = sv::parseEditTransaction(bytes.data(), bytes.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind           == sv::EditKind::SetField);
    REQUIRE(parsed->txId           == tx.txId);
    REQUIRE(parsed->originClientId == tx.originClientId);
    REQUIRE(parsed->requiredScope  == tx.requiredScope);
    REQUIRE(parsed->entityId       == tx.entityId);
    REQUIRE(parsed->typeNameHash   == tx.typeNameHash);
    REQUIRE(parsed->timestampMs    == tx.timestampMs);
    REQUIRE(parsed->payload.size() == sv::kNetTransformWireSize);

    auto decoded = sv::readNetTransformLE(parsed->payload.data(),
                                          parsed->payload.size());
    REQUIRE(decoded.has_value());
    requireTransformsEqual(makeTransform(10.0f), *decoded);
}

TEST_CASE("EditTransaction: Spawn round-trip preserves owner", "[edit][wire][spawn]") {
    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::Spawn;
    tx.txId           = 1u;
    tx.originClientId = 0u; // server
    tx.requiredScope  = sv::PermissionScope::Admin;
    tx.entityId       = 100u;
    tx.typeNameHash   = sv::fnv1a32("NetTransform");
    tx.timestampMs    = 42u;
    sv::writeSpawnPayload(makeTransform(-2.5f), 3u, tx.payload);

    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeEditTransaction(tx, bytes));
    auto parsed = sv::parseEditTransaction(bytes.data(), bytes.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind == sv::EditKind::Spawn);

    sv::NetTransform state;
    uint32_t owner = 0;
    REQUIRE(sv::readSpawnPayload(parsed->payload.data(), parsed->payload.size(),
                                 state, owner));
    requireTransformsEqual(makeTransform(-2.5f), state);
    REQUIRE(owner == 3u);
}

TEST_CASE("EditTransaction: Undo/Redo/Despawn encode as header-only",
          "[edit][wire]") {
    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::Despawn;
    tx.txId           = 9u;
    tx.originClientId = 0u;
    tx.requiredScope  = sv::PermissionScope::Admin;
    tx.entityId       = 107u;
    tx.typeNameHash   = 0;
    tx.timestampMs    = 1u;
    // Empty payload.

    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeEditTransaction(tx, bytes));
    REQUIRE(bytes.size() == sv::kEditTransactionHeaderSize);

    auto parsed = sv::parseEditTransaction(bytes.data(), bytes.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind == sv::EditKind::Despawn);
    REQUIRE(parsed->entityId == 107u);
    REQUIRE(parsed->payload.empty());
}

TEST_CASE("EditTransaction: parse rejects short buffer", "[edit][wire][error]") {
    uint8_t header[16] = {sv::net::kFrameEditTransaction};
    REQUIRE_FALSE(sv::parseEditTransaction(header, sizeof(header)).has_value());
}

TEST_CASE("EditTransaction: parse rejects wrong msgType", "[edit][wire][error]") {
    std::vector<uint8_t> bytes(sv::kEditTransactionHeaderSize, 0);
    bytes[0] = 0xFF; // not kFrameEditTransaction
    REQUIRE_FALSE(sv::parseEditTransaction(bytes.data(), bytes.size()).has_value());
}

TEST_CASE("EditTransaction: parse rejects payload overrun", "[edit][wire][error]") {
    sv::EditTransaction tx;
    tx.kind = sv::EditKind::SetField;
    tx.entityId = 1;
    sv::writeNetTransformLE(makeTransform(0.0f), tx.payload);

    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeEditTransaction(tx, bytes));
    // Forge a payloadLen byte that exceeds the remaining buffer.
    bytes[31] = 0xFF;
    bytes[32] = 0xFF;
    REQUIRE_FALSE(sv::parseEditTransaction(bytes.data(), bytes.size()).has_value());
}

// ── Welcome message ────────────────────────────────────────────────

TEST_CASE("WelcomeMessage: round-trip", "[edit][welcome][wire]") {
    sv::net::WelcomeMessage w;
    w.clientId       = 42u;
    w.scope          = static_cast<uint8_t>(sv::PermissionScope::Editor);
    w.avatarEntityId = 105u;

    std::vector<uint8_t> bytes;
    REQUIRE(sv::net::encodeWelcomeMessage(w, bytes));
    REQUIRE(bytes.size() == sv::net::kWelcomeMessageSize);
    REQUIRE(bytes[0] == sv::net::kFrameWelcome);

    sv::net::WelcomeMessage parsed;
    REQUIRE(sv::net::parseWelcomeMessage(bytes.data(), bytes.size(), parsed));
    REQUIRE(parsed.clientId       == 42u);
    REQUIRE(parsed.scope          == static_cast<uint8_t>(sv::PermissionScope::Editor));
    REQUIRE(parsed.avatarEntityId == 105u);
}

TEST_CASE("WelcomeMessage: parse rejects short or wrong-type buffer",
          "[edit][welcome][error]") {
    sv::net::WelcomeMessage parsed;

    uint8_t tiny[5] = {sv::net::kFrameWelcome};
    REQUIRE_FALSE(sv::net::parseWelcomeMessage(tiny, sizeof(tiny), parsed));

    std::vector<uint8_t> bytes(sv::net::kWelcomeMessageSize, 0);
    bytes[0] = 0xFF; // wrong type
    REQUIRE_FALSE(sv::net::parseWelcomeMessage(bytes.data(), bytes.size(), parsed));
}

// ── UndoLog LIFO semantics ─────────────────────────────────────────

TEST_CASE("UndoLog: empty log has no undoable or redoable", "[edit][undo]") {
    sv::UndoLog log;
    REQUIRE(log.empty());
    REQUIRE(log.size() == 0);
    REQUIRE(log.findLatestUndoable(1) == nullptr);
    REQUIRE(log.findLatestRedoable(1) == nullptr);
    REQUIRE(log.undoableCount(1) == 0);
    REQUIRE(log.redoableCount(1) == 0);
}

TEST_CASE("UndoLog: recordApplied then findLatestUndoable returns the entry",
          "[edit][undo]") {
    sv::UndoLog log;
    sv::UndoEntry e;
    e.txId = 100;
    e.originClientId = 7;
    e.entityId = 105;
    e.typeNameHash = sv::fnv1a32("NetTransform");
    e.beforeState = makeTransform(0.0f);
    e.afterState  = makeTransform(10.0f);
    log.recordApplied(e);

    REQUIRE(log.size() == 1);
    const sv::UndoEntry* found = log.findLatestUndoable(7);
    REQUIRE(found != nullptr);
    REQUIRE(found->txId == 100);
    REQUIRE(found->undone == false);

    // Other client sees nothing.
    REQUIRE(log.findLatestUndoable(99) == nullptr);
}

TEST_CASE("UndoLog: per-client LIFO walkback", "[edit][undo]") {
    sv::UndoLog log;
    for (uint64_t i = 1; i <= 4; ++i) {
        sv::UndoEntry e;
        e.txId = i;
        e.originClientId = (i % 2 == 0) ? 2u : 1u; // alternating owners
        e.entityId = 100 + static_cast<uint32_t>(i);
        log.recordApplied(e);
    }
    // Client 1 owns tx 1, 3 — latest is 3
    const sv::UndoEntry* u1 = log.findLatestUndoable(1);
    REQUIRE(u1 != nullptr);
    REQUIRE(u1->txId == 3);
    // Client 2 owns tx 2, 4 — latest is 4
    const sv::UndoEntry* u2 = log.findLatestUndoable(2);
    REQUIRE(u2 != nullptr);
    REQUIRE(u2->txId == 4);
}

TEST_CASE("UndoLog: markUndone flips the flag and excludes from undoable walk",
          "[edit][undo]") {
    sv::UndoLog log;
    sv::UndoEntry e1; e1.txId = 1; e1.originClientId = 5; log.recordApplied(e1);
    sv::UndoEntry e2; e2.txId = 2; e2.originClientId = 5; log.recordApplied(e2);

    REQUIRE(log.markUndone(2));
    // Latest undoable is now tx 1
    const sv::UndoEntry* u = log.findLatestUndoable(5);
    REQUIRE(u != nullptr);
    REQUIRE(u->txId == 1);
    // Latest redoable is tx 2
    const sv::UndoEntry* r = log.findLatestRedoable(5);
    REQUIRE(r != nullptr);
    REQUIRE(r->txId == 2);
}

TEST_CASE("UndoLog: markUndone returns false on unknown txId", "[edit][undo]") {
    sv::UndoLog log;
    REQUIRE_FALSE(log.markUndone(99999));
    REQUIRE_FALSE(log.markRedone(99999));
}

TEST_CASE("UndoLog: undo then redo restores position", "[edit][undo]") {
    sv::UndoLog log;
    sv::UndoEntry e1; e1.txId = 10; e1.originClientId = 1; log.recordApplied(e1);
    sv::UndoEntry e2; e2.txId = 20; e2.originClientId = 1; log.recordApplied(e2);

    REQUIRE(log.markUndone(20));
    REQUIRE(log.undoableCount(1) == 1);
    REQUIRE(log.redoableCount(1) == 1);

    REQUIRE(log.markRedone(20));
    REQUIRE(log.undoableCount(1) == 2);
    REQUIRE(log.redoableCount(1) == 0);
}

TEST_CASE("UndoLog: undoable/redoable counts are per-client", "[edit][undo]") {
    sv::UndoLog log;
    for (uint64_t i = 1; i <= 6; ++i) {
        sv::UndoEntry e;
        e.txId = i;
        e.originClientId = (i <= 4) ? 1u : 2u;
        log.recordApplied(e);
    }
    REQUIRE(log.undoableCount(1) == 4);
    REQUIRE(log.undoableCount(2) == 2);
    REQUIRE(log.undoableCount(3) == 0);
    log.markUndone(4);
    REQUIRE(log.undoableCount(1) == 3);
    REQUIRE(log.redoableCount(1) == 1);
}

// ── Generic payload dispatch ──────────────────────────────

TEST_CASE("Generic SetField payload: NetTransform round-trip via ReplicationRegistry",
          "[edit][wire][generic]") {
    // Force NetTransform into the registry — identical anchor to the
    // one the server + lab call from main().
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);

    const sv::NetTransform src = makeTransform(17.25f);
    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, payload));
    REQUIRE(payload.size() > 0);

    sv::NetTransform dst;
    sv::DirtyMask dstMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                           payload.data(),
                                           payload.size(),
                                           &dst,
                                           dstMask));
    requireTransformsEqual(src, dst);
    // Every field should come back dirty because we encoded with
    // a full mask.
    for (size_t i = 0; i < meta->fields.size(); ++i) {
        REQUIRE(dstMask.test(i));
    }
}

TEST_CASE("Generic SetField payload: partial mask leaves untouched fields alone",
          "[edit][wire][generic]") {
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);

    // Source carries a partial mask (posX + posZ only). Destination
    // starts with pre-populated fields that must survive the decode
    // for bits that were never marked dirty.
    const sv::FieldDesc* posX = meta->findField("posX");
    const sv::FieldDesc* posZ = meta->findField("posZ");
    REQUIRE(posX != nullptr);
    REQUIRE(posZ != nullptr);

    sv::NetTransform src = makeTransform(0.0f);
    src.posX = 99.0f;
    src.posZ = 42.0f;
    sv::DirtyMask partial(meta->fields.size());
    partial.set(posX->dirtyBit);
    partial.set(posZ->dirtyBit);

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, partial, payload));

    // Destination has different pre-filled values — decodeSnapshot
    // should leave posY/rotX/rotY/rotZ/rotW alone and only overwrite
    // posX + posZ.
    sv::NetTransform dst;
    dst.posX = -1.0f;
    dst.posY = 500.5f;
    dst.posZ = -1.0f;
    dst.rotX = 0.75f;
    dst.rotY = 0.5f;
    dst.rotZ = 0.25f;
    dst.rotW = 0.0f;
    sv::DirtyMask dstMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                           payload.data(),
                                           payload.size(),
                                           &dst,
                                           dstMask));

    REQUIRE(dst.posX == Approx(99.0f));
    REQUIRE(dst.posY == Approx(500.5f));   // untouched
    REQUIRE(dst.posZ == Approx(42.0f));
    REQUIRE(dst.rotX == Approx(0.75f));    // untouched
    REQUIRE(dst.rotW == Approx(0.0f));     // untouched

    REQUIRE(dstMask.test(posX->dirtyBit));
    REQUIRE_FALSE(dstMask.test(meta->findField("posY")->dirtyBit));
    REQUIRE(dstMask.test(posZ->dirtyBit));
}

TEST_CASE("Generic SetField payload: rejects null / unknown type / short buffer",
          "[edit][wire][generic][error]") {
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);
    sv::NetTransform dst;
    sv::DirtyMask dstMask(meta->fields.size());

    // Null instance on encode
    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();
    std::vector<uint8_t> buf;
    REQUIRE_FALSE(sv::writeGenericSetFieldPayload(*meta, nullptr, fullMask, buf));

    // Unknown typeNameHash on decode
    const sv::NetTransform src = makeTransform(5.0f);
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, buf));
    REQUIRE_FALSE(sv::readGenericSetFieldPayload(/*typeNameHash=*/0xDEADBEEF,
                                                 buf.data(), buf.size(),
                                                 &dst, dstMask));

    // Short buffer (below schemaVersion header)
    REQUIRE_FALSE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                                 buf.data(), 1,
                                                 &dst, dstMask));
}

TEST_CASE("Generic Spawn payload: 4-byte owner prefix + full-mask snapshot",
          "[edit][wire][generic][spawn]") {
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);

    const sv::NetTransform src = makeTransform(-7.5f);
    const uint32_t owner = 12345u;

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSpawnPayload(*meta, &src, owner, payload));
    REQUIRE(payload.size() >= sizeof(uint32_t));

    // First four bytes should be the owner LE.
    REQUIRE(payload[0] == static_cast<uint8_t>(owner & 0xFF));
    REQUIRE(payload[1] == static_cast<uint8_t>((owner >> 8) & 0xFF));
    REQUIRE(payload[2] == static_cast<uint8_t>((owner >> 16) & 0xFF));
    REQUIRE(payload[3] == static_cast<uint8_t>((owner >> 24) & 0xFF));

    sv::NetTransform dst;
    uint32_t decodedOwner = 0;
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSpawnPayload(meta->typeNameHash,
                                        payload.data(),
                                        payload.size(),
                                        decodedOwner,
                                        &dst,
                                        decodedMask));
    REQUIRE(decodedOwner == owner);
    requireTransformsEqual(src, dst);
}

TEST_CASE("Generic Spawn payload: short buffer rejected", "[edit][wire][generic][error]") {
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);

    uint8_t tiny[3] = {0xFF, 0xFF, 0xFF};   // less than 4 bytes
    sv::NetTransform dst;
    uint32_t owner = 0;
    sv::DirtyMask dstMask(meta->fields.size());
    REQUIRE_FALSE(sv::readGenericSpawnPayload(meta->typeNameHash,
                                              tiny, sizeof(tiny),
                                              owner, &dst, dstMask));
}

TEST_CASE("Generic SetField payload: end-to-end via full EditTransaction round-trip",
          "[edit][wire][generic][roundtrip]") {
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);

    // Build a transaction whose payload comes from the generic
    // encoder, then run it through encodeEditTransaction +
    // parseEditTransaction + readGenericSetFieldPayload and verify
    // every field survives.
    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::SetField;
    tx.txId           = 9001u;
    tx.originClientId = 3u;
    tx.requiredScope  = sv::PermissionScope::Editor;
    tx.entityId       = 200u;
    tx.typeNameHash   = meta->typeNameHash;
    tx.timestampMs    = 123456u;

    const sv::NetTransform src = makeTransform(3.1415f);
    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, tx.payload));

    std::vector<uint8_t> wire;
    REQUIRE(sv::encodeEditTransaction(tx, wire));
    auto parsed = sv::parseEditTransaction(wire.data(), wire.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->typeNameHash == meta->typeNameHash);

    sv::NetTransform dst;
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(parsed->typeNameHash,
                                           parsed->payload.data(),
                                           parsed->payload.size(),
                                           &dst,
                                           decodedMask));
    requireTransformsEqual(src, dst);
}

// ── kEditTransactionHeaderSize layout invariant ────────────────────

TEST_CASE("Wire layout constants match hand-computed values",
          "[edit][wire][layout]") {
    // Hand-sum of the field widths in the header. If this breaks
    // the wire format is skewed and both encode and parse need to
    // be updated in lockstep.
    constexpr size_t kHandSum =
          1   // msgType
        + 1   // kind
        + 8   // txId
        + 4   // originClientId
        + 1   // requiredScope
        + 4   // entityId
        + 4   // typeNameHash
        + 8   // timestampMs
        + 2;  // payloadLen
    static_assert(kHandSum == sv::kEditTransactionHeaderSize,
                  "kEditTransactionHeaderSize drifted from wire layout");
    static_assert(sv::kNetTransformWireSize == 7 * 4,
                  "kNetTransformWireSize should match 7 * sizeof(float)");
    static_assert(sv::kSpawnPayloadSize     == 7 * 4 + 4,
                  "kSpawnPayloadSize should be NetTransform + u32 ownerClientId");
    REQUIRE(true); // silence "TEST_CASE with no assertions" warning
}

// ══════════════════════════════════════════════════════════════════
// ParentLink replicated component
// ══════════════════════════════════════════════════════════════════

TEST_CASE("ParentLink: SV_REPLICATE registers 1 field + Owner authority",
          "[edit][parentlink]") {
    (void)sv::ensureParentLinkRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("ParentLink");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == sv::kParentLinkFieldCount);
    REQUIRE(meta->fields.size() == 1);
    REQUIRE(std::string("parentEntityId") == meta->fields[0].name);
    REQUIRE(meta->fields[0].type == sv::FieldType::UInt32);
    REQUIRE(meta->authority == sv::Authority::Owner);
}

TEST_CASE("ParentLink: SetField payload round-trip via generic path",
          "[edit][parentlink][wire]") {
    (void)sv::ensureParentLinkRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("ParentLink");
    REQUIRE(meta != nullptr);

    sv::ParentLink src;
    src.parentEntityId = 0xDEADBEEFu;

    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, payload));

    sv::ParentLink dst;
    dst.parentEntityId = 0xA5A5A5A5u;   // pre-populate with garbage
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                           payload.data(),
                                           payload.size(),
                                           &dst,
                                           decodedMask));
    REQUIRE(dst.parentEntityId == 0xDEADBEEFu);
    REQUIRE(decodedMask.test(0));
}

TEST_CASE("ParentLink: zero-mask SetField preserves existing state",
          "[edit][parentlink][wire]") {
    (void)sv::ensureParentLinkRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("ParentLink");
    REQUIRE(meta != nullptr);

    sv::ParentLink src;
    src.parentEntityId = 12345u;

    sv::DirtyMask noMask(meta->fields.size());   // all zero

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, noMask, payload));

    sv::ParentLink dst;
    dst.parentEntityId = 99u;
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                           payload.data(),
                                           payload.size(),
                                           &dst,
                                           decodedMask));
    // With a zero mask the decoder should not touch the target.
    REQUIRE(dst.parentEntityId == 99u);
    REQUIRE_FALSE(decodedMask.test(0));
}

TEST_CASE("ParentLink: EditTransaction end-to-end with encode/parse",
          "[edit][parentlink][roundtrip]") {
    (void)sv::ensureParentLinkRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("ParentLink");
    REQUIRE(meta != nullptr);

    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::SetField;
    tx.txId           = 7u;
    tx.originClientId = 42u;
    tx.requiredScope  = sv::PermissionScope::Editor;
    tx.entityId       = 101u;
    tx.typeNameHash   = meta->typeNameHash;
    tx.timestampMs    = 9876u;

    sv::ParentLink src;
    src.parentEntityId = 1u;
    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, tx.payload));

    std::vector<uint8_t> wire;
    REQUIRE(sv::encodeEditTransaction(tx, wire));
    auto parsed = sv::parseEditTransaction(wire.data(), wire.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->typeNameHash == meta->typeNameHash);
    REQUIRE(parsed->entityId == 101u);

    sv::ParentLink dst;
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(parsed->typeNameHash,
                                           parsed->payload.data(),
                                           parsed->payload.size(),
                                           &dst,
                                           decodedMask));
    REQUIRE(dst.parentEntityId == 1u);
}

TEST_CASE("ParentLink: readGeneric with wrong typeNameHash is rejected",
          "[edit][parentlink][error]") {
    (void)sv::ensureParentLinkRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("ParentLink");
    REQUIRE(meta != nullptr);

    sv::ParentLink src;
    src.parentEntityId = 5u;
    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, payload));

    sv::ParentLink dst;
    sv::DirtyMask decodedMask(meta->fields.size());
    // Flip the typeNameHash to a bogus value — readGenericSetFieldPayload
    // should refuse to dispatch.
    REQUIRE_FALSE(sv::readGenericSetFieldPayload(meta->typeNameHash ^ 0xFFFFFFFFu,
                                                  payload.data(),
                                                  payload.size(),
                                                  &dst,
                                                  decodedMask));
}

TEST_CASE("ParentLink: ensureParentLinkRegistered is idempotent",
          "[edit][parentlink][registry]") {
    const sv::ReplicationMeta& first  = sv::ensureParentLinkRegistered();
    const sv::ReplicationMeta& second = sv::ensureParentLinkRegistered();
    // Same underlying registry record.
    REQUIRE(first.typeNameHash == second.typeNameHash);
    REQUIRE(first.schemaVersion == second.schemaVersion);
    REQUIRE(first.fields.size() == second.fields.size());
    // Calling the anchor twice must not corrupt the authority tag.
    REQUIRE(first.authority  == sv::Authority::Owner);
    REQUIRE(second.authority == sv::Authority::Owner);
}
