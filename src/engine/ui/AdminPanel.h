// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "../PermissionScope.h"  // scope-gated Edit tab
#include "../PostProcess.h"
#include "AdminPanelDecorations.h"
#include "UiStyle.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sv {

// Forward declaration — AdminPanel only needs a pointer; the
// AssetBrowser header is included by the .cpp that draws the tab.
class AssetBrowser;

// Forward declaration — PerformanceContext lives in
// BaseSystemContext.h; AdminPanel reads it via a const pointer so
// the public header stays free of the full BaseSystemContext dep.
struct PerformanceContext;

// Forward declaration — NetworkContext lives in
// BaseSystemContext.h. The Edit tab reads `scope`/`clientId`/
// `avatarEntityId` via a const pointer; no other BSC bits needed.
struct NetworkContext;

// Callbacks for state persistence actions (set once by Engine)
struct AdminCallbacks {
    std::function<void()> onSave;
    std::function<void()> onLoad;
    std::function<void()> onReset;
};

// Pointers to Engine live state that the admin panel reads/writes directly.
// All pointers must remain valid for the panel's lifetime.
//
// Game-specific bindings (SceneUBO, Ocean, PlayerComponent, etc.) are NOT
// part of this struct.  Games inject UI for those via extraTabUI callbacks.
struct AdminBindings {
    // Post-processing (engine-owned PostProcessUBO)
    PostProcessUBO* postProc = nullptr;

    // Feature toggles
    bool*   dlssEnabled   = nullptr;
    bool*   restirEnabled = nullptr;
    bool*   sharcEnabled  = nullptr;
    int*    dlssQualityIndex = nullptr;
    bool*   rtShadowManual   = nullptr;

    // SHaRC GI tuning
    float*    sharcLogBase       = nullptr;
    float*    sharcLevelBias     = nullptr;
    uint32_t* sharcStaleFrameMax = nullptr;

    // Display info (read-only)
    float   fps           = 0.f;
    bool    rtAvailable   = false;
    bool    rqAvailable   = false;
    bool    dlssAvailable = false;

    // GPU profiling (read-only display)
    const float*       gpuPassMs          = nullptr;
    const char* const* gpuPassNames       = nullptr;
    int                gpuTimerCount      = 0;
    bool               gpuProfilingEnabled = false;
    float              vramUsedMB         = 0.f;
    float              vramBudgetMB       = 0.f;

    // Live performance counters + budget + network observability.
    // Games wire this to their engine's live PerformanceContext so
    // the Render tab can show draw counts, triangle counts, CPU/GPU
    // frame time, and the 6 network fields from docs/NETWORK_DESIGN.md
    // §7 without any extra flat fields. Leave null to keep the older
    // HUD layout.
    const PerformanceContext* perfContext = nullptr;

    // Live NetworkContext pointer used by the Edit tab to
    // show the client's scope / clientId / avatarEntityId and to
    // gate the Undo/Redo/Move buttons on scope >= Editor. Leave
    // null on single-player hosts to hide the Edit tab entirely.
    const NetworkContext* networkContext = nullptr;

    // Callbacks fired when the user clicks the Edit tab
    // buttons. Games wire these into their own transaction sender
    // (or into the lab harness helpers below). Any callback left
    // null keeps the matching button disabled.
    std::function<void(float dx, float dy, float dz)> onAvatarMove;
    std::function<void()>                              onUndo;
    std::function<void()>                              onRedo;

    // Admin-open state (read by DLL drawUI to skip when closed)
    bool*   adminOpen     = nullptr;

    // Asset browser — engine-owned. If non-null AND "Assets"
    // appears in tabNames, AdminPanel draws a file tree + filter UI
    // directly. extraTabUI["Assets"] callbacks still run after the
    // engine-drawn content for game-specific inspector panels.
    AssetBrowser* assetBrowser = nullptr;

    // Tab configuration — games define which tabs appear and in what order.
    // The "Render" tab receives additional engine-drawn content (GPU profiling,
    // DLSS, ReSTIR, SHaRC).  The "Assets" tab receives engine-drawn asset
    // browser content when assetBrowser is set.  All other tabs are pure
    // extraTabUI dispatchers.
    std::vector<const char*> tabNames;

    // Extra UI callbacks — DLL systems and game code inject content into tabs.
    // Key = tab name (e.g. "Terrain", "Render"), value = draw callbacks.
    // This is the primary mechanism for game-specific admin panel content.
    std::unordered_map<std::string, std::vector<std::function<void()>>> extraTabUI;
};

class AdminPanel {
public:
    // Draw the admin panel.  Call between ImGui::NewFrame() and Render().
    // open is read/written — caller can close via F2 toggle.
    void draw(AdminBindings& b, bool& open);

    // Set persistence callbacks (called once after Engine creates the panel)
    void setCallbacks(AdminCallbacks cb) { m_callbacks = std::move(cb); }

    // Decoration rects populated each frame by draw(), read by DLL systems
    AdminDecorationRects m_decorationRects;

private:
    // Tab drawing helpers
    void drawTabRender(AdminBindings& b);
    void drawTabAssets(AdminBindings& b);
    void drawTabEdit  (AdminBindings& b);
    void drawExtraTab(const char* name, AdminBindings& b);

    // Helpers
    static void helpMarker(const char* desc);
    static bool sliderResetFloat(const char* label, float* v,
                                  float vmin, float vmax, float defVal,
                                  const char* fmt = "%.3f");

    // Decoration rect capture wrappers
    bool trackedCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags);
    void trackedSeparator();

    AdminCallbacks m_callbacks;
    bool m_showResetConfirm = false;
    int  m_lastActiveTab    = -1;

    // Render tab section collapse state
    bool m_renderPerf        = true;
    bool m_renderDLSS        = true;
    bool m_renderLighting    = true;
    bool m_renderSharcTuning = false;  // collapsed by default — advanced
    bool m_renderHealth      = true;

    // Assets tab state
    char m_assetsFilter[128] = {};   // substring filter input
    int  m_assetsKindFilter  = -1;   // -1 = all, otherwise AssetKind int
    int  m_assetsSelected    = -1;   // index into filtered view

    // GPU timing smoothing (EMA)
    static constexpr int kMaxTimers = 10;
    float m_smoothedPassMs[kMaxTimers + 1]{};  // +1 for total
    bool  m_smoothedInit = false;
};

} // namespace sv
