// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// Engine-generic subset of the DLL boundary context.
// Games extend this to define SystemContext with game-specific pointers.
//
// Usage (in game code):
//   struct SystemContext : BaseSystemContext {
//       const SceneUBO*   sceneState = nullptr;
//       const TerrainUBO* terrainState = nullptr;
//       // ... game-specific pointers and function pointers
//   };
//
// ── Layout (1.2.0; perf; network) ──
// The context is organized as a flat set of hot fields (device,
// allocator, ECS, window, shared flags, core services) plus eleven
// nested POD sub-structs that group fields by ownership:
//
//   rendering     — formats, resolution, frame, quality, HZB, postproc
//   ui            — ImGui + UI pipeline layouts + style JSON
//   input         — keyboard, mouse, actions, gamepad
//   buffers       — VMA buffer/image wrappers, shader module loader
//   vkfn          — 29 volk-loaded PFN_vk* function pointers
//   meshRegistry  — multi-mesh indirect draw SSBO registry
//   audio         — event-driven audio API
//   animation     — ozz runtime, bone palette, skinned pipeline layouts
//   world         — world bounds + asset manifest (engine snapshot)
//   perf          — PerformanceBudget + live frame/draw/VRAM counters
//                    + NetworkStats observability placeholders
//   network       — INetworkContext* + future transport / permission
//                    / edit-transaction slots (1.3.0)
//
// Each sub-struct is a plain POD (trivially copyable, default member
// initializers only, no constructors/virtuals). The outer struct is
// still trivially copyable and DLL-safe. Access paths changed from
//   ctx.isKeyDown(K_SPACE)
// to
//   ctx.input.isKeyDown(K_SPACE)
// Games update their ctx-wiring code in onInit(), and DLL plugins
// update their field accesses to use the sub-struct paths.
//
// 1.3.0 migration: the flat `INetworkContext* network`
// slot moved INTO the new `network` sub-struct as `network.context`:
//   1.2.x: ctx.network->tick();          1.3.0: ctx.network.context->tick();
//   1.2.x: if (ctx.network) ...          1.3.0: if (ctx.network.context) ...
//   1.2.x: ctx.network = impl.get();     1.3.0: ctx.network.context = impl.get();
// See CHANGELOG.md §1.3.0 for the full migration table.

#include "vk/VkBuffer.h"               // VkBuf (value type in function signatures)
#include "PermissionScope.h"           // NetworkContext::scope
#include <volk.h>                       // VkDevice, VkFormat, PFN_vk*, etc.
#include <vk_mem_alloc.h>              // VmaAllocator, VmaAllocation
#include <entt/entity/registry.hpp>    // entt::registry
#include <entt/signal/dispatcher.hpp>  // entt::dispatcher
#include <glm/glm.hpp>                 // glm::mat4, glm::vec2
#include <functional>                  // std::function
#include <cstdint>

namespace sv {

using EventBus = entt::dispatcher;     // also defined in Events.h

// Forward declarations (complete types not needed for pointers / std::function args)
struct GraphicsPipelineDesc;
struct PostProcessUBO;
struct AdminDecorationRects;
struct FrameData;
struct MeshVertex;
struct MeshSlot;

// Forward declarations for engine service interfaces
class EngineLog;
class INetworkContext;
class IPhysicsContext;
class AnimationSystem;
class SkinnedMeshPass;
struct WorldBounds; // Config.h
class AssetManifest; // AssetManifest.h

// MsQuic transport handle forward decl.
// Kept as a namespace-qualified forward decl so the DLL boundary
// stays clean of <msquic.h>. Transport itself lives in src/engine/net.
namespace net { class Transport; }

// ═══════════════════════════════════════════════════════════════
// Nested sub-structs (POD, trivially copyable, DLL-safe)
// ═══════════════════════════════════════════════════════════════

// ── RenderingContext (29 fields) ────────────────────────────────
// Frozen renderer state: formats, resolution, frame index, the
// scene descriptor/pipeline layouts the engine owns, GPU feature
// flags, quality/render settings, post-process UBO, render-graph
// pass registration, HZB resources, and the screenshot hook.
struct RenderingContext {
    // ── Formats + resolution ─────────────────────────────────
    VkFormat  hdrFormat       = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat  swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
    uint32_t  renderWidth     = 0;
    uint32_t  renderHeight    = 0;
    uint32_t  currentFrame    = 0;       // double-buffer index (0 or 1)
    glm::mat4 currentViewProj = glm::mat4(1.0f);

    // ── Descriptor Sets & Layouts (frozen renderer) ─────────
    VkDescriptorSetLayout sceneDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout      scenePipeLayout = VK_NULL_HANDLE;

    // ── GPU Feature Availability ────────────────────────────
    bool rtAvailable   = false;
    bool rqAvailable   = false;
    bool dlssAvailable = false;

    // ── Quality / Render Settings (engine-owned) ────────────
    int*      fpsCapTarget      = nullptr;
    int*      dlssQualityIndex  = nullptr;
    bool*     dlssEnabled       = nullptr;
    int*      qualityPresetIndex = nullptr;
    bool*     restirEnabled     = nullptr;
    bool*     sharcEnabled      = nullptr;
    bool*     rtShadowManual    = nullptr;
    uint32_t* shadowMapSize     = nullptr;

    // ── PostProcess (engine-owned) ──────────────────────────
    PostProcessUBO* postProcMutable = nullptr;

    // ── Pipeline factory ────────────────────────────────────
    std::function<VkPipeline(const GraphicsPipelineDesc&)> getGraphicsPipeline;

    // ── Render Graph: DLL Pass Registration ─────────────────
    // DLL-safe callback: void(*)(VkCommandBuffer, const FrameData*, const void*)
    // Sort orders 0-110 used by built-in passes; DLL passes use gaps.
    std::function<bool(const char* name, uint32_t sortOrder,
                       void(*recordFn)(VkCommandBuffer, const FrameData*, const void*),
                       const void* userData)> registerRenderPass;
    std::function<void(const char* name)> unregisterRenderPass;

    // ── HZB Occlusion Culling ───────────────────────────────
    VkImageView hzbImageView = VK_NULL_HANDLE;
    VkSampler   hzbSampler   = VK_NULL_HANDLE;
    uint32_t    hzbWidth     = 0;
    uint32_t    hzbHeight    = 0;
    uint32_t    hzbMipLevels = 0;

    // ── Screenshot Capture ──────────────────────────
    // Game wires this to its swapchain readback implementation.
    // Path should include extension (e.g. ".png").  Returns true on success.
    std::function<bool(const char* path)> captureScreenshot;
};

// ── UIContext (12 fields) ───────────────────────────────────────
// ImGui handle, admin panel decorations, UI theme JSON blobs, and
// the UI shader pipeline layouts the engine owns.
struct UIContext {
    // ── ImGui (DLL boundary) ────────────────────────────────
    void*                       imguiContext           = nullptr;
    const AdminDecorationRects* adminDecoRects         = nullptr;
    const char*                 uiStyleDecorationsJSON = nullptr;
    const char*                 uiStyleBackgroundJSON  = nullptr;
    uint32_t                    uiStyleVersion         = 0;

    // ── UI Pipeline Layouts (engine UI system) ──────────────
    VkPipelineLayout uiBgPipeLayout        = VK_NULL_HANDLE;
    VkPipelineLayout uiGlassPipeLayout     = VK_NULL_HANDLE;
    const char*      uiStyleGlassJSON      = nullptr;
    VkPipelineLayout uiAtmoPipeLayout      = VK_NULL_HANDLE;
    const char*      uiStyleAtmosphereJSON = nullptr;
    std::function<void(const char* section, const char* json)> saveStyleSection;
    bool*            uiShadersEnabled      = nullptr;
};

// ── InputContext (15 fields) ────────────────────────────────────
// Raw keyboard/mouse, rebindable action queries, and gamepad state.
struct InputContext {
    // ── Keyboard + Mouse ────────────────────────────────────
    std::function<bool(int key)> isKeyPressed;
    std::function<bool(int key)> isKeyDown;
    std::function<glm::vec2()>   getMousePos;
    std::function<bool(int)>     isMouseDown;
    std::function<float()>       getScrollDelta;
    std::function<bool()>        isCursorLocked;
    std::function<void(bool)>    setCursorLocked;
    std::function<void()>        toggleFullscreen;

    // ── Input Actions (rebindable, gamepad-aware) ───────────
    std::function<bool(int action)>  isActionDown;      // int = Action enum cast
    std::function<bool(int action)>  isActionPressed;
    std::function<float(int action)> getActionAxis;     // 0..1 magnitude

    // ── Gamepad ─────────────────────────────────────────────
    std::function<bool()>          isGamepadConnected;
    std::function<bool(int btn)>   isGamepadButtonDown;
    std::function<bool(int btn)>   isGamepadButtonPressed;
    std::function<float(int axis)> getGamepadAxis;
};

// ── BufferContext (7 fields) ────────────────────────────────────
// VMA buffer wrappers, shader module loader, VMA image wrappers.
struct BufferContext {
    // ── Buffer Management (VMA, DLL-safe) ───────────────────
    std::function<VkBuf(VkDeviceSize size, VkBufferUsageFlags usage, bool cpuVisible)> createBuffer;
    std::function<void(VkBuf&)>        destroyBuffer;
    std::function<void*(const VkBuf&)> mapBuffer;
    std::function<void(const VkBuf&)>  unmapBuffer;

    // ── Shader Compilation ──────────────────────────────────
    // Returns VK_NULL_HANDLE on failure.  Caller owns the module.
    std::function<VkShaderModule(const char* path, VkShaderStageFlagBits stage)> createShaderModule;

    // ── VMA Image Wrappers ──────────────────────────────────
    std::function<VkResult(VkDevice, const VkImageCreateInfo*, VkImage*, VmaAllocation*)> createImage;
    std::function<void(VkImage, VmaAllocation)> destroyImage;
};

// ── VulkanFnContext (29 fields) ─────────────────────────────────
// Volk-loaded PFN_vk* function pointers. Split into "drawing" and
// "compute/resource management" groups for readability.
struct VulkanFnContext {
    // ── Drawing ─────────────────────────────────────────────
    PFN_vkCmdBindPipeline           fnCmdBindPipeline        = nullptr;
    PFN_vkCmdBindDescriptorSets     fnCmdBindDescriptorSets  = nullptr;
    PFN_vkCmdBindVertexBuffers      fnCmdBindVertexBuffers   = nullptr;
    PFN_vkCmdBindIndexBuffer        fnCmdBindIndexBuffer     = nullptr;
    PFN_vkCmdDrawIndexed            fnCmdDrawIndexed         = nullptr;
    PFN_vkCmdDraw                   fnCmdDraw                = nullptr;
    PFN_vkCmdDrawIndirect           fnCmdDrawIndirect        = nullptr;
    PFN_vkCmdDrawIndexedIndirect    fnCmdDrawIndexedIndirect = nullptr;
    PFN_vkCmdPushConstants          fnCmdPushConstants       = nullptr;
    PFN_vkCmdSetScissor             fnCmdSetScissor          = nullptr;
    // ── Compute + resource management ───────────────────────
    PFN_vkCmdDispatch               fnCmdDispatch                = nullptr;
    PFN_vkCmdPipelineBarrier        fnCmdPipelineBarrier         = nullptr;
    PFN_vkCmdFillBuffer             fnCmdFillBuffer              = nullptr;
    PFN_vkCmdUpdateBuffer           fnCmdUpdateBuffer            = nullptr;
    PFN_vkCreateDescriptorPool      fnCreateDescriptorPool       = nullptr;
    PFN_vkDestroyDescriptorPool     fnDestroyDescriptorPool      = nullptr;
    PFN_vkCreateDescriptorSetLayout fnCreateDescriptorSetLayout  = nullptr;
    PFN_vkDestroyDescriptorSetLayout fnDestroyDescriptorSetLayout = nullptr;
    PFN_vkAllocateDescriptorSets    fnAllocateDescriptorSets     = nullptr;
    PFN_vkUpdateDescriptorSets      fnUpdateDescriptorSets       = nullptr;
    PFN_vkCreatePipelineLayout      fnCreatePipelineLayout       = nullptr;
    PFN_vkDestroyPipelineLayout     fnDestroyPipelineLayout      = nullptr;
    PFN_vkCreateComputePipelines    fnCreateComputePipelines     = nullptr;
    PFN_vkDestroyPipeline           fnDestroyPipeline            = nullptr;
    PFN_vkCreateImageView           fnCreateImageView            = nullptr;
    PFN_vkDestroyImageView          fnDestroyImageView           = nullptr;
    PFN_vkCreateSampler             fnCreateSampler              = nullptr;
    PFN_vkDestroySampler            fnDestroySampler             = nullptr;
    PFN_vkDestroyShaderModule       fnDestroyShaderModule        = nullptr;
};

// ── MeshRegistryContext (9 fields) ──────────────────────────────
// Multi-mesh indirect draw registry: mega VBO/IBO + slot SSBO.
struct MeshRegistryContext {
    // ── Registration hooks ──────────────────────────────────
    std::function<uint32_t(const MeshVertex* verts, uint32_t vertCount,
                           const uint32_t* indices, uint32_t idxCount,
                           float boundingSphereRadius)> registerMesh;
    std::function<uint32_t(const glm::vec4& albedo)> registerMaterial;
    std::function<const MeshSlot*(uint32_t meshId)>  getMeshSlot;

    // ── Engine-owned SSBOs + mega buffers ───────────────────
    VkBuffer meshSlotSSBO            = VK_NULL_HANDLE;
    VkBuffer materialSSBO            = VK_NULL_HANDLE;
    VkBuffer megaVBO                 = VK_NULL_HANDLE;
    VkBuffer megaIBO                 = VK_NULL_HANDLE;
    uint32_t registeredMeshCount     = 0;
    uint32_t registeredMaterialCount = 0;
};

// ── AudioContext (8 fields) ─────────────────────────────────────
// Event-driven audio API (post/stop/param/volume/bus).
struct AudioContext {
    std::function<uint32_t(const char* eventName)>                              postAudioEvent;
    std::function<uint32_t(const char* eventName, float x, float y, float z)>   postAudioEventAt;
    std::function<void(uint32_t handle)>                                        stopAudioEvent;
    std::function<void(const char* tag)>                                        stopAudioByTag;
    std::function<void(const char* name, float value)>                          setAudioParam;
    std::function<void(const char* tag, bool active)>                           setAudioEnvironment;
    std::function<void(int bus, float vol)>                                     setAudioVolume;
    std::function<float(int bus)>                                               getAudioVolume;
};

// ── AnimationContext (10 fields) ────────────────────────────────
// ozz-animation runtime, bone palette SSBO, skinned mesh pipeline.
struct AnimationContext {
    // ── Animation runtime + bone palette ────────────
    AnimationSystem*      animationSystem    = nullptr;            // engine-owned animation runtime
    VkBuffer              bonePaletteSSBO    = VK_NULL_HANDLE;     // SSBO: std430 mat4 bones[]
    VkDescriptorSetLayout bonePaletteLayout  = VK_NULL_HANDLE;     // set 1 binding 0 layout
    VkDescriptorSet       bonePaletteDescSet = VK_NULL_HANDLE;     // pre-allocated descriptor set
    std::function<void()>                                              resetBonePalette;   // call at frame start
    std::function<uint32_t(const glm::mat4* matrices, uint32_t count)> uploadBones;        // returns bone offset
    std::function<void()>                                              flushBonePalette;   // call before rendering

    // ── Skinned Mesh Rendering ──────────────
    SkinnedMeshPass* skinnedMeshPass          = nullptr;           // engine-owned skinned mesh renderer
    VkPipelineLayout skinnedPipeLayout        = VK_NULL_HANDLE;    // set 0 scene + set 1 bone palette + set 2 material
    VkPipelineLayout skinnedShadowPipeLayout  = VK_NULL_HANDLE;    // set 0 scene + set 1 bone palette + cascade push
};

// ── WorldContext (2 fields) ─────────────────────────────────────
// Engine snapshots that plugins can read without pulling engine
// headers across the DLL boundary.
struct WorldContext {
    // Engine-owned snapshot of the playable-area AABB. Populated by
    // EngineBase::initServices() from Config ("world.boundsMin" /
    // "world.boundsMax"). Games point their SystemContext at this
    // slot in onInit() so DLL plugins can clamp/cull against it
    // without pulling Config.h across the DLL boundary.
    const WorldBounds*   worldBounds   = nullptr;

    // Engine-owned asset manifest (preload list loaded from JSON).
    // Plugins look up mesh/texture/audio entries by logical name.
    // Actual GPU upload stays in game init code — the manifest only
    // exposes the list of entries and their paths.
    const AssetManifest* assetManifest = nullptr;
};

// ── PerformanceBudget ─────────────────────────────────
// Games configure budget targets; the engine reports runtime
// counters against them. Budget values are targets/warnings, not
// enforced caps — nothing in the engine refuses to render a frame
// because it exceeded these numbers. The AdminPanel HUD uses the
// budget as the "green / yellow / red" threshold when drawing the
// live frame-time and VRAM bars.
//
// See docs/PERF_BASELINE.md for recommended values per game.
struct PerformanceBudget {
    // ── Frame budget ────────────────────────────────────────
    float    targetFps    = 60.0f;    // desired frames per second
    float    maxFrameMs   = 16.67f;   // 1000/targetFps; hard-budget threshold

    // ── Draw budget (warnings only, not enforced) ───────────
    uint32_t maxDrawCalls = 10000;
    uint32_t maxTriangles = 5'000'000;

    // ── GPU memory budget (MB, nominal ceiling) ─────────────
    uint32_t maxGpuMemMB  = 8192;
};

// ── NetworkStats (placeholder) ─────
// The six network observability fields specified in
// docs/NETWORK_DESIGN.md §7. Added now as zero-valued placeholders
// so the AdminPanel HUD + DLL plugin surface are ready when the
// actual replication + transport sessions land. Populated by:
//   - tickMs                → server loop
//   - bytesPerSec / pktPerSec → transport callbacks
//   - replicatedEntityCount  → snapshot generator
//   - ackLatencyMs           → reliable ack stream
//   - droppedDatagramPct     → QUIC loss telemetry
//
// This block intentionally stays under `perf`
// rather than migrating into the new `NetworkContext` sub-struct.
// Rationale in the note on PerformanceContext::network below and
// in docs/NETWORK_DESIGN.md §7 — observability lives with the other
// perf bars the AdminPanel HUD draws each frame.
struct NetworkStats {
    // uint64_t first — keeps the 8-byte alignment hole out of the
    // middle of the struct, shaving a trailing pad byte.
    uint64_t bytesPerSec           = 0;    // per-client outbound bandwidth
    uint32_t packetsPerSec         = 0;    // per-client outbound packet rate
    uint32_t replicatedEntityCount = 0;    // entities this client is tracking
    float    tickMs                = 0.0f; // server tick budget (ms)
    float    ackLatencyMs          = 0.0f; // RTT via replication ack stream
    float    droppedDatagramPct    = 0.0f; // fraction of unreliable datagrams dropped (0..1)
};

// ── PerformanceContext ────────────────────────────────
// Live engine performance counters + configurable budget + network
// observability placeholders. The engine populates the
// runtime fields each frame; games wire their live instance into
// SystemContext.perf so DLL plugins and the AdminPanel HUD can
// read without reaching into the render loop.
//
// Populated by:
//   - budget        → game (once at startup or on reconfiguration)
//   - frameTimeMs   → engine main loop, from onFrame(dt)
//   - avgFps        → engine EMA over frameTimeMs
//   - cpuFrameTimeMs/gpuFrameTimeMs → engine CPU timer + GpuProfiler
//   - drawCallCount / triangleCount → recordFrame() accumulator
//   - vramUsedMB / vramBudgetMB      → VMA stats query
//   - network.*     → networking sessions (placeholders today)
//
// Zero-initialised values mean "not yet measured" — the HUD hides
// bars for zero-valued fields rather than drawing empty progress.
struct PerformanceContext {
    // ── Budget (games set, engine reads) ─────────────────────
    PerformanceBudget budget {};

    // ── Frame timing (engine populates per frame) ────────────
    float frameTimeMs     = 0.0f; // wall-clock delta this frame (ms)
    float cpuFrameTimeMs  = 0.0f; // CPU-side work (main thread)
    float gpuFrameTimeMs  = 0.0f; // GPU-side work (sum of profiler passes)
    float avgFps          = 0.0f; // EMA-smoothed fps

    // ── Draw counters (engine populates, game records) ───────
    uint32_t drawCallCount = 0;
    uint32_t triangleCount = 0;

    // ── GPU memory (VMA stats, engine populates) ─────────────
    float vramUsedMB   = 0.0f;
    float vramBudgetMB = 0.0f;

    // ── Network observability (placeholders) ────────
    // NOTE: NetworkStats intentionally stays here (under `perf`)
    // and does NOT move into the new NetworkContext sub-struct.
    // Rationale: these are passive runtime *observability* counters
    // that the AdminPanel HUD renders alongside frameTime / draw /
    // VRAM bars. NetworkContext holds active networking *services*
    // (interface pointer, future transport handles). Keeping the
    // split — services in `network`, metrics in `perf.network` —
    // means the AdminPanel reads a single `PerformanceContext`
    // slice per frame without chasing pointers into a second
    // sub-struct. See docs/NETWORK_DESIGN.md §7 for the rationale.
    NetworkStats network {};
};

// ── NetworkContext (1.3.0 + 1.3.1 + 1.3.4) ─
// Engine networking services exposed to DLL plugins. The flat
// `INetworkContext* network` slot on BaseSystemContext was
// promoted into this sub-struct so future networking
// work can add transport / permission-scope / snapshot slots
// without breaking the DLL ABI.
//
// ── Fields (5 slots) ──────────────────────────────────
//   context        — client-side INetworkContext* (semantics
//                    identical to the 1.2.x flat pointer). NULL =
//                    no legacy impl wired; games point this at
//                    createNoOpNetworkContext() or their concrete
//                    impl in onInit().
//   transport      — raw sv::net::Transport* for plugins that need
//                    to call sendDatagram / setDatagramHandler /
//                    connect directly. The host (lab harness,
//                    stratumv_server, or game engine) owns the
//                    Transport's lifetime; this slot is a
//                    non-owning borrow. NULL when the host doesn't
//                    expose its transport (offline lab runs,
//                    single-player-only games).
//   scope          — local client's permission scope, populated
//                    from the Welcome message after connect.
//                    Default Spectator so AdminPanel tabs that
//                    require Editor stay hidden on unattached
//                    hosts.
//   clientId       — server-assigned monotonic id for the local
//                    client. 0 = not-yet-welcomed or not connected.
//   avatarEntityId — entity id of the client's auto-spawned avatar.
//                    0 = not-yet-welcomed. Used by the lab harness
//                    to drive its own avatar via SetField
//                    transactions.
//
// Slots still pending:
//   join-with-snapshot apply hook
//   asset sync channel accessor
//
// Why not move `PerformanceContext::network` (NetworkStats) into
// this sub-struct? The split is intentional:
//   ctx.network.*       → active networking services
//   ctx.perf.network.*  → passive observability counters
// See the comment on PerformanceContext::network above and
// docs/NETWORK_DESIGN.md §7 for the full rationale.
struct NetworkContext {
    INetworkContext* context        = nullptr;
    net::Transport*  transport      = nullptr;   // 1.3.1
    PermissionScope  scope          = PermissionScope::Spectator;
    uint32_t         clientId       = 0;
    uint32_t         avatarEntityId = 0;
};

// ═══════════════════════════════════════════════════════════════
// BaseSystemContext — flat hot fields + eleven nested sub-structs
// ═══════════════════════════════════════════════════════════════

struct BaseSystemContext {

    // ── Core Vulkan & Memory ────────────────────────────────────
    VkDevice      device    = VK_NULL_HANDLE;
    VmaAllocator  allocator = VK_NULL_HANDLE;

    // ── ECS & Events ────────────────────────────────────────────
    entt::registry* ecs    = nullptr;
    EventBus*       events = nullptr;

    // ── Window ──────────────────────────────────────────────────
    uint32_t windowWidth            = 0;
    uint32_t windowHeight           = 0;
    bool     isBorderlessFullscreen = false;

    // ── Shared Flags ────────────────────────────────────────────
    bool* wireframeEnabled = nullptr;
    bool* gamePaused       = nullptr;
    bool* requestShutdown  = nullptr;
    bool* adminOpen        = nullptr;

    // ── Logging ──────────────────────────────────────────
    EngineLog* engineLog = nullptr;

    // ── Physics ────────────────────────────────────────
    // Networking moved out of the flat hot fields into the `network`
    // sub-struct below (1.3.0). See the migration note on
    // NetworkContext and CHANGELOG.md §1.3.0.
    IPhysicsContext* physics = nullptr;

    // ── Nested sub-structs ──────────────────────────────────────
    // (1.2.0 shipped the first nine;
    //  added `perf`;
    //  added `network`, semver 1.3.0.)
    RenderingContext    rendering    {};
    UIContext           ui           {};
    InputContext        input        {};
    BufferContext       buffers      {};
    VulkanFnContext     vkfn         {};
    MeshRegistryContext meshRegistry {};
    AudioContext        audio        {};
    AnimationContext    animation    {};
    WorldContext        world        {};
    PerformanceContext  perf         {};
    NetworkContext      network      {};   // 1.3.0
};

} // namespace sv
