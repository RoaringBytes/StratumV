// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── PerformanceBudget + NetworkStats + PerformanceContext ───
// Focused unit tests for the new performance observability sub-struct
// added to BaseSystemContext during . The contract is:
//
//   1. Default-construction yields sensible "not yet measured" values
//      (all counters zero, budget = 60 fps / 16.67 ms / 10k draws /
//      5M tris / 8 GB VRAM) so the AdminPanel HUD can hide bars that
//      haven't been populated yet.
//   2. Budget fields can be overridden by the game without touching
//      any engine-populated counter.
//   3. Runtime counters (frame / draw / VRAM / network.*) can be
//      populated by the engine without touching the budget.
// 4. NetworkStats zeros are the placeholder state — each
// session (/ / / ) will fill in one
//      or more of the six fields listed in docs/NETWORK_DESIGN.md §7.
//   5. PerformanceBudget and NetworkStats are plain PODs (trivially
//      copyable) so they can ride the DLL boundary without surprises.
//
// These tests mirror the pattern used by test_MockContext.cpp for the
// nested sub-struct smoke tests but isolate PerformanceContext so a
// future refactor that moves NetworkStats out of perf and
// into a new NetworkContext sub-struct has a clear regression target.

#include "BaseSystemContext.h"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using sv::BaseSystemContext;
using sv::NetworkStats;
using sv::PerformanceBudget;
using sv::PerformanceContext;

// ── Layout guarantees ────────────────────────────────────────────────
// The budget + network stats travel across the DLL boundary as part
// of BaseSystemContext.perf, so they must be trivially copyable POD.
static_assert(std::is_trivially_copyable_v<PerformanceBudget>,
              "PerformanceBudget must be trivially copyable for the DLL boundary");
static_assert(std::is_trivially_copyable_v<NetworkStats>,
              "NetworkStats must be trivially copyable for the DLL boundary");
static_assert(std::is_trivially_copyable_v<PerformanceContext>,
              "PerformanceContext must be trivially copyable for the DLL boundary");
static_assert(std::is_default_constructible_v<PerformanceContext>,
              "PerformanceContext must be default-constructible");

// ── Tests ────────────────────────────────────────────────────────────

TEST_CASE("PerformanceBudget: default values are 60 fps / 16.67 ms / 10k draws / 5M tris / 8 GB",
          "[perf][budget]")
{
    PerformanceBudget b{};
    REQUIRE(b.targetFps    == 60.0f);
    REQUIRE(b.maxFrameMs   == 16.67f);
    REQUIRE(b.maxDrawCalls == 10000u);
    REQUIRE(b.maxTriangles == 5'000'000u);
    REQUIRE(b.maxGpuMemMB  == 8192u);
}

TEST_CASE("PerformanceBudget: game can override budget without affecting defaults of fresh copy",
          "[perf][budget]")
{
    PerformanceBudget a{};
    a.targetFps    = 144.0f;
    a.maxFrameMs   = 1000.0f / 144.0f;
    a.maxDrawCalls = 20000;
    a.maxTriangles = 10'000'000;
    a.maxGpuMemMB  = 16384;

    REQUIRE(a.targetFps    == 144.0f);
    REQUIRE(a.maxDrawCalls == 20000u);
    REQUIRE(a.maxTriangles == 10'000'000u);
    REQUIRE(a.maxGpuMemMB  == 16384u);

    // A second default-constructed budget must not have picked up
    // anything from the first. (Sanity for trivial copy semantics.)
    PerformanceBudget fresh{};
    REQUIRE(fresh.targetFps    == 60.0f);
    REQUIRE(fresh.maxDrawCalls == 10000u);
    REQUIRE(fresh.maxGpuMemMB  == 8192u);
}

TEST_CASE("NetworkStats: all six fields default to zero",
          "[perf][network]")
{
    NetworkStats n{};
    REQUIRE(n.tickMs                == 0.0f);
    REQUIRE(n.bytesPerSec           == 0u);
    REQUIRE(n.packetsPerSec         == 0u);
    REQUIRE(n.replicatedEntityCount == 0u);
    REQUIRE(n.ackLatencyMs          == 0.0f);
    REQUIRE(n.droppedDatagramPct    == 0.0f);
}

TEST_CASE("NetworkStats: all six fields can be populated independently (* wiring target)",
          "[perf][network]")
{
    NetworkStats n{};
    n.tickMs = 15.5f; // server loop
    n.bytesPerSec = 1'250'000; // transport callback
    n.packetsPerSec = 3600; //
    n.replicatedEntityCount = 512; // snapshot generator
    n.ackLatencyMs = 42.3f; // reliable ack stream
    n.droppedDatagramPct = 0.012f; // QUIC loss telemetry

    REQUIRE(n.tickMs                == 15.5f);
    REQUIRE(n.bytesPerSec           == 1'250'000u);
    REQUIRE(n.packetsPerSec         == 3600u);
    REQUIRE(n.replicatedEntityCount == 512u);
    REQUIRE(n.ackLatencyMs          == 42.3f);
    REQUIRE(n.droppedDatagramPct    == 0.012f);
}

TEST_CASE("PerformanceContext: default-constructed has zero counters + default budget + zero network",
          "[perf][context]")
{
    PerformanceContext pc{};

    // Budget defaults match PerformanceBudget{} contract
    REQUIRE(pc.budget.targetFps    == 60.0f);
    REQUIRE(pc.budget.maxDrawCalls == 10000u);

    // All runtime counters start at zero
    REQUIRE(pc.frameTimeMs    == 0.0f);
    REQUIRE(pc.cpuFrameTimeMs == 0.0f);
    REQUIRE(pc.gpuFrameTimeMs == 0.0f);
    REQUIRE(pc.avgFps         == 0.0f);
    REQUIRE(pc.drawCallCount  == 0u);
    REQUIRE(pc.triangleCount  == 0u);
    REQUIRE(pc.vramUsedMB     == 0.0f);
    REQUIRE(pc.vramBudgetMB   == 0.0f);

    // Nested NetworkStats is all zero
    REQUIRE(pc.network.tickMs                == 0.0f);
    REQUIRE(pc.network.bytesPerSec           == 0u);
    REQUIRE(pc.network.replicatedEntityCount == 0u);
}

TEST_CASE("PerformanceContext: realistic frame update populates counters without touching budget",
          "[perf][context]")
{
    PerformanceContext pc{};

    // Simulate one frame of engine population at 165 fps target
    pc.budget.targetFps  = 165.0f;
    pc.budget.maxFrameMs = 1000.0f / 165.0f;

    // A typical 1080p frame in the lab harness
    pc.frameTimeMs    = 5.8f;
    pc.cpuFrameTimeMs = 2.1f;
    pc.gpuFrameTimeMs = 3.4f;
    pc.avgFps         = 172.3f;
    pc.drawCallCount  = 14;      // CC5 has ~14 submeshes
    pc.triangleCount  = 83'000;  // rough CC5 poly count
    pc.vramUsedMB     = 412.5f;
    pc.vramBudgetMB   = 8192.0f;

    // Budget stayed where the game set it
    REQUIRE(pc.budget.targetFps    == 165.0f);
    REQUIRE(pc.budget.maxDrawCalls == 10000u); // default not touched
    REQUIRE(pc.budget.maxTriangles == 5'000'000u);

    // Frame counters reflect what the engine wrote
    REQUIRE(pc.frameTimeMs    == 5.8f);
    REQUIRE(pc.cpuFrameTimeMs == 2.1f);
    REQUIRE(pc.gpuFrameTimeMs == 3.4f);
    REQUIRE(pc.avgFps         == 172.3f);
    REQUIRE(pc.drawCallCount  == 14u);
    REQUIRE(pc.triangleCount  == 83'000u);

    // Runtime counters are under budget → HUD would draw green
    REQUIRE(pc.frameTimeMs    <  pc.budget.maxFrameMs);
    REQUIRE(pc.drawCallCount  <  pc.budget.maxDrawCalls);
    REQUIRE(pc.triangleCount  <  pc.budget.maxTriangles);
    REQUIRE(pc.vramUsedMB     <  pc.budget.maxGpuMemMB);

    // Network fields are still placeholders — zero
    REQUIRE(pc.network.tickMs      == 0.0f);
    REQUIRE(pc.network.bytesPerSec == 0u);
}

TEST_CASE("PerformanceContext: over-budget frame is detectable via budget comparison",
          "[perf][context][budget]")
{
    PerformanceContext pc{};
    pc.budget.targetFps    = 60.0f;
    pc.budget.maxFrameMs   = 16.67f;
    pc.budget.maxDrawCalls = 1000;

    // Simulate a 30 fps frame with 1500 draws → over-budget
    pc.frameTimeMs   = 33.3f;
    pc.drawCallCount = 1500;

    // Both over budget → HUD would draw red
    REQUIRE(pc.frameTimeMs   >  pc.budget.maxFrameMs);
    REQUIRE(pc.drawCallCount >  pc.budget.maxDrawCalls);

    // VRAM and triangle counts untouched, so they stay under their
    // respective default budgets (safe-by-default)
    REQUIRE(pc.triangleCount <  pc.budget.maxTriangles);
    REQUIRE(pc.vramUsedMB    <  static_cast<float>(pc.budget.maxGpuMemMB));
}

TEST_CASE("BaseSystemContext.perf is independently writable via nested sub-struct path",
          "[perf][context][mock]")
{
    BaseSystemContext ctx{};

    // Simulate the lab harness populating a frame of perf data
    ctx.perf.frameTimeMs              = 6.1f;
    ctx.perf.cpuFrameTimeMs           = 2.4f;
    ctx.perf.gpuFrameTimeMs           = 3.6f;
    ctx.perf.avgFps                   = 163.9f;
    ctx.perf.drawCallCount            = 14;
    ctx.perf.triangleCount            = 83'000;
    ctx.perf.vramUsedMB               = 412.0f;
    ctx.perf.vramBudgetMB             = 8192.0f;

    // Simulate a replication session writing network stats
    ctx.perf.network.tickMs                = 16.7f;
    ctx.perf.network.bytesPerSec           = 524288;   // 0.5 MB/s
    ctx.perf.network.packetsPerSec         = 1800;
    ctx.perf.network.replicatedEntityCount = 128;
    ctx.perf.network.ackLatencyMs          = 37.5f;
    ctx.perf.network.droppedDatagramPct    = 0.0025f;

    // Round-trip read
    REQUIRE(ctx.perf.frameTimeMs               == 6.1f);
    REQUIRE(ctx.perf.drawCallCount             == 14u);
    REQUIRE(ctx.perf.triangleCount             == 83'000u);
    REQUIRE(ctx.perf.vramUsedMB                == 412.0f);
    REQUIRE(ctx.perf.network.bytesPerSec       == 524288u);
    REQUIRE(ctx.perf.network.replicatedEntityCount == 128u);
    REQUIRE(ctx.perf.network.ackLatencyMs      == 37.5f);

    // Completely unrelated sub-structs are unaffected (cross-check)
    REQUIRE(ctx.rendering.renderWidth == 0u);
    REQUIRE(!ctx.audio.postAudioEvent);      // std::function still empty
    REQUIRE(ctx.world.worldBounds == nullptr);
    REQUIRE(!ctx.input.isKeyDown);           // std::function still empty
}
