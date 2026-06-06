// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <entt/signal/dispatcher.hpp>
#include <cstdint>

namespace sv {

// ── Event types ─────────────────────────────────────────────────

struct WindowResizeEvent {
    uint32_t width;
    uint32_t height;
};

struct SwapchainRecreatedEvent {
    uint32_t width;
    uint32_t height;
};

enum class Feature : uint8_t {
    RTShadows,
    ReSTIR,
    SHaRC,
    DLSS
};

struct FeatureToggleEvent {
    Feature feature;
    bool    enabled;
};

struct ShaderReloadEvent {};

struct FrameBeginEvent {
    uint32_t frameIndex;
};

struct FrameEndEvent {};

struct ConfigChangedEvent {};

struct DLLReloadEvent {
    const char* dllPath;
    int         systemCount;
};

struct GamepadConnectionEvent {
    int  jid;
    bool connected;
    char name[128];
};

struct ConstraintBrokenEvent {
    uint32_t constraintId;
    uint32_t bodyIdA;
    uint32_t bodyIdB;
};

// Inventory state changed (item added/removed/swapped)
struct InventoryChangedEvent {
    uint32_t     entityId;    // entt entity as uint32_t (avoids entt/entity include)
    int8_t       slotIndex;   // -1 = bulk change (clear all, craft, etc.)
    uint16_t     itemId;
    uint16_t     newCount;
};

// Resource node gathered
struct ResourceGatheredEvent {
    uint32_t     nodeEntityId;
    uint32_t     playerEntityId;
    uint16_t     itemId;
    uint16_t     count;
};

// Crafting progress events
struct CraftingStartedEvent {
    uint32_t     entityId;
    int          recipeIndex;    // index into recipe catalog
    float        craftTime;      // total craft duration (seconds)
};

struct CraftingCompletedEvent {
    uint32_t     entityId;
    int          recipeIndex;
    uint16_t     outputItemId;
    uint16_t     outputQty;
};

// Convenience alias
using EventBus = entt::dispatcher;

} // namespace sv
