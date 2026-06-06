// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once
#include <cstdint>

namespace sv {

struct NodeMeta {
    const char* id;           // Unique node identifier
    const char* label;        // Display name
    const char* icon;         // Emoji icon
    int         layer;        // 0-6 = engine pipeline, 8 = DLL plugin
    const char* color;        // Hex color string e.g. "#7eb8ff"
    const char* simple;       // One-sentence user-friendly description
    const char* detail;       // Technical detail string
    const char* files;        // Comma-separated source file paths
    const char* constraints;  // Newline-separated constraint strings (may be nullptr)
    float       budgetMs;     // Frame time budget in ms (0 = no target)
    const char* lastTouched;  // Session ID of the last change
    const char* dependencies; // Comma-separated node IDs this system depends on (may be nullptr)
};

struct HealthReport {
    float health;   // 0-100
    bool  active;   // Is this system currently doing work?
};

class EngineSystem {
public:
    virtual ~EngineSystem() = default;
    virtual NodeMeta     getMeta()   const = 0;
    virtual HealthReport getHealth() const = 0;
};

} // namespace sv
