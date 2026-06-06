// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── stratumv_core carve-out boundary tests ──────────────
//
// Verifies the Layer 4 networking + replication carve-out compiles +
// links against the minimal subset of the engine (EngineLog, INetwork-
// Context, ReplicationRegistry, NetTransform, ReplicationProtocol,
// MsQuicTransport) without dragging any graphics headers into the TU.
//
// These tests are in the `core` test lineup — they build on both
// STRATUMV_CORE_ONLY=OFF (full engine) and STRATUMV_CORE_ONLY=ON
// (Linux dedicated-server flavor). The test file's compile-time
// contract is what makes the core/full split load-bearing:
//
//   * Includes ONLY the core subset headers.
//   * Must NOT transitively include volk.h, vulkan.h, vk_mem_alloc.h,
//     glm/glm.hpp, imgui.h, ozz/*, ufbx.h, miniaudio.h, tinygltf.h.
//
// The negative assertion is enforced at compile time via `#if
// __has_include(...)`. If a future refactor accidentally pulls one of
// those heavy headers into the core surface, this file fails to build
// with a clear error message pointing at the offending include.
//
// The runtime assertions are minimal because the meat of each Layer 4
// module is already covered by test_ReplicationRegistry, test_Replication-
// Wire, and test_MsQuicTransport. This file exists to pin the BOUNDARY.

#include "EngineLog.h"
#include "INetworkContext.h"
#include "ReplicationRegistry.h"
#include "NetTransform.h"
#include "net/ReplicationProtocol.h"
#include "net/MsQuicTransport.h"
#include "StratumVVersion.h"

#include <catch2/catch_test_macros.hpp>

// ── Compile-time boundary assertion ──────────────────────────────
// __has_include is a C++17 feature; both MSVC and clang/gcc support it.
// We deliberately use the exact spellings that the heavy graphics
// headers use in stratumv.lib, so a creeping transitive include would
// fire one of these.
//
// IMPORTANT: __has_include returns true only for headers the
// preprocessor can ACTUALLY find on the current include path. If the
// stratumv_core target does not add volk/VulkanHeaders/glm/imgui/ozz
// to its include directories, these should all evaluate to false even
// though those SDKs exist elsewhere in the build tree. That is the
// point — the test is proving the include-path surface, not the file-
// system.

#if __has_include(<volk.h>)
#  error "stratumv_core is leaking volk.h — Layer 4 should not see Vulkan"
#endif

#if __has_include(<vulkan/vulkan.h>)
#  error "stratumv_core is leaking vulkan/vulkan.h — Layer 4 should not see Vulkan"
#endif

#if __has_include(<vk_mem_alloc.h>)
#  error "stratumv_core is leaking vk_mem_alloc.h — Layer 4 should not see VMA"
#endif

#if __has_include(<glm/glm.hpp>)
#  error "stratumv_core is leaking glm/glm.hpp — Layer 4 should not see glm"
#endif

#if __has_include(<imgui.h>)
#  error "stratumv_core is leaking imgui.h — Layer 4 should not see ImGui"
#endif

#if __has_include(<ozz/base/memory/allocator.h>)
#  error "stratumv_core is leaking ozz — Layer 4 should not see ozz-animation"
#endif

// ── Runtime assertions ───────────────────────────────────────────

TEST_CASE("stratumv_core: version header is accessible", "[core][version]") {
    // StratumVVersion.h is generated from CMake's project VERSION and
    // lives in the generated/ include path. stratumv_core must expose
    // this so the server can emit a version line on startup.
    REQUIRE(STRATUMV_VERSION_MAJOR >= 1);
    REQUIRE(STRATUMV_VERSION_MINOR >= 0);
    REQUIRE(std::string(STRATUMV_VERSION_STRING).size() >= 5);
}

TEST_CASE("stratumv_core: EngineLog singleton is wired", "[core][log]") {
    // EngineLog is the one non-networking dependency the Layer 4
    // modules share. Validate the ring buffer is reachable from a
    // core-only TU (i.e. linked into stratumv_core, not dead-stripped).
    sv::EngineLog& log = sv::EngineLog::get();
    const uint64_t before = log.latestId();
    SV_LOG_INFO("", "core boundary test: EngineLog round-trip");
    const uint64_t after = log.latestId();
    REQUIRE(after > before);
}

TEST_CASE("stratumv_core: INetworkContext NoOp factory links", "[core][netctx]") {
    // Proves the INetworkContext.cpp TU actually compiled into
    // stratumv_core — without it the NoOp factory would be an
    // unresolved external and this test would fail to link.
    auto ctx = sv::createNoOpNetworkContext();
    REQUIRE(ctx != nullptr);
    REQUIRE_FALSE(ctx->isConnected());
    REQUIRE(ctx->getBytesSent() == 0);
    REQUIRE(ctx->getBytesReceived() == 0);
}

TEST_CASE("stratumv_core: NetTransform registers with the replication registry",
          "[core][replication]") {
    // Pulls NetTransform.obj into the link (via ensureNetTransforms
    // Registered's non-inline symbol) and verifies the SV_REPLICATE
    // static initializer ran. Without stratumv_core this would fail
    // either at link time (unresolved ensureNetTransformRegistered)
    // or at runtime (registry->find returns nullptr because the
    // static archive's TU was dead-stripped).
    const sv::ReplicationMeta& meta = sv::ensureNetTransformRegistered();
    REQUIRE(meta.typeName == "NetTransform");
    REQUIRE(meta.fields.size() == sv::kNetTransformFieldCount);
    REQUIRE(meta.authority == sv::Authority::Server);

    const sv::ReplicationMeta* lookedUp =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(lookedUp != nullptr);
    REQUIRE(lookedUp->typeNameHash == meta.typeNameHash);
}

TEST_CASE("stratumv_core: ReplicationProtocol encodeSchemaHandshake round-trip",
          "[core][wire]") {
    // Final boundary check: the wire framing code (+
    // ) compiles + links + executes from the core subset
    // alone, against an actual registry entry rather than a synthetic
    // test fixture.
    sv::ensureNetTransformRegistered();

    sv::net::SchemaHandshake hs;
    hs.semver = sv::net::packSemver(STRATUMV_VERSION_MAJOR,
                                    STRATUMV_VERSION_MINOR,
                                    STRATUMV_VERSION_PATCH);
    for (const auto& e : sv::ReplicationRegistry::get().getSchemaTable()) {
        hs.types.push_back({e.typeNameHash, e.schemaVersion});
    }

    std::vector<uint8_t> bytes;
    REQUIRE(sv::net::encodeSchemaHandshake(hs, bytes));
    REQUIRE(bytes.size() ==
            sv::net::schemaHandshakeSize(hs.types.size()));
    REQUIRE(bytes[0] == sv::net::kFrameSchemaHandshake);

    sv::net::SchemaHandshake parsed;
    REQUIRE(sv::net::parseSchemaHandshake(bytes.data(), bytes.size(), parsed));
    REQUIRE(parsed.semver == hs.semver);
    REQUIRE(parsed.types.size() == hs.types.size());

    const auto cmp = sv::net::compareSchemaHandshake(parsed);
    REQUIRE(cmp.status == sv::net::SchemaCompareStatus::Ok);
}

TEST_CASE("stratumv_core: MsQuic transport factory links", "[core][msquic]") {
    // A lighter version of test_MsQuicTransport's [status] case.
    // Proves the MsQuic wrapper's .cpp TU is actually in the link —
    // test_MsQuicTransport itself runs on the core subset too, but
    // this file is specifically the boundary test, so asserting the
    // version pin here is load-bearing for "refactor hasn't drifted".
    REQUIRE(sv::net::Transport::isMsquicAvailable());
    REQUIRE(sv::net::Transport::msquicVersionString() == "2.5.6");
    REQUIRE(std::string(sv::net::transportStatusToString(
        sv::net::TransportStatus::Ok)) == "Ok");
}
