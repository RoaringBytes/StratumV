// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── LightComponent tests ──────────────────────────
// Pure logic — lives in sv_core_tests so it runs on both full Windows
// builds and the Linux core-only carve-out. Tagged [lightcomponent]
// with sub-tags per area. The existing [edit][parentlink] cases in
// test_EditTransaction.cpp are the template this file follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "EditTransaction.h"
#include "LightComponent.h"
#include "ReplicationRegistry.h"

#include <cstring>
#include <string>
#include <vector>

using Catch::Approx;

// ══════════════════════════════════════════════════════════════════
// LightComponent registration + authority
// ══════════════════════════════════════════════════════════════════

TEST_CASE("LightComponent: SV_REPLICATE registers 8 fields + Editor authority",
          "[lightcomponent][registry]") {
    (void)sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == sv::kLightComponentFieldCount);
    REQUIRE(meta->fields.size() == 8);

    // Field names + types in declaration order.
    REQUIRE(std::string("type")         == meta->fields[0].name);
    REQUIRE(std::string("colorR")       == meta->fields[1].name);
    REQUIRE(std::string("colorG")       == meta->fields[2].name);
    REQUIRE(std::string("colorB")       == meta->fields[3].name);
    REQUIRE(std::string("intensity")    == meta->fields[4].name);
    REQUIRE(std::string("range")        == meta->fields[5].name);
    REQUIRE(std::string("coneInnerDeg") == meta->fields[6].name);
    REQUIRE(std::string("coneOuterDeg") == meta->fields[7].name);

    REQUIRE(meta->fields[0].type == sv::FieldType::UInt32);
    REQUIRE(meta->fields[1].type == sv::FieldType::Float);
    REQUIRE(meta->fields[2].type == sv::FieldType::Float);
    REQUIRE(meta->fields[3].type == sv::FieldType::Float);
    REQUIRE(meta->fields[4].type == sv::FieldType::Float);
    REQUIRE(meta->fields[5].type == sv::FieldType::Float);
    REQUIRE(meta->fields[6].type == sv::FieldType::Float);
    REQUIRE(meta->fields[7].type == sv::FieldType::Float);

    // First shipped component that actually exercises Authority::Editor.
    REQUIRE(meta->authority == sv::Authority::Editor);
}

TEST_CASE("LightComponent: ensureLightComponentRegistered is idempotent",
          "[lightcomponent][registry]") {
    const sv::ReplicationMeta& first  = sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta& second = sv::ensureLightComponentRegistered();
    REQUIRE(first.typeNameHash  == second.typeNameHash);
    REQUIRE(first.schemaVersion == second.schemaVersion);
    REQUIRE(first.fields.size() == second.fields.size());
    // Critical: the anchor must re-apply setAuthority on every call so
    // the second invocation still sees Editor rather than falling back
    // to the default Server tag (ReplicationRegistry::registerType
    // replaces the stored meta on every re-registration).
    REQUIRE(first.authority  == sv::Authority::Editor);
    REQUIRE(second.authority == sv::Authority::Editor);
}

// ══════════════════════════════════════════════════════════════════
// Wire: generic SetField payload round-trip
// ══════════════════════════════════════════════════════════════════

TEST_CASE("LightComponent: full-mask SetField round-trip via generic path",
          "[lightcomponent][wire]") {
    (void)sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    REQUIRE(meta != nullptr);

    sv::LightComponent src;
    src.type         = 2u;           // Point
    src.colorR       = 0.8f;
    src.colorG       = 0.9f;
    src.colorB       = 1.0f;
    src.intensity    = 2.5f;
    src.range        = 17.5f;
    src.coneInnerDeg = 22.0f;
    src.coneOuterDeg = 44.0f;

    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, payload));

    // Pre-populate dst with garbage to prove every field is overwritten.
    sv::LightComponent dst;
    dst.type         = 0xBAADu;
    dst.colorR       = -99.0f;
    dst.colorG       = -99.0f;
    dst.colorB       = -99.0f;
    dst.intensity    = -99.0f;
    dst.range        = -99.0f;
    dst.coneInnerDeg = -99.0f;
    dst.coneOuterDeg = -99.0f;

    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                           payload.data(),
                                           payload.size(),
                                           &dst,
                                           decodedMask));
    REQUIRE(dst.type         == 2u);
    REQUIRE(dst.colorR       == Approx(0.8f));
    REQUIRE(dst.colorG       == Approx(0.9f));
    REQUIRE(dst.colorB       == Approx(1.0f));
    REQUIRE(dst.intensity    == Approx(2.5f));
    REQUIRE(dst.range        == Approx(17.5f));
    REQUIRE(dst.coneInnerDeg == Approx(22.0f));
    REQUIRE(dst.coneOuterDeg == Approx(44.0f));
    for (size_t i = 0; i < meta->fields.size(); ++i) {
        REQUIRE(decodedMask.test(i));
    }
}

TEST_CASE("LightComponent: partial-mask preserves untouched fields",
          "[lightcomponent][wire]") {
    (void)sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    REQUIRE(meta != nullptr);

    sv::LightComponent src;
    src.type      = 3u;             // Spot
    src.intensity = 4.0f;

    // Mark only type + intensity dirty — other fields must survive
    // on the receiver end.
    sv::DirtyMask mask(meta->fields.size());
    mask.set(0);   // type
    mask.set(4);   // intensity

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, mask, payload));

    // Destination starts with a well-known non-default colour + range +
    // cone that the decoder must leave alone.
    sv::LightComponent dst;
    dst.type         = 1u;          // previous type
    dst.colorR       = 0.5f;
    dst.colorG       = 0.25f;
    dst.colorB       = 0.125f;
    dst.intensity    = 0.0f;
    dst.range        = 42.0f;
    dst.coneInnerDeg = 10.0f;
    dst.coneOuterDeg = 20.0f;

    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                           payload.data(),
                                           payload.size(),
                                           &dst,
                                           decodedMask));

    // Decoded: updated
    REQUIRE(dst.type      == 3u);
    REQUIRE(dst.intensity == Approx(4.0f));
    // Untouched: preserved
    REQUIRE(dst.colorR       == Approx(0.5f));
    REQUIRE(dst.colorG       == Approx(0.25f));
    REQUIRE(dst.colorB       == Approx(0.125f));
    REQUIRE(dst.range        == Approx(42.0f));
    REQUIRE(dst.coneInnerDeg == Approx(10.0f));
    REQUIRE(dst.coneOuterDeg == Approx(20.0f));

    REQUIRE(decodedMask.test(0));
    REQUIRE_FALSE(decodedMask.test(1));
    REQUIRE_FALSE(decodedMask.test(2));
    REQUIRE_FALSE(decodedMask.test(3));
    REQUIRE(decodedMask.test(4));
    REQUIRE_FALSE(decodedMask.test(5));
    REQUIRE_FALSE(decodedMask.test(6));
    REQUIRE_FALSE(decodedMask.test(7));
}

TEST_CASE("LightComponent: zero-mask SetField is a no-op on the receiver",
          "[lightcomponent][wire]") {
    (void)sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    REQUIRE(meta != nullptr);

    sv::LightComponent src;
    src.type      = 99u;              // bogus — should never reach dst
    src.intensity = 777.0f;

    sv::DirtyMask noMask(meta->fields.size());   // all zero

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, noMask, payload));

    sv::LightComponent dst;      // default-init
    const sv::LightComponent expected;   // same default-init snapshot

    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                           payload.data(),
                                           payload.size(),
                                           &dst,
                                           decodedMask));

    REQUIRE(dst.type         == expected.type);
    REQUIRE(dst.colorR       == Approx(expected.colorR));
    REQUIRE(dst.colorG       == Approx(expected.colorG));
    REQUIRE(dst.colorB       == Approx(expected.colorB));
    REQUIRE(dst.intensity    == Approx(expected.intensity));
    REQUIRE(dst.range        == Approx(expected.range));
    REQUIRE(dst.coneInnerDeg == Approx(expected.coneInnerDeg));
    REQUIRE(dst.coneOuterDeg == Approx(expected.coneOuterDeg));

    for (size_t i = 0; i < meta->fields.size(); ++i) {
        REQUIRE_FALSE(decodedMask.test(i));
    }
}

// ══════════════════════════════════════════════════════════════════
// EditTransaction end-to-end
// ══════════════════════════════════════════════════════════════════

TEST_CASE("LightComponent: EditTransaction end-to-end with encode/parse",
          "[lightcomponent][roundtrip]") {
    (void)sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    REQUIRE(meta != nullptr);

    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::SetField;
    tx.txId           = 404u;
    tx.originClientId = 7u;
    tx.requiredScope  = sv::PermissionScope::Editor;
    tx.entityId       = 100u;
    tx.typeNameHash   = meta->typeNameHash;
    tx.timestampMs    = 1234u;

    sv::LightComponent src;
    src.type      = 2u;            // Point
    src.colorR    = 1.0f;
    src.colorG    = 0.9f;
    src.colorB    = 0.7f;
    src.intensity = 3.0f;
    src.range     = 25.0f;

    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, tx.payload));

    std::vector<uint8_t> wire;
    REQUIRE(sv::encodeEditTransaction(tx, wire));

    auto parsed = sv::parseEditTransaction(wire.data(), wire.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind         == sv::EditKind::SetField);
    REQUIRE(parsed->typeNameHash == meta->typeNameHash);
    REQUIRE(parsed->entityId     == 100u);

    sv::LightComponent dst;
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(parsed->typeNameHash,
                                           parsed->payload.data(),
                                           parsed->payload.size(),
                                           &dst,
                                           decodedMask));
    REQUIRE(dst.type      == 2u);
    REQUIRE(dst.colorR    == Approx(1.0f));
    REQUIRE(dst.colorG    == Approx(0.9f));
    REQUIRE(dst.colorB    == Approx(0.7f));
    REQUIRE(dst.intensity == Approx(3.0f));
    REQUIRE(dst.range     == Approx(25.0f));
}

TEST_CASE("LightComponent: readGeneric with wrong typeNameHash is rejected",
          "[lightcomponent][error]") {
    (void)sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    REQUIRE(meta != nullptr);

    sv::LightComponent src;
    src.type      = 1u;
    src.intensity = 5.0f;
    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, payload));

    sv::LightComponent dst;
    sv::DirtyMask decodedMask(meta->fields.size());
    // Flip the hash to a bogus value — the dispatch must refuse.
    REQUIRE_FALSE(sv::readGenericSetFieldPayload(meta->typeNameHash ^ 0xFFFFFFFFu,
                                                  payload.data(),
                                                  payload.size(),
                                                  &dst,
                                                  decodedMask));
}

TEST_CASE("LightComponent: default state has zero intensity and type=0",
          "[lightcomponent][defaults]") {
    // Every entity in the server's ReplicatedEntity map gains a default
    // LightComponent sidecar; the invariant is that a default sidecar
    // contributes nothing to the shader (type 0 means "disabled",
    // intensity 0 means "no energy"). A regression here would mean
    // every entity in the world silently emits light after the S-
    // BLENDER-LINK2b bump, which would be very visible in the render.
    sv::LightComponent lc;
    REQUIRE(lc.type      == 0u);
    REQUIRE(lc.intensity == Approx(0.0f));
    // Color default is white so an Editor that flips type+intensity
    // without touching colour gets a visible light immediately.
    REQUIRE(lc.colorR == Approx(1.0f));
    REQUIRE(lc.colorG == Approx(1.0f));
    REQUIRE(lc.colorB == Approx(1.0f));
    // Range + cone defaults are non-zero so point/spot lights have a
    // reasonable starting radius.
    REQUIRE(lc.range        == Approx(10.0f));
    REQUIRE(lc.coneInnerDeg == Approx(30.0f));
    REQUIRE(lc.coneOuterDeg == Approx(45.0f));
}
