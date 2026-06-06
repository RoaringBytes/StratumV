// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── CameraComponent tests ─────────────────────────
// Pure logic — lives in sv_core_tests so it runs on both full Windows
// builds and the Linux core-only carve-out. Tagged [cameracomponent]
// with sub-tags per area. Mirrors test_LightComponent.cpp's structure
// exactly so a future drift in the registry / wire codecs lights up
// the same way for all three Editor-authority components.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "CameraComponent.h"
#include "EditTransaction.h"
#include "ReplicationRegistry.h"

#include <cstring>
#include <string>
#include <vector>

using Catch::Approx;

// ══════════════════════════════════════════════════════════════════
// CameraComponent registration + authority
// ══════════════════════════════════════════════════════════════════

TEST_CASE("CameraComponent: SV_REPLICATE registers 4 fields + Editor authority",
          "[cameracomponent][registry]") {
    (void)sv::ensureCameraComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("CameraComponent");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == sv::kCameraComponentFieldCount);
    REQUIRE(meta->fields.size() == 4);

    REQUIRE(std::string("fovDeg")    == meta->fields[0].name);
    REQUIRE(std::string("aspect")    == meta->fields[1].name);
    REQUIRE(std::string("nearPlane") == meta->fields[2].name);
    REQUIRE(std::string("farPlane")  == meta->fields[3].name);

    REQUIRE(meta->fields[0].type == sv::FieldType::Float);
    REQUIRE(meta->fields[1].type == sv::FieldType::Float);
    REQUIRE(meta->fields[2].type == sv::FieldType::Float);
    REQUIRE(meta->fields[3].type == sv::FieldType::Float);

    // Editor authority — same class as LightComponent.
    REQUIRE(meta->authority == sv::Authority::Editor);
}

TEST_CASE("CameraComponent: ensureCameraComponentRegistered is idempotent",
          "[cameracomponent][registry]") {
    const sv::ReplicationMeta& first  = sv::ensureCameraComponentRegistered();
    const sv::ReplicationMeta& second = sv::ensureCameraComponentRegistered();
    REQUIRE(first.typeNameHash  == second.typeNameHash);
    REQUIRE(first.schemaVersion == second.schemaVersion);
    REQUIRE(first.fields.size() == second.fields.size());
    // The anchor must re-apply setAuthority on every call so the
    // second invocation still sees Editor — same regression guard
    // LightComponent ships.
    REQUIRE(first.authority  == sv::Authority::Editor);
    REQUIRE(second.authority == sv::Authority::Editor);
}

// ══════════════════════════════════════════════════════════════════
// Wire: generic SetField payload round-trip
// ══════════════════════════════════════════════════════════════════

TEST_CASE("CameraComponent: full-mask SetField round-trip via generic path",
          "[cameracomponent][wire]") {
    (void)sv::ensureCameraComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("CameraComponent");
    REQUIRE(meta != nullptr);

    sv::CameraComponent src;
    src.fovDeg    = 75.0f;
    src.aspect    = 16.0f / 9.0f;
    src.nearPlane = 0.25f;
    src.farPlane  = 1500.0f;

    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, payload));

    sv::CameraComponent dst;
    dst.fovDeg    = -99.0f;
    dst.aspect    = -99.0f;
    dst.nearPlane = -99.0f;
    dst.farPlane  = -99.0f;

    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                            payload.data(),
                                            payload.size(),
                                            &dst,
                                            decodedMask));
    REQUIRE(dst.fovDeg    == Approx(75.0f));
    REQUIRE(dst.aspect    == Approx(16.0f / 9.0f));
    REQUIRE(dst.nearPlane == Approx(0.25f));
    REQUIRE(dst.farPlane  == Approx(1500.0f));
    for (size_t i = 0; i < meta->fields.size(); ++i) {
        REQUIRE(decodedMask.test(i));
    }
}

TEST_CASE("CameraComponent: partial-mask preserves untouched fields",
          "[cameracomponent][wire]") {
    (void)sv::ensureCameraComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("CameraComponent");
    REQUIRE(meta != nullptr);

    // Only mark fovDeg dirty — aspect/near/far must survive on the
    // receiver end.
    sv::CameraComponent src;
    src.fovDeg = 90.0f;

    sv::DirtyMask mask(meta->fields.size());
    mask.set(0);   // fovDeg

    std::vector<uint8_t> payload;
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, mask, payload));

    sv::CameraComponent dst;
    dst.fovDeg    = 60.0f;
    dst.aspect    = 1.5f;
    dst.nearPlane = 0.5f;
    dst.farPlane  = 800.0f;

    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(meta->typeNameHash,
                                            payload.data(),
                                            payload.size(),
                                            &dst,
                                            decodedMask));

    REQUIRE(dst.fovDeg    == Approx(90.0f));
    REQUIRE(dst.aspect    == Approx(1.5f));
    REQUIRE(dst.nearPlane == Approx(0.5f));
    REQUIRE(dst.farPlane  == Approx(800.0f));
    REQUIRE(decodedMask.test(0));
    REQUIRE_FALSE(decodedMask.test(1));
    REQUIRE_FALSE(decodedMask.test(2));
    REQUIRE_FALSE(decodedMask.test(3));
}

// ══════════════════════════════════════════════════════════════════
// EditTransaction end-to-end
// ══════════════════════════════════════════════════════════════════

TEST_CASE("CameraComponent: EditTransaction end-to-end with encode/parse",
          "[cameracomponent][roundtrip]") {
    (void)sv::ensureCameraComponentRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("CameraComponent");
    REQUIRE(meta != nullptr);

    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::SetField;
    tx.txId           = 808u;
    tx.originClientId = 5u;
    tx.requiredScope  = sv::PermissionScope::Editor;
    tx.entityId       = 200u;
    tx.typeNameHash   = meta->typeNameHash;
    tx.timestampMs    = 4321u;

    sv::CameraComponent src;
    src.fovDeg    = 30.0f;
    src.aspect    = 0.0f;       // 0 = use window aspect
    src.nearPlane = 0.1f;
    src.farPlane  = 2000.0f;

    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();
    REQUIRE(sv::writeGenericSetFieldPayload(*meta, &src, fullMask, tx.payload));

    std::vector<uint8_t> wire;
    REQUIRE(sv::encodeEditTransaction(tx, wire));

    auto parsed = sv::parseEditTransaction(wire.data(), wire.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind         == sv::EditKind::SetField);
    REQUIRE(parsed->typeNameHash == meta->typeNameHash);
    REQUIRE(parsed->entityId     == 200u);

    sv::CameraComponent dst;
    sv::DirtyMask decodedMask(meta->fields.size());
    REQUIRE(sv::readGenericSetFieldPayload(parsed->typeNameHash,
                                            parsed->payload.data(),
                                            parsed->payload.size(),
                                            &dst,
                                            decodedMask));
    REQUIRE(dst.fovDeg    == Approx(30.0f));
    REQUIRE(dst.aspect    == Approx(0.0f));
    REQUIRE(dst.nearPlane == Approx(0.1f));
    REQUIRE(dst.farPlane  == Approx(2000.0f));
}

TEST_CASE("CameraComponent: default state has fovDeg=0 farPlane=0 (no override)",
          "[cameracomponent][defaults]") {
    // The lab harness gates the per-frame override pick on
    // `fovDeg > 0 && farPlane > nearPlane`. A default-constructed
    // sidecar (everything 0 except nearPlane=0.1) MUST not trip that
    // gate, otherwise every entity in the world would silently
    // override the local viewport.
    sv::CameraComponent c;
    REQUIRE(c.fovDeg    == Approx(0.0f));
    REQUIRE(c.aspect    == Approx(0.0f));
    REQUIRE(c.nearPlane == Approx(0.1f));
    REQUIRE(c.farPlane  == Approx(0.0f));
    REQUIRE_FALSE(c.farPlane > c.nearPlane);
}
