// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── / / Mock BaseSystemContext pattern ──
// Documents + tests the recommended pattern for mocking BaseSystemContext
// in future DLL-plugin unit tests.
//
// BaseSystemContext holds its leaves in a flat hot core plus eleven
// nested POD sub-structs (as of 1.3.0): rendering, ui, input,
// buffers, vkfn, meshRegistry, audio, animation, world, perf, network.
// Most leaves are raw pointers or std::function callbacks. The mock
// pattern is:
//   1. Default-construct the struct (zero-inits POD fields,
//      null-inits function/pointer members).
//   2. Assign recording lambdas to only the callbacks the unit under test
//      actually calls, using the nested paths (e.g. ctx.input.isKeyDown,
//      ctx.buffers.createBuffer). Leave everything else untouched.
//   3. After the test, assert call counts / captured arguments.
//
// This file tests the pattern on a handful of easy fields rather than
// building a full mock. Future plugin tests can copy this scaffolding.

#include "BaseSystemContext.h"
#include "Config.h"           // sv::WorldBounds (complete type needed by refactor smoke test)
#include "INetworkContext.h" // createNoOpNetworkContext for default-init check

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

#include <string>
#include <type_traits>
#include <vector>

using sv::BaseSystemContext;

// ── Layout guarantees (; ; ; ) ─
// pinned the baseline at 3376 bytes. added the
// PerformanceContext sub-struct (budget + frame/draw/VRAM counters + 6
// NetworkStats placeholders) and relaxed the ceiling to 3504.
// promoted the flat `INetworkContext* network` into a NetworkContext
// sub-struct; ceiling moved to 3520 with a small cushion.
// added a second slot — `net::Transport* transport` — to NetworkContext
// so plugins can reach the datagram send/recv API without the legacy
// INetworkContext façade. The one-pointer growth plus any padding
// bumps the ceiling to 3536 (16-byte cushion retained for future
// single-pointer slots inside NetworkContext).
//
// The pre-refactor struct was NOT trivially copyable either
// (std::function has a non-trivial copy ctor), so the DLL boundary
// guarantee is public inheritance + base-at-offset-0, enforced by
// the scaffold-generated
//   struct SystemContext : BaseSystemContext { ... };
// convention — not something C++ type traits can encode directly.
// See DLLLoader::loadSystems reinterpret_cast note for the ABI
// assumption.
static_assert(std::is_default_constructible_v<BaseSystemContext>,
              "BaseSystemContext must be default-constructible for plugin scaffolding");
static_assert(sizeof(BaseSystemContext) <= 3536,
              "BaseSystemContext sizeof must stay within the ceiling (3536)");

// A small "unit under test" that pretends to be DLL plugin code:
// it reads input state and creates a GPU buffer through the context.
namespace {

struct FakePlugin {
    int framesWithKeyDown = 0;
    int buffersCreated    = 0;

    void tick(BaseSystemContext& ctx) {
        if (ctx.input.isKeyDown && ctx.input.isKeyDown(/*key=*/32)) // space
            framesWithKeyDown++;

        if (ctx.buffers.createBuffer) {
            auto buf = ctx.buffers.createBuffer(1024, 0, true);
            (void)buf;
            buffersCreated++;
        }
    }
};

} // anonymous

TEST_CASE("BaseSystemContext: default-constructed context has null callbacks", "[mock]") {
    BaseSystemContext ctx{};
    // std::function default-constructs to "empty" (evaluates false)
    REQUIRE(!ctx.input.isKeyDown);
    REQUIRE(!ctx.input.isKeyPressed);
    REQUIRE(!ctx.buffers.createBuffer);
}

TEST_CASE("BaseSystemContext: mock pattern records calls via capture", "[mock]") {
    BaseSystemContext ctx{};

    int keyDownCalls = 0;
    ctx.input.isKeyDown = [&keyDownCalls](int key) {
        keyDownCalls++;
        return key == 32; // simulate space held
    };

    int bufferCalls = 0;
    ctx.buffers.createBuffer = [&bufferCalls](VkDeviceSize, VkBufferUsageFlags, bool) {
        bufferCalls++;
        return sv::VkBuf{}; // return a blank handle — we don't touch it
    };

    FakePlugin plugin;
    for (int i = 0; i < 5; ++i) plugin.tick(ctx);

    REQUIRE(keyDownCalls == 5);
    REQUIRE(bufferCalls == 5);
    REQUIRE(plugin.framesWithKeyDown == 5); // mock says space is held every frame
    REQUIRE(plugin.buffersCreated == 5);
}

TEST_CASE("BaseSystemContext: selective callback leaves others null", "[mock]") {
    BaseSystemContext ctx{};
    ctx.input.isKeyDown = [](int) { return false; };

    // Only input.isKeyDown was set
    REQUIRE(ctx.input.isKeyDown);
    REQUIRE(!ctx.input.isKeyPressed);
    REQUIRE(!ctx.buffers.createBuffer);

    FakePlugin plugin;
    plugin.tick(ctx);

    REQUIRE(plugin.framesWithKeyDown == 0); // lambda always returns false
    REQUIRE(plugin.buffersCreated == 0);    // no buffer callback → skipped
}

// ── sub-struct layout smoke tests ────────────────────
// Exercise each sub-struct so a future field move away from its
// expected sub-struct breaks compilation rather than silently drifts.

TEST_CASE("BaseSystemContext: sizeof probe (informational)", "[mock][refactor][size]") {
    std::fprintf(stderr,
                 "\n sizeof(BaseSystemContext) = %zu bytes "
                 "(baseline 3376, 3464, 3520, ceiling 3536)\n",
                 sizeof(BaseSystemContext));
    REQUIRE(sizeof(BaseSystemContext) > 0);
}

TEST_CASE("BaseSystemContext: nested sub-structs default-initialise cleanly", "[mock][refactor]") {
    BaseSystemContext ctx{};

    // Flat core (the old `INetworkContext* network` slot
    // moved into the `network` sub-struct — flat core now holds
    // only engineLog + physics as non-ECS non-flag services).
    REQUIRE(ctx.device == VK_NULL_HANDLE);
    REQUIRE(ctx.ecs == nullptr);
    REQUIRE(ctx.windowWidth == 0);
    REQUIRE(ctx.wireframeEnabled == nullptr);
    REQUIRE(ctx.engineLog == nullptr);
    REQUIRE(ctx.physics == nullptr);

    // rendering (format + resolution + GPU features + quality + HZB)
    REQUIRE(ctx.rendering.hdrFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
    REQUIRE(ctx.rendering.renderWidth == 0);
    REQUIRE(ctx.rendering.currentFrame == 0);
    REQUIRE(ctx.rendering.dlssAvailable == false);
    REQUIRE(ctx.rendering.fpsCapTarget == nullptr);
    REQUIRE(ctx.rendering.postProcMutable == nullptr);
    REQUIRE(!ctx.rendering.getGraphicsPipeline);
    REQUIRE(!ctx.rendering.registerRenderPass);
    REQUIRE(ctx.rendering.hzbImageView == VK_NULL_HANDLE);
    REQUIRE(!ctx.rendering.captureScreenshot);

    // ui (ImGui + UI pipeline layouts)
    REQUIRE(ctx.ui.imguiContext == nullptr);
    REQUIRE(ctx.ui.uiStyleVersion == 0);
    REQUIRE(ctx.ui.uiBgPipeLayout == VK_NULL_HANDLE);
    REQUIRE(!ctx.ui.saveStyleSection);
    REQUIRE(ctx.ui.uiShadersEnabled == nullptr);

    // input (keyboard + mouse + actions + gamepad)
    REQUIRE(!ctx.input.isKeyPressed);
    REQUIRE(!ctx.input.getMousePos);
    REQUIRE(!ctx.input.toggleFullscreen);
    REQUIRE(!ctx.input.isActionDown);
    REQUIRE(!ctx.input.isGamepadConnected);

    // buffers (VMA wrappers + shader module)
    REQUIRE(!ctx.buffers.createBuffer);
    REQUIRE(!ctx.buffers.createShaderModule);
    REQUIRE(!ctx.buffers.createImage);

    // vkfn (PFN_vk*)
    REQUIRE(ctx.vkfn.fnCmdBindPipeline == nullptr);
    REQUIRE(ctx.vkfn.fnCmdDispatch == nullptr);
    REQUIRE(ctx.vkfn.fnDestroyShaderModule == nullptr);

    // meshRegistry (multi-mesh indirect SSBO)
    REQUIRE(!ctx.meshRegistry.registerMesh);
    REQUIRE(ctx.meshRegistry.meshSlotSSBO == VK_NULL_HANDLE);
    REQUIRE(ctx.meshRegistry.registeredMeshCount == 0);

    // audio (event-driven)
    REQUIRE(!ctx.audio.postAudioEvent);
    REQUIRE(!ctx.audio.setAudioVolume);

    // animation (ozz runtime + skinned pipeline)
    REQUIRE(ctx.animation.animationSystem == nullptr);
    REQUIRE(ctx.animation.bonePaletteSSBO == VK_NULL_HANDLE);
    REQUIRE(!ctx.animation.uploadBones);
    REQUIRE(ctx.animation.skinnedMeshPass == nullptr);
    REQUIRE(ctx.animation.skinnedPipeLayout == VK_NULL_HANDLE);

    // world (engine snapshots)
    REQUIRE(ctx.world.worldBounds == nullptr);
    REQUIRE(ctx.world.assetManifest == nullptr);

    // perf (budget + live counters + network observability)
    REQUIRE(ctx.perf.budget.targetFps == 60.0f);
    REQUIRE(ctx.perf.budget.maxDrawCalls == 10000);
    REQUIRE(ctx.perf.frameTimeMs == 0.0f);
    REQUIRE(ctx.perf.drawCallCount == 0);
    REQUIRE(ctx.perf.triangleCount == 0);
    REQUIRE(ctx.perf.network.tickMs == 0.0f);
    REQUIRE(ctx.perf.network.bytesPerSec == 0);
    REQUIRE(ctx.perf.network.droppedDatagramPct == 0.0f);

    // network (active networking services — `context`
    // replaces the old flat `INetworkContext* network` slot.
    // added a second slot, `transport`, for plugin-level
    // access to the MsQuic datagram API.)
    REQUIRE(ctx.network.context == nullptr);
    REQUIRE(ctx.network.transport == nullptr);
}

TEST_CASE("BaseSystemContext: sub-struct writes are independent", "[mock][refactor]") {
    BaseSystemContext ctx{};

    // Touch one field in every sub-struct; nothing else should leak.
    ctx.rendering.renderWidth         = 1920;
    ctx.rendering.renderHeight        = 1080;
    ctx.rendering.dlssAvailable       = true;
    ctx.ui.uiStyleVersion             = 3;
    ctx.input.isKeyDown               = [](int) { return true; };
    ctx.buffers.createBuffer          = [](VkDeviceSize, VkBufferUsageFlags, bool) { return sv::VkBuf{}; };
    ctx.vkfn.fnCmdBindPipeline        = reinterpret_cast<PFN_vkCmdBindPipeline>(std::uintptr_t{0xDEAD});
    ctx.meshRegistry.registeredMeshCount = 7;
    ctx.audio.postAudioEvent          = [](const char*) -> uint32_t { return 42; };
    ctx.animation.bonePaletteDescSet  = reinterpret_cast<VkDescriptorSet>(std::uintptr_t{0xBEEF});

    static const sv::WorldBounds stubBounds{};
    ctx.world.worldBounds             = &stubBounds;

    // touch perf sub-struct independent of everything else
    ctx.perf.frameTimeMs              = 12.3f;
    ctx.perf.drawCallCount            = 420;
    ctx.perf.triangleCount            = 987654;
    ctx.perf.network.bytesPerSec      = 524288;
    ctx.perf.network.tickMs           = 8.5f;

    // touch network sub-struct with a real NoOp impl so we
    // can verify the write lands AND doesn't alias the perf.network
    // observability block (different sub-structs, same unqualified
    // name `network`).
    auto noOpNet = sv::createNoOpNetworkContext();
    ctx.network.context = noOpNet.get();

    REQUIRE(ctx.rendering.renderWidth == 1920);
    REQUIRE(ctx.rendering.renderHeight == 1080);
    REQUIRE(ctx.rendering.dlssAvailable == true);
    REQUIRE(ctx.ui.uiStyleVersion == 3);
    REQUIRE(ctx.input.isKeyDown);
    REQUIRE(ctx.input.isKeyDown(0));
    REQUIRE(ctx.buffers.createBuffer);
    REQUIRE(ctx.vkfn.fnCmdBindPipeline != nullptr);
    REQUIRE(ctx.meshRegistry.registeredMeshCount == 7);
    REQUIRE(ctx.audio.postAudioEvent("x") == 42);
    REQUIRE(ctx.animation.bonePaletteDescSet != VK_NULL_HANDLE);
    REQUIRE(ctx.world.worldBounds == &stubBounds);
    REQUIRE(ctx.perf.frameTimeMs == 12.3f);
    REQUIRE(ctx.perf.drawCallCount == 420);
    REQUIRE(ctx.perf.triangleCount == 987654);
    REQUIRE(ctx.perf.network.bytesPerSec == 524288);
    REQUIRE(ctx.perf.network.tickMs == 8.5f);

    // network.context lands independently of perf.network.*
    REQUIRE(ctx.network.context != nullptr);
    REQUIRE(ctx.network.context == noOpNet.get());

    // Unrelated sub-structs stay untouched
    REQUIRE(!ctx.rendering.getGraphicsPipeline);
    REQUIRE(!ctx.ui.saveStyleSection);
    REQUIRE(!ctx.input.isKeyPressed);
    REQUIRE(!ctx.buffers.destroyBuffer);
    REQUIRE(ctx.vkfn.fnCmdDraw == nullptr);
    REQUIRE(!ctx.meshRegistry.registerMaterial);
    REQUIRE(!ctx.audio.stopAudioByTag);
    REQUIRE(ctx.animation.animationSystem == nullptr);
    REQUIRE(ctx.world.assetManifest == nullptr);
    REQUIRE(ctx.perf.budget.targetFps == 60.0f); // budget defaults untouched
    REQUIRE(ctx.perf.cpuFrameTimeMs == 0.0f);
    REQUIRE(ctx.perf.gpuFrameTimeMs == 0.0f);
}
