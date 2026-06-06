// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── MaterialComponent tests ───────────────────────
// Pure logic — lives in sv_core_tests so it runs on both full Windows
// builds and the Linux core-only carve-out. Tagged [materialcomponent]
// with sub-tags per area. Mirrors test_CameraComponent.cpp + the
// LightComponent template; the three Editor-authority components share
// the same shape so a single drift surface lights up all three.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "EditTransaction.h"
#include "MaterialComponent.h"
#include "ReplicationRegistry.h"

#include <cstring>
#include <string>
#include <vector>

using Catch::Approx;

// ══════════════════════════════════════════════════════════════════
// MaterialComponent registration + authority
// ══════════════════════════════════════════════════════════════════

TEST_CASE("MaterialComponent: SV_REPLICATE registers 4 fields + Editor authority",
          "[materialcomponent][registry]") {
    (void)sv::ensureMaterialComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("MaterialComponent");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == sv::kMaterialComponentFieldCount);
    REQUIRE(meta->fields.size() == 4);

    REQUIRE(std::string("baseColorR")       == meta->fields[0].name);
    REQUIRE(std::string("baseColorG")       == meta->fields[1].name);
    REQUIRE(std::string("baseColorB")       == meta->fields[2].name);
    REQUIRE(std::string("overrideStrength") == meta->fields[3].name);

    REQUIRE(meta->fields[0].type == sv::FieldType::Float);
    REQUIRE(meta->fields[1].type == sv::FieldType::Float);
    REQUIRE(meta->fields[2].type == sv::FieldType::Float);
    REQUIRE(meta->fields[3].type == sv::FieldType::Float);

    // Editor authority — same class as Light + Camera.
    REQUIRE(meta->authority == sv::Authority::Editor);
}

TEST_CASE("MaterialComponent: ensureMaterialComponentRegistered is idempotent",
          "[materialcomponent][registry]") {
    const sv::ReplicationMeta& first  = sv::ensureMaterialComponentRegistered();
    const sv::ReplicationMeta& second = sv::ensureMaterialComponentRegistered();
    REQUIRE(first.typeNameHash  == second.typeNameHash);
    REQUIRE(first.schemaVersion == second.schemaVersion);
    REQUIRE(first.fields.size() == second.fields.size());
    REQUIRE(first.authority  == sv::Authority::Editor);
    REQUIRE(second.authority == sv::Authority::Editor);
}

// ══════════════════════════════════════════════════════════════════
// Wire: generic SetField payload round-trip
// ══════════════════════════════════════════════════════════════════

TEST_CASE("MaterialComponent: full-mask SetField round-trip via generic path",
          "[materialcomponent][wire]") {
    (void)sv::ensureMaterialComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("MaterialComponent");
    REQUIRE(meta != nullptr);

    sv::MaterialComponent src;
    src.baseColorR       = 0.2f;
    src.baseColorG       = 0.7f;
    src.baseColorB       = 0.9f;
    src.overrideStrength = 0.85f;

    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, payload));

    sv::MaterialComponent dst;
    dst.baseColorR       = -99.0f;
    dst.baseColorG       = -99.0f;
    dst.baseColorB       = -99.0f;
    dst.overrideStrength = -99.0f;

    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                            payload.data(),
                                            payload.size(),
                                            &dst,
                                            decodedMask));
    REQUIRE(dst.baseColorR       == Approx(0.2f));
    REQUIRE(dst.baseColorG       == Approx(0.7f));
    REQUIRE(dst.baseColorB       == Approx(0.9f));
    REQUIRE(dst.overrideStrength == Approx(0.85f));
    for (size_t i = 0; i < meta->fields.size(); ++i) {
        REQUIRE(decodedMask.test(i));
    }
}

TEST_CASE("MaterialComponent: partial-mask preserves untouched fields",
          "[materialcomponent][wire]") {
    (void)sv::ensureMaterialComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("MaterialComponent");
    REQUIRE(meta != nullptr);

    // Only mark overrideStrength dirty — colors must survive.
    sv::MaterialComponent src;
    src.overrideStrength = 0.5f;

    sv::DirtyMask mask(meta->fields.size());
    mask.set(3);   // overrideStrength

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, mask, payload));

    sv::MaterialComponent dst;
    dst.baseColorR       = 0.4f;
    dst.baseColorG       = 0.5f;
    dst.baseColorB       = 0.6f;
    dst.overrideStrength = 0.0f;

    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                            payload.data(),
                                            payload.size(),
                                            &dst,
                                            decodedMask));

    REQUIRE(dst.baseColorR       == Approx(0.4f));
    REQUIRE(dst.baseColorG       == Approx(0.5f));
    REQUIRE(dst.baseColorB       == Approx(0.6f));
    REQUIRE(dst.overrideStrength == Approx(0.5f));
    REQUIRE_FALSE(decodedMask.test(0));
    REQUIRE_FALSE(decodedMask.test(1));
    REQUIRE_FALSE(decodedMask.test(2));
    REQUIRE(decodedMask.test(3));
}

// ══════════════════════════════════════════════════════════════════
// EditTransaction end-to-end
// ══════════════════════════════════════════════════════════════════

TEST_CASE("MaterialComponent: EditTransaction end-to-end with encode/parse",
          "[materialcomponent][roundtrip]") {
    (void)sv::ensureMaterialComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("MaterialComponent");
    REQUIRE(meta != nullptr);

    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::SetField;
    tx.txId           = 909u;
    tx.originClientId = 11u;
    tx.requiredScope  = sv::PermissionScope::Editor;
    tx.entityId       = 300u;
    tx.typeNameHash   = meta->typeNameHash;
    tx.timestampMs    = 5678u;

    sv::MaterialComponent src;
    src.baseColorR       = 0.95f;
    src.baseColorG       = 0.10f;
    src.baseColorB       = 0.20f;
    src.overrideStrength = 1.0f;

    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, tx.payload));

    std::vector<uint8_t> wire;
    REQUIRE(sv::encodeEditTransaction(tx, wire));

    auto parsed = sv::parseEditTransaction(wire.data(), wire.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind         == sv::EditKind::SetField);
    REQUIRE(parsed->typeNameHash == meta->typeNameHash);
    REQUIRE(parsed->entityId     == 300u);

    sv::MaterialComponent dst;
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(parsed->typeNameHash,
                                            parsed->payload.data(),
                                            parsed->payload.size(),
                                            &dst,
                                            decodedMask));
    REQUIRE(dst.baseColorR       == Approx(0.95f));
    REQUIRE(dst.baseColorG       == Approx(0.10f));
    REQUIRE(dst.baseColorB       == Approx(0.20f));
    REQUIRE(dst.overrideStrength == Approx(1.0f));
}

TEST_CASE("MaterialComponent: default state has white base color and zero strength",
          "[materialcomponent][defaults]") {
    // The lab harness gates the per-frame override pick on
    // `overrideStrength > 0`. A default-constructed sidecar MUST
    // have strength 0 so every entity in the world doesn't silently
    // tint the local mesh after the bump. The
    // default color is white so even at full strength, white tint ×
    // original = original (a "strength only" push has no visible
    // effect).
    sv::MaterialComponent m;
    REQUIRE(m.baseColorR       == Approx(1.0f));
    REQUIRE(m.baseColorG       == Approx(1.0f));
    REQUIRE(m.baseColorB       == Approx(1.0f));
    REQUIRE(m.overrideStrength == Approx(0.0f));
}
