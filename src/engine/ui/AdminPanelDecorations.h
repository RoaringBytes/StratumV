// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <imgui.h>

namespace sv {

// POD struct passed across DLL boundary — no virtuals, no std::string, fixed arrays.
// Engine-side AdminPanel populates this each frame; DLL-side reads it for decoration rendering.
struct AdminDecorationRects {
    ImVec2 windowMin{}, windowMax{};
    bool   windowVisible = false;

    ImVec2 tabBarMin{}, tabBarMax{};
    int    activeTabIndex = -1;
    ImVec2 activeTabMin{}, activeTabMax{};

    static constexpr int kMaxHeaders = 16;
    struct HeaderRect { ImVec2 min{}, max{}; bool open = false; };
    HeaderRect headers[kMaxHeaders]{};
    int headerCount = 0;

    static constexpr int kMaxSeparators = 16;
    struct SepRect { ImVec2 left{}, right{}; };
    SepRect separators[kMaxSeparators]{};
    int separatorCount = 0;

    // ── Interaction state (populated by AdminPanel::draw each frame) ──
    int    hoveredHeaderIndex = -1;       // which header is hovered (-1 = none)
    int    hoveredTabIndex    = -1;       // which tab is hovered (-1 = none)

    // Tab transition animation
    int    previousTabIndex   = -1;       // tab before current switch
    ImVec2 prevTabMin{}, prevTabMax{};    // previous tab rect (for fade-out)
    float  tabTransitionT     = 1.0f;     // 0→1 animation progress (1 = complete)

    // Per-header animation progress
    float  headerHoverT[kMaxHeaders]{};   // 0 = idle, 1 = fully hovered
    float  headerOpenT[kMaxHeaders]{};    // 0 = collapsed, 1 = open (animated)
};

} // namespace sv
