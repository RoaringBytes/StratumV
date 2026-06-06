// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <imgui.h>
#include <string>

namespace sv {
namespace style {

// ── Background tiers ─────────────────────────────────────────────────────────
constexpr ImVec4 kWindowBg      {0.04f, 0.05f, 0.08f, 0.96f};
constexpr ImVec4 kChildBg       {0.06f, 0.07f, 0.10f, 0.90f};
constexpr ImVec4 kPopupBg       {0.06f, 0.07f, 0.11f, 0.96f};
constexpr ImVec4 kFrameBg       {0.08f, 0.10f, 0.16f, 0.80f};
constexpr ImVec4 kFrameBgHover  {0.10f, 0.14f, 0.22f, 0.90f};
constexpr ImVec4 kFrameBgActive {0.12f, 0.18f, 0.28f, 1.00f};

// ── Primary accent (cyan / teal) ─────────────────────────────────────────────
constexpr ImVec4 kAccent        {0.00f, 0.75f, 0.85f, 1.00f};
constexpr ImVec4 kAccentHover   {0.00f, 0.85f, 0.95f, 1.00f};
constexpr ImVec4 kAccentActive  {0.10f, 0.95f, 1.00f, 1.00f};
constexpr ImVec4 kAccentDim     {0.00f, 0.55f, 0.65f, 0.60f};

// ── Secondary accent (gold) ──────────────────────────────────────────────────
constexpr ImVec4 kGold          {0.85f, 0.65f, 0.15f, 1.00f};
constexpr ImVec4 kGoldHover     {0.95f, 0.75f, 0.25f, 1.00f};

// ── Semantic ─────────────────────────────────────────────────────────────────
constexpr ImVec4 kSuccess       {0.15f, 0.65f, 0.25f, 0.85f};
constexpr ImVec4 kSuccessHover  {0.20f, 0.78f, 0.32f, 0.95f};
constexpr ImVec4 kDanger        {0.70f, 0.18f, 0.18f, 0.85f};
constexpr ImVec4 kDangerHover   {0.85f, 0.25f, 0.25f, 0.95f};
constexpr ImVec4 kWarning       {0.85f, 0.65f, 0.15f, 0.85f};

// ── Inline reset buttons (dark red, used by slider/color helpers) ────────────
constexpr ImVec4 kResetBtn      {0.35f, 0.12f, 0.12f, 0.70f};
constexpr ImVec4 kResetBtnHover {0.55f, 0.18f, 0.18f, 0.90f};

// ── Text ─────────────────────────────────────────────────────────────────────
constexpr ImVec4 kText          {0.85f, 0.88f, 0.92f, 1.00f};
constexpr ImVec4 kTextDim       {0.45f, 0.50f, 0.58f, 1.00f};

// ── Header / tab tints (dark teal) ──────────────────────────────────────────
constexpr ImVec4 kHeader        {0.06f, 0.16f, 0.22f, 0.80f};
constexpr ImVec4 kHeaderHover   {0.08f, 0.24f, 0.32f, 0.90f};
constexpr ImVec4 kHeaderActive  {0.10f, 0.30f, 0.40f, 1.00f};

constexpr ImVec4 kTab           {0.06f, 0.12f, 0.18f, 0.90f};
constexpr ImVec4 kTabHover      {0.08f, 0.22f, 0.32f, 0.90f};
constexpr ImVec4 kTabActive     {0.05f, 0.28f, 0.38f, 1.00f};

// ── Borders ──────────────────────────────────────────────────────────────────
constexpr ImVec4 kBorder        {0.00f, 0.55f, 0.65f, 0.25f};
constexpr ImVec4 kSeparator     {0.00f, 0.45f, 0.55f, 0.30f};

// ── Button (default, matches headers) ────────────────────────────────────────
constexpr ImVec4 kButton        {0.08f, 0.18f, 0.26f, 0.80f};
constexpr ImVec4 kButtonHover   {0.10f, 0.26f, 0.36f, 0.90f};
constexpr ImVec4 kButtonActive  {0.12f, 0.32f, 0.44f, 1.00f};

// ── Title bar ────────────────────────────────────────────────────────────────
constexpr ImVec4 kTitleBg       {0.03f, 0.04f, 0.07f, 1.00f};
constexpr ImVec4 kTitleBgActive {0.05f, 0.10f, 0.18f, 1.00f};

// ── API ──────────────────────────────────────────────────────────────────────

// Apply the default engine theme (colors, geometry, font).
// Call once after ImGui::CreateContext(), before backend font upload.
void applyDefaultStyle();

// Load style overrides from JSON file. Missing keys keep compiled defaults.
// Returns true if file loaded successfully.
bool loadStyleFromJSON(const std::string& path);

// Re-apply style from the last loaded JSON path (AssetWatcher callback).
void reloadStyle();

// Returns the "decorations" section of ui_style.json as a C-string (JSON).
// Safe to pass across DLL boundary. Returns "{}" if not loaded.
const char* getDecorationsJSON();

// Returns the "background" section of ui_style.json as a C-string (JSON).
// Safe to pass across DLL boundary. Returns "{}" if not loaded.
const char* getBackgroundJSON();

// Returns the "glass" section of ui_style.json as a C-string (JSON).
// Safe to pass across DLL boundary. Returns "{}" if not loaded.
const char* getGlassJSON();

// Returns the "atmosphere" section of ui_style.json as a C-string (JSON).
// Safe to pass across DLL boundary. Returns "{}" if not loaded.
const char* getAtmosphereJSON();

// Save a single section back to ui_style.json (merge + write).
// Suppresses the next AssetWatcher reload to prevent save→reload loop.
// Thread-safe: called from DLL drawUI on the main thread.
void saveStyleSection(const char* section, const char* jsonStr);

// Monotonic version counter, incremented on each style reload.
uint32_t getStyleVersion();

// Returns the loaded Roboto font, or nullptr if fallback to default.
ImFont* getDefaultFont();

} // namespace style
} // namespace sv
