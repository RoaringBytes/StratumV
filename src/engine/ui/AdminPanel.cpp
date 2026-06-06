// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif
#include "AdminPanel.h"
#include "UiStyle.h"
#include "../AssetBrowser.h"
#include "../BaseSystemContext.h"   // PerformanceContext full type
#include "../SystemRegistry.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sv {

// ── Utility helpers ──────────────────────────────────────────────────────────

void AdminPanel::helpMarker(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Slider with an inline reset button. Returns true if value changed.
bool AdminPanel::sliderResetFloat(const char* label, float* v,
                                   float vmin, float vmax, float defVal,
                                   const char* fmt)
{
    bool changed = ImGui::SliderFloat(label, v, vmin, vmax, fmt);
    ImGui::SameLine();
    char btnId[64];
    snprintf(btnId, sizeof(btnId), "##rst_%s", label);
    ImGui::PushStyleColor(ImGuiCol_Button,        style::kResetBtn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  style::kResetBtnHover);
    if (ImGui::SmallButton(btnId)) { *v = defVal; changed = true; }
    ImGui::PopStyleColor(2);
    return changed;
}

// ── Decoration rect capture helpers ──────────────────────────────────────────

bool AdminPanel::trackedCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags)
{
    bool open = ImGui::CollapsingHeader(label, flags);
    int idx = m_decorationRects.headerCount;
    if (idx < AdminDecorationRects::kMaxHeaders) {
        auto& h = m_decorationRects.headers[idx];
        h.min  = ImGui::GetItemRectMin();
        h.max  = ImGui::GetItemRectMax();
        h.open = open;

        // Track hovered header
        if (ImGui::IsItemHovered())
            m_decorationRects.hoveredHeaderIndex = idx;

        // Animate hover progress (lerp toward target)
        float dt = ImGui::GetIO().DeltaTime;
        float hoverTarget = (m_decorationRects.hoveredHeaderIndex == idx) ? 1.0f : 0.0f;
        float& hoverT = m_decorationRects.headerHoverT[idx];
        hoverT += (hoverTarget - hoverT) * std::min(1.0f, dt * 8.0f);

        // Animate open/close progress
        float openTarget = open ? 1.0f : 0.0f;
        float& openT = m_decorationRects.headerOpenT[idx];
        openT += (openTarget - openT) * std::min(1.0f, dt * 5.0f);

        m_decorationRects.headerCount++;
    }
    return open;
}

void AdminPanel::trackedSeparator()
{
    ImGui::Separator();
    if (m_decorationRects.separatorCount < AdminDecorationRects::kMaxSeparators) {
        auto& sep = m_decorationRects.separators[m_decorationRects.separatorCount++];
        sep.left  = ImGui::GetItemRectMin();
        sep.right = ImGui::GetItemRectMax();
    }
}

// ── Extra tab dispatcher ─────────────────────────────────────────────────────

void AdminPanel::drawExtraTab(const char* name, AdminBindings& b)
{
    auto it = b.extraTabUI.find(name);
    if (it != b.extraTabUI.end())
        for (auto& fn : it->second) fn();
}

// ── Edit tab ─────────────────────────────────────────────────────────────────
// Scope-gated collaborative editing surface. Games add "Edit" to
// their `tabNames` list and wire a live `NetworkContext*` + three
// callbacks (`onAvatarMove`, `onUndo`, `onRedo`) into AdminBindings.
// When the client's scope is below Editor the buttons are disabled
// but the identity readout still renders so a spectator can see
// that they're authenticated but read-only.
//
// If the host has no NetworkContext (single-player, offline lab
// run, etc.), the tab shows a disabled "no network" line instead
// of hiding the buttons — leaving the tab visible makes the
// feature discoverable even on non-multiplayer builds.

void AdminPanel::drawTabEdit(AdminBindings& b)
{
    const NetworkContext* net = b.networkContext;
    if (!net) {
        ImGui::TextDisabled("No NetworkContext wired into AdminBindings.");
        ImGui::TextWrapped(
            "Set bindings.networkContext in your game engine's "
            "admin panel setup to enable collaborative editing.");
        drawExtraTab("Edit", b);
        return;
    }

    // ── Identity readout ────────────────────────────────────────
    if (net->clientId == 0) {
        ImGui::TextDisabled("Not connected to a server yet.");
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.95f, 1.0f),
            "Client:   %u   Avatar: %u",
            static_cast<unsigned>(net->clientId),
            static_cast<unsigned>(net->avatarEntityId));
    }
    ImGui::Text("Scope:    %s", permissionScopeToString(net->scope));

    // ── Scope gate for edit buttons ─────────────────────────────
    const bool canEdit =
        (net->clientId != 0) && (net->scope >= PermissionScope::Editor);

    ImGui::Separator();
    if (!canEdit) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.4f, 1.0f),
            "Editor scope required for edit buttons.");
    }

    ImGui::BeginDisabled(!canEdit || !b.onAvatarMove);
    const float moveStep = 5.0f;
    if (ImGui::Button("Move X-", ImVec2(70, 0)) && b.onAvatarMove)
        b.onAvatarMove(-moveStep, 0.0f, 0.0f);
    ImGui::SameLine();
    if (ImGui::Button("Move X+", ImVec2(70, 0)) && b.onAvatarMove)
        b.onAvatarMove(moveStep, 0.0f, 0.0f);
    ImGui::SameLine();
    if (ImGui::Button("Move Z-", ImVec2(70, 0)) && b.onAvatarMove)
        b.onAvatarMove(0.0f, 0.0f, -moveStep);
    ImGui::SameLine();
    if (ImGui::Button("Move Z+", ImVec2(70, 0)) && b.onAvatarMove)
        b.onAvatarMove(0.0f, 0.0f, moveStep);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginDisabled(!canEdit || !b.onUndo);
    if (ImGui::Button("Undo (Ctrl+Z)", ImVec2(140, 0)) && b.onUndo)
        b.onUndo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canEdit || !b.onRedo);
    if (ImGui::Button("Redo (Ctrl+Y)", ImVec2(140, 0)) && b.onRedo)
        b.onRedo();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextWrapped(
        "Collaborative editing substrate. Move buttons "
        "request a SetField on your avatar's NetTransform via "
        "the reliable QUIC stream; the server validates + "
        "broadcasts. Undo/Redo walk the server-side UndoLog.");

    drawExtraTab("Edit", b);
}

// ── Main draw ────────────────────────────────────────────────────────────────

void AdminPanel::draw(AdminBindings& b, bool& open)
{
    // Reset decoration rects each frame
    m_decorationRects = {};

    if (!open) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 380.f, 0.f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(370.f, io.DisplaySize.y), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 400.f), ImVec2(600.f, io.DisplaySize.y));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Admin Panel", &open, wf)) {
        ImGui::End();
        return;
    }

    // Capture window rect for decorations
    m_decorationRects.windowVisible = true;
    m_decorationRects.windowMin = ImGui::GetWindowPos();
    ImVec2 wSize = ImGui::GetWindowSize();
    m_decorationRects.windowMax = ImVec2(
        m_decorationRects.windowMin.x + wSize.x,
        m_decorationRects.windowMin.y + wSize.y);

    // FPS at top
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.f), "FPS: %.0f", b.fps);

    // Save / Load / Reset buttons
    {
        float btnW = 60.f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float totalW = btnW * 3 + spacing * 2;
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - totalW);

        ImGui::PushStyleColor(ImGuiCol_Button,        style::kSuccess);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  style::kSuccessHover);
        if (ImGui::Button("Save", ImVec2(btnW, 0)) && m_callbacks.onSave)
            m_callbacks.onSave();
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        style::kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  style::kAccentHover);
        if (ImGui::Button("Load", ImVec2(btnW, 0)) && m_callbacks.onLoad)
            m_callbacks.onLoad();
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        style::kDanger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  style::kDangerHover);
        if (ImGui::Button("Reset", ImVec2(btnW, 0)))
            m_showResetConfirm = true;
        ImGui::PopStyleColor(2);

        // Reset confirmation popup
        if (m_showResetConfirm) {
            ImGui::OpenPopup("Reset All?");
            m_showResetConfirm = false;
        }
        if (ImGui::BeginPopupModal("Reset All?", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Reset all settings to defaults?");
            ImGui::Text("Unsaved changes will be lost.");
            ImGui::Separator();
            if (ImGui::Button("Yes, Reset", ImVec2(120, 0))) {
                if (m_callbacks.onReset) m_callbacks.onReset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    trackedSeparator();

    // Tab bar — tab list is game-configured via b.tabNames
    int tabCount = static_cast<int>(b.tabNames.size());
    if (tabCount > 0 && ImGui::BeginTabBar("AdminTabs")) {
        m_decorationRects.tabBarMin = ImGui::GetItemRectMin();
        m_decorationRects.tabBarMax = ImGui::GetItemRectMax();

        for (int i = 0; i < tabCount; i++) {
            bool tabSelected = ImGui::BeginTabItem(b.tabNames[i]);
            // Track hovered tab (works for all tabs, not just selected)
            if (ImGui::IsItemHovered())
                m_decorationRects.hoveredTabIndex = i;

            if (tabSelected) {
                // Capture active tab rect
                m_decorationRects.activeTabIndex = i;
                m_decorationRects.activeTabMin = ImGui::GetItemRectMin();
                m_decorationRects.activeTabMax = ImGui::GetItemRectMax();

                // Tab transition detection
                if (m_lastActiveTab != i && m_lastActiveTab >= 0) {
                    m_decorationRects.previousTabIndex = m_lastActiveTab;
                    m_decorationRects.prevTabMin = m_decorationRects.activeTabMin;
                    m_decorationRects.prevTabMax = m_decorationRects.activeTabMax;
                    m_decorationRects.tabTransitionT = 0.0f;
                }
                m_lastActiveTab = i;

                // Render / Assets / Edit tabs get engine-drawn content
                // before extraTabUI.
                if (std::strcmp(b.tabNames[i], "Render") == 0)
                    drawTabRender(b);
                else if (std::strcmp(b.tabNames[i], "Assets") == 0)
                    drawTabAssets(b);
                else if (std::strcmp(b.tabNames[i], "Edit") == 0)
                    drawTabEdit(b);
                else
                    drawExtraTab(b.tabNames[i], b);

                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    // Tick tab transition animation
    {
        float dt = ImGui::GetIO().DeltaTime;
        float& t = m_decorationRects.tabTransitionT;
        if (t < 1.0f)
            t = std::min(1.0f, t + dt * 5.0f); // ~200ms transition
    }

    ImGui::End();
}

// ── TAB: RENDER ──────────────────────────────────────────────────────────────

void AdminPanel::drawTabRender(AdminBindings& b)
{
    // ── Performance (GPU timing + VRAM) ─────────────────────────
    if (trackedCollapsingHeader("Performance", m_renderPerf ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        m_renderPerf = true;
        if (b.gpuProfilingEnabled && b.gpuPassMs && b.gpuPassNames) {
            constexpr float kBudgetMs = 16.67f; // 60fps target
            constexpr float kSmooth   = 0.05f;  // EMA alpha (lower = smoother)
            constexpr float kLabelW   = 80.f;   // fixed label column width

            // Smooth pass timings (EMA)
            int count = (b.gpuTimerCount <= kMaxTimers) ? b.gpuTimerCount : kMaxTimers;
            float totalRaw = 0.f;
            for (int i = 0; i < count; i++) {
                float raw = b.gpuPassMs[i];
                totalRaw += raw;
                if (!m_smoothedInit)
                    m_smoothedPassMs[i] = raw;
                else
                    m_smoothedPassMs[i] += kSmooth * (raw - m_smoothedPassMs[i]);
            }
            if (!m_smoothedInit)
                m_smoothedPassMs[count] = totalRaw;
            else
                m_smoothedPassMs[count] += kSmooth * (totalRaw - m_smoothedPassMs[count]);
            m_smoothedInit = true;

            float barW = ImGui::GetContentRegionAvail().x - kLabelW;

            // Per-pass bars with label on the left
            for (int i = 0; i < count; i++) {
                float ms = m_smoothedPassMs[i];
                ImVec4 col = (ms < 2.f) ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                           : (ms < 5.f) ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                         : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
                ImGui::TextUnformatted(b.gpuPassNames[i]);
                ImGui::SameLine(kLabelW);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.2f ms", ms);
                ImGui::ProgressBar(ms / kBudgetMs, ImVec2(barW, 0), overlay);
                ImGui::PopStyleColor();
            }
            // Total
            {
                float ms = m_smoothedPassMs[count];
                ImVec4 col = (ms < 8.f)  ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                           : (ms < 16.f) ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                          : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
                ImGui::TextUnformatted("TOTAL");
                ImGui::SameLine(kLabelW);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.2f ms", ms);
                ImGui::ProgressBar(ms / kBudgetMs, ImVec2(barW, 0), overlay);
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
        } else {
            ImGui::TextDisabled("(F11 to enable GPU profiling)");
        }

        // VRAM bar
        if (b.vramBudgetMB > 0.f) {
            constexpr float kLabelW = 80.f;
            float barW = ImGui::GetContentRegionAvail().x - kLabelW;
            float pct = b.vramUsedMB / b.vramBudgetMB;
            ImVec4 vramCol = (pct < 0.7f) ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                           : (pct < 0.9f) ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                           : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
            ImGui::TextUnformatted("VRAM");
            ImGui::SameLine(kLabelW);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vramCol);
            char vramOverlay[48];
            snprintf(vramOverlay, sizeof(vramOverlay), "%.0f / %.0f MB (%.0f%%)",
                     b.vramUsedMB, b.vramBudgetMB, pct * 100.f);
            ImGui::ProgressBar(pct, ImVec2(barW, 0), vramOverlay);
            ImGui::PopStyleColor();
        }

        // Frame + Draw stats from PerformanceContext
        if (b.perfContext) {
            const auto& pc = *b.perfContext;
            ImGui::Spacing();
            ImGui::SeparatorText("Frame + Draw Stats");

            // Frame time vs budget
            {
                const float budget = (pc.budget.maxFrameMs > 0.f) ? pc.budget.maxFrameMs : 16.67f;
                const float ms     = pc.frameTimeMs;
                ImVec4 col = (ms < budget * 0.75f) ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                           : (ms < budget)         ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                                    : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
                char label[64];
                snprintf(label, sizeof(label), "%.2f / %.2f ms (%.0f fps)",
                         ms, budget, pc.avgFps);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                ImGui::ProgressBar(budget > 0.f ? ms / budget : 0.f, ImVec2(-1, 0), label);
                ImGui::PopStyleColor();
            }
            ImGui::Text("CPU: %.2f ms   GPU: %.2f ms",
                        pc.cpuFrameTimeMs, pc.gpuFrameTimeMs);

            // Draw calls
            {
                const uint32_t cur   = pc.drawCallCount;
                const uint32_t bmax  = pc.budget.maxDrawCalls > 0 ? pc.budget.maxDrawCalls : 10000;
                const float    pct   = static_cast<float>(cur) / static_cast<float>(bmax);
                ImVec4 col = (pct < 0.75f) ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                           : (pct < 1.f)   ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                            : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
                char label[64];
                snprintf(label, sizeof(label), "Draws: %u / %u", cur, bmax);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                ImGui::ProgressBar(pct, ImVec2(-1, 0), label);
                ImGui::PopStyleColor();
            }

            // Triangles
            {
                const uint32_t cur  = pc.triangleCount;
                const uint32_t bmax = pc.budget.maxTriangles > 0 ? pc.budget.maxTriangles : 5'000'000;
                const float    pct  = static_cast<float>(cur) / static_cast<float>(bmax);
                ImVec4 col = (pct < 0.75f) ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                           : (pct < 1.f)   ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                            : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
                char label[64];
                // Thousands separator via manual thousands format
                if (cur >= 1'000'000) {
                    snprintf(label, sizeof(label), "Tris: %.2fM / %.2fM",
                             cur / 1.0e6f, bmax / 1.0e6f);
                } else {
                    snprintf(label, sizeof(label), "Tris: %u / %u", cur, bmax);
                }
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                ImGui::ProgressBar(pct, ImVec2(-1, 0), label);
                ImGui::PopStyleColor();
            }

            // Network observability — 6 fields from
            // docs/NETWORK_DESIGN.md §7. Zero until networking wires
            // the fields via MsQuic + ReplicationRegistry.
            ImGui::Spacing();
            ImGui::SeparatorText("Network Stats (placeholders)");

            const auto& net = pc.network;
            const bool netIdle = (net.tickMs == 0.0f && net.bytesPerSec == 0 &&
                                  net.packetsPerSec == 0 && net.replicatedEntityCount == 0 &&
                                  net.ackLatencyMs == 0.0f && net.droppedDatagramPct == 0.0f);
            if (netIdle) {
                ImGui::TextDisabled("(no network session active — networking will populate)");
            } else {
                ImGui::Text("Tick:       %.2f ms  (budget %.2f ms)", net.tickMs, pc.budget.maxFrameMs * 2.f);
                ImGui::Text("Bandwidth:  %llu bytes/s",
                            static_cast<unsigned long long>(net.bytesPerSec));
                ImGui::Text("Packets:    %u pkts/s", net.packetsPerSec);
                ImGui::Text("Entities:   %u replicated", net.replicatedEntityCount);
                ImGui::Text("Ack RTT:    %.2f ms", net.ackLatencyMs);
                ImGui::Text("Dropped:    %.2f %%", net.droppedDatagramPct * 100.f);
            }
        }
    }

    // ── DLSS ────────────────────────────────────────────────────
    if (trackedCollapsingHeader("DLSS", m_renderDLSS ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        m_renderDLSS = true;
        if (b.dlssEnabled) {
            ImGui::BeginDisabled(!b.dlssAvailable);
            ImGui::Checkbox("DLSS SR", b.dlssEnabled);
            if (!b.dlssAvailable) { ImGui::SameLine(); ImGui::TextDisabled("(N/A)"); }
            ImGui::EndDisabled();
        }
        if (b.dlssQualityIndex && b.dlssEnabled) {
            ImGui::BeginDisabled(!b.dlssAvailable || !*b.dlssEnabled);
            static const char* dlssItems[] = { "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
            ImGui::Combo("Quality Mode", b.dlssQualityIndex, dlssItems, 5);
            helpMarker("DLAA=native AA, Quality=67%, Balanced=58%, Performance=50%, Ultra=33%");
            ImGui::EndDisabled();
        }
    }

    // ── Lighting ────────────────────────────────────────────────
    if (trackedCollapsingHeader("Lighting", m_renderLighting ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        m_renderLighting = true;
        if (b.restirEnabled) {
            ImGui::BeginDisabled(!b.rqAvailable);
            ImGui::Checkbox("ReSTIR DI", b.restirEnabled);
            if (!b.rqAvailable) { ImGui::SameLine(); ImGui::TextDisabled("(N/A)"); }
            ImGui::EndDisabled();
        }
        if (b.sharcEnabled) {
            ImGui::BeginDisabled(!b.rqAvailable);
            ImGui::Checkbox("SHaRC GI", b.sharcEnabled);
            if (!b.rqAvailable) { ImGui::SameLine(); ImGui::TextDisabled("(N/A)"); }
            ImGui::EndDisabled();
        }
        if (b.rtShadowManual) {
            ImGui::BeginDisabled(!b.rtAvailable);
            ImGui::Checkbox("Disable RT Shadows", b.rtShadowManual);
            helpMarker("Force rasterized shadows even on RT-capable GPUs");
            ImGui::EndDisabled();
        }
    }

    // ── SHaRC Tuning (advanced, collapsed by default) ───────────
    if (b.sharcLogBase && b.sharcEnabled) {
        bool sharcOn = *b.sharcEnabled;
        if (trackedCollapsingHeader("SHaRC Tuning", m_renderSharcTuning ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            m_renderSharcTuning = true;
            ImGui::BeginDisabled(!sharcOn);
            sliderResetFloat("Log Base",    b.sharcLogBase,    1.f, 4.f, 2.0f, "%.2f");
            helpMarker("Voxel grid resolution progression");
            sliderResetFloat("Level Bias",  b.sharcLevelBias,  -2.f, 2.f, 0.0f, "%.2f");
            helpMarker("Shifts quality level selection");
            if (b.sharcStaleFrameMax) {
                int stale = (int)*b.sharcStaleFrameMax;
                if (ImGui::SliderInt("Stale Frame Max", &stale, 16, 512)) {
                    *b.sharcStaleFrameMax = (uint32_t)stale;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("R##stale")) { *b.sharcStaleFrameMax = 128; }
                helpMarker("Frames before cache entry expires");
            }
            ImGui::EndDisabled();
        }
    }

    // ── System Health ────────────────────────────────────────────
    if (trackedCollapsingHeader("System Health", m_renderHealth ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        m_renderHealth = true;
        nlohmann::json health = SystemRegistry::get().serializeHealth();
        constexpr float kLabelW = 120.f;
        float barW = ImGui::GetContentRegionAvail().x - kLabelW;
        for (auto& [id, data] : health.items()) {
            float h = data.value("health", 0.f);
            bool active = data.value("active", false);
            ImVec4 col = !active       ? ImVec4(0.5f, 0.5f, 0.5f, 1.f)
                       : (h >= 80.f)   ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                       : (h >= 50.f)   ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                       : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
            ImGui::TextUnformatted(id.c_str());
            ImGui::SameLine(kLabelW);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            char overlay[32];
            if (active)
                snprintf(overlay, sizeof(overlay), "%.0f%%", h);
            else
                snprintf(overlay, sizeof(overlay), "inactive");
            ImGui::ProgressBar(h / 100.f, ImVec2(barW, 0), overlay);
            ImGui::PopStyleColor();
        }
        if (health.empty()) {
            ImGui::TextDisabled("(no systems registered)");
        }
    }

    // Extra render UI from DLL systems (post-process tuning, shadows, etc.)
    drawExtraTab("Render", b);
}

// ── TAB: ASSETS ──────────────────────────────────────────────────────────────

void AdminPanel::drawTabAssets(AdminBindings& b)
{
    AssetBrowser* browser = b.assetBrowser;
    if (!browser) {
        ImGui::TextDisabled("(no AssetBrowser bound)");
        drawExtraTab("Assets", b);
        return;
    }

    // ── Root + refresh row ──
    {
        const std::string& root = browser->rootDir();
        if (root.empty()) {
            ImGui::TextDisabled("(no scan root — call AssetBrowser::scan)");
        } else {
            ImGui::TextWrapped("Root: %s", root.c_str());
        }
        if (ImGui::SmallButton("Rescan")) {
            browser->rescan();
            m_assetsSelected = -1;
        }
        ImGui::SameLine();
        ImGui::Text("%zu file(s)", browser->size());
    }
    ImGui::Separator();

    // ── Filter row ──
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##assetFilter", "filter (name or path)",
                             m_assetsFilter, sizeof(m_assetsFilter));

    const char* kindLabels[] = {
        "All", "Mesh", "Texture", "Audio", "Shader",
        "Scene", "Material", "Animation", "Other"
    };
    int kindCombo = m_assetsKindFilter + 1; // 0 = All
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Combo("##assetKind", &kindCombo, kindLabels, IM_ARRAYSIZE(kindLabels))) {
        m_assetsKindFilter = kindCombo - 1;
        m_assetsSelected   = -1;
    }
    ImGui::Separator();

    // ── Filtered view ──
    std::vector<const AssetBrowserEntry*> view;
    {
        std::string_view needle(m_assetsFilter);
        if (m_assetsKindFilter < 0) {
            view = browser->filter(needle);
        } else {
            AssetKind k = static_cast<AssetKind>(m_assetsKindFilter);
            for (const auto* e : browser->entriesOfKind(k)) {
                bool match = needle.empty();
                if (!match) {
                    // substring match on name + relativePath
                    auto contains = [&](const std::string& s) {
                        if (needle.empty()) return true;
                        auto it = std::search(
                            s.begin(), s.end(),
                            needle.begin(), needle.end(),
                            [](char a, char b) {
                                return std::tolower(static_cast<unsigned char>(a)) ==
                                       std::tolower(static_cast<unsigned char>(b));
                            });
                        return it != s.end();
                    };
                    if (contains(e->name) || contains(e->relativePath)) match = true;
                }
                if (match) view.push_back(e);
            }
        }
    }

    // Clamp selection into range
    if (m_assetsSelected >= static_cast<int>(view.size())) m_assetsSelected = -1;

    // ── List ──
    ImVec2 listSize(0.f, ImGui::GetContentRegionAvail().y * 0.55f);
    if (ImGui::BeginListBox("##assetList", listSize)) {
        for (int i = 0; i < static_cast<int>(view.size()); ++i) {
            const AssetBrowserEntry* e = view[i];
            char row[320];
            snprintf(row, sizeof(row), "[%s] %s",
                     assetKindToString(e->kind), e->relativePath.c_str());
            bool selected = (m_assetsSelected == i);
            if (ImGui::Selectable(row, selected)) {
                m_assetsSelected = i;
            }
            // Drag source — payload is the asset's relativePath
            // (null-terminated). Game code registers drop targets via
            // ImGui::AcceptDragDropPayload("STRATUMV_ASSET_PATH", ...).
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                const std::string& rel = e->relativePath;
                ImGui::SetDragDropPayload("STRATUMV_ASSET_PATH",
                                          rel.c_str(),
                                          rel.size() + 1);
                ImGui::Text("[%s] %s",
                            assetKindToString(e->kind),
                            e->name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        if (view.empty()) {
            ImGui::TextDisabled("(no matches)");
        }
        ImGui::EndListBox();
    }

    ImGui::Separator();

    // ── Detail / import settings for selection ──
    if (m_assetsSelected >= 0 && m_assetsSelected < static_cast<int>(view.size())) {
        const AssetBrowserEntry* e = view[m_assetsSelected];
        ImGui::Text("Name:  %s", e->name.c_str());
        ImGui::Text("Path:  %s", e->relativePath.c_str());
        ImGui::Text("Kind:  %s", assetKindToString(e->kind));
        ImGui::Text("Size:  %.2f KB", e->sizeBytes / 1024.0);

        ImGui::Separator();
        ImGui::TextUnformatted("Import settings");

        // Opt-in write-through to <file>.meta.json.
        {
            bool autoSave = browser->autoSaveMeta();
            if (ImGui::Checkbox("Auto-save .meta.json", &autoSave)) {
                browser->setAutoSaveMeta(autoSave);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Save now")) {
                browser->saveMetaFile(e->relativePath);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reload")) {
                browser->loadMetaFile(e->relativePath);
            }
        }

        ImportSettings s = browser->getImportSettings(e->relativePath);
        bool changed = false;
        changed |= ImGui::SliderFloat("Scale", &s.scale, 0.01f, 100.f, "%.3f",
                                       ImGuiSliderFlags_Logarithmic);
        const char* axes[] = { "Y-up", "Z-up" };
        int axis = static_cast<int>(s.upAxis);
        if (ImGui::Combo("Up Axis", &axis, axes, IM_ARRAYSIZE(axes))) {
            s.upAxis = static_cast<ImportSettings::UpAxis>(axis);
            changed  = true;
        }
        char mapBuf[128];
        snprintf(mapBuf, sizeof(mapBuf), "%s", s.materialMapping.c_str());
        if (ImGui::InputText("Material map", mapBuf, sizeof(mapBuf))) {
            s.materialMapping = mapBuf;
            changed = true;
        }
        changed |= ImGui::Checkbox("Preload", &s.preload);

        if (changed) browser->setImportSettings(e->relativePath, s);
    } else {
        ImGui::TextDisabled("(select an asset)");
    }

    // Game-specific extras still run at the bottom.
    drawExtraTab("Assets", b);
}

} // namespace sv
