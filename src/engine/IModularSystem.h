// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "EngineSystem.h"
#include "BaseSystemContext.h" // engine-generic context
#include "RenderPass.h"         // FrameData

#include <volk.h>
#include <string>
#include <cstdint>

namespace sv {

// Forward declaration — games define the full SystemContext extending BaseSystemContext.
// Engine code (DLLLoader, DevServer) passes it through by reference
// without accessing members directly.
//
// Game usage:
//   struct SystemContext : BaseSystemContext {
//       const SceneUBO* sceneState = nullptr;
//       // ... game-specific pointers
//   };
struct SystemContext;

// Abstract interface for hot-reloadable modular systems.
// Loaded from a DLL, iterated by the Engine main loop.
// Inherits EngineSystem for Connectome self-description.
class IModularSystem : public EngineSystem {
public:
    virtual ~IModularSystem() = default;

    // Engine interface version — bump when IModularSystem ABI changes.
    static constexpr uint32_t kEngineInterfaceVersion = 1;

    virtual const char* name() const = 0;

    // Interface version this plugin was compiled against.
    // Default returns kEngineInterfaceVersion (header-inlined → always current).
    // Stale DLLs compiled against an older header return the old value.
    virtual uint32_t getInterfaceVersion() const { return kEngineInterfaceVersion; }

    // Initialization priority — lower values initialize first. Default 0.
    virtual int32_t initPriority() const { return 0; }

    // Default Connectome metadata — uses name() as placeholder.
    // Plugins override with real metadata.
    NodeMeta getMeta() const override {
        return { name(), name(), "", 8, "#888888", "", "", "", nullptr, 0.f, "", nullptr };
    }
    HealthReport getHealth() const override {
        return { 100.f, true };
    }

    // ── Lifecycle ────────────────────────────────────────────────
    virtual bool init(const SystemContext& ctx) = 0;
    virtual void shutdown() = 0;

    // Called after init with a mutable SystemContext. Systems can set
    // function pointers to expose services to other DLL systems.
    virtual void postInit(SystemContext& /*ctx*/) {}

    // ── Per-frame (all optional) ──���──────────────────────────────
    virtual void tick(float /*dt*/, const SystemContext& /*ctx*/) {}

    // PreDraw: called inside recordMainPass BEFORE terrain/ocean draw.
    virtual void recordPreDraw(VkCommandBuffer /*cmd*/, const FrameData& /*frame*/,
                               const SystemContext& /*ctx*/) {}

    // PostDraw: called inside recordMainPass AFTER terrain/ocean draw.
    virtual void recordPostDraw(VkCommandBuffer /*cmd*/, const FrameData& /*frame*/,
                                const SystemContext& /*ctx*/) {}

    // ShadowDraw: called per cascade inside recordShadowPass AFTER terrain draw.
    // Pipeline, viewport, scissor, and descriptor set 0 are already bound by engine.
    virtual void recordShadowDraw(VkCommandBuffer /*cmd*/, const FrameData& /*frame*/,
                                  const SystemContext& /*ctx*/, uint32_t /*cascadeIndex*/) {}

    // UIPass: called in recordFrame AFTER post-process, BEFORE ImGui.
    // Swapchain is in COLOR_ATTACHMENT_OPTIMAL. Use for UI-layer effects.
    virtual void recordUIPass(VkCommandBuffer /*cmd*/, const FrameData& /*frame*/,
                              const SystemContext& /*ctx*/) {}

    // Compute: called in recordFrame BEFORE ocean compute dispatch.
    virtual void recordCompute(VkCommandBuffer /*cmd*/, const FrameData& /*frame*/,
                               const SystemContext& /*ctx*/) {}

    // PostUIPass: called AFTER ImGui overlay rendering, BEFORE present.
    virtual void recordPostUIPass(VkCommandBuffer /*cmd*/, const FrameData& /*frame*/,
                                  const SystemContext& /*ctx*/) {}

    // ImGui calls (drawn inside the admin panel area)
    virtual void drawUI() {}

    // Which admin tab to draw in (e.g. "Terrain", "Render", "World").
    // Return nullptr or "" to draw standalone (outside any tab).
    virtual const char* adminTab() const { return nullptr; }

    // Extra admin tabs — systems that draw in multiple tabs.
    // drawUI() draws in adminTab(); drawExtraTabUI(idx) draws in extraAdminTab(idx).
    virtual int extraAdminTabCount() const { return 0; }
    virtual const char* extraAdminTab(int /*idx*/) const { return nullptr; }
    virtual void drawExtraTabUI(int /*idx*/) {}

    // Hook mode — declares which rendering hooks this system uses
    enum HookMode : uint8_t { PreDraw = 1, PostDraw = 2, Both = 3 };
    virtual HookMode hookMode() const { return PostDraw; }

    // Called when engine-owned GPU resources are destroyed and recreated
    // (e.g. resolution change, DLSS mode switch). Systems holding
    // descriptor sets that reference these resources must rebind here.
    virtual void onResourcesInvalidated(const SystemContext& /*ctx*/) {}

    // ── Hot-reload ────────��──────────────────────────────────────
    virtual void onReload(const SystemContext& /*ctx*/) {}

    // State serialization (JSON string, safe across DLL boundaries)
    virtual std::string serializeState() { return "{}"; }
    virtual void deserializeState(const std::string& /*json*/) {}
};

// ── DLL export interface ────���────────────────────────────────────
// DLLs implement these two functions.
using CreateSystemsFn  = IModularSystem** (*)(int* outCount);
using DestroySystemsFn = void (*)();

} // namespace sv
