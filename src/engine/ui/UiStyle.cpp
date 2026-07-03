// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif
#include "UiStyle.h"

#include "../CrtCompat.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>

namespace sv {
namespace style {

static ImFont* s_defaultFont = nullptr;
static std::string s_stylePath;
static std::string s_decorationsJSON = "{}";
static std::string s_backgroundJSON  = "{}";
static std::string s_glassJSON       = "{}";
static std::string s_atmosphereJSON  = "{}";
static uint32_t    s_styleVersion = 0;
static bool        s_saveSuppressed = false;  // skip next reload after our own write

// ── Helpers for JSON parsing ─────────────────────────────────────────────────

static ImVec4 getVec4(const nlohmann::json& j, const char* key, const ImVec4& def)
{
    if (!j.contains(key) || !j[key].is_array()) return def;
    auto& a = j[key];
    return ImVec4(
        a.size() > 0 ? a[0].get<float>() : def.x,
        a.size() > 1 ? a[1].get<float>() : def.y,
        a.size() > 2 ? a[2].get<float>() : def.z,
        a.size() > 3 ? a[3].get<float>() : def.w
    );
}

static ImVec2 getVec2(const nlohmann::json& j, const char* key, const ImVec2& def)
{
    if (!j.contains(key) || !j[key].is_array()) return def;
    auto& a = j[key];
    return ImVec2(
        a.size() > 0 ? a[0].get<float>() : def.x,
        a.size() > 1 ? a[1].get<float>() : def.y
    );
}

static float getFloat(const nlohmann::json& j, const char* key, float def)
{
    if (!j.contains(key)) return def;
    return j[key].get<float>();
}

// ── Core style application (compiled defaults) ───────────────────────────────

void applyDefaultStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // ── Geometry ─────────────────────────────────────────────────────────────
    s.WindowRounding    = 3.f;
    s.FrameRounding     = 2.f;
    s.GrabRounding      = 2.f;
    s.TabRounding       = 2.f;
    s.ScrollbarRounding = 2.f;
    s.ChildRounding     = 2.f;
    s.PopupRounding     = 3.f;

    s.WindowBorderSize  = 1.f;
    s.FrameBorderSize   = 0.f;
    s.PopupBorderSize   = 1.f;
    s.TabBorderSize     = 0.f;

    s.WindowPadding     = ImVec2(8.f, 8.f);
    s.FramePadding      = ImVec2(6.f, 3.f);
    s.ItemSpacing       = ImVec2(8.f, 5.f);
    s.ItemInnerSpacing  = ImVec2(5.f, 4.f);
    s.IndentSpacing     = 18.f;

    s.ScrollbarSize     = 12.f;
    s.GrabMinSize       = 8.f;

    // ── Colors ───────────────────────────────────────────────────────────────
    ImVec4* c = s.Colors;

    // Text
    c[ImGuiCol_Text]                 = kText;
    c[ImGuiCol_TextDisabled]         = kTextDim;

    // Backgrounds
    c[ImGuiCol_WindowBg]             = kWindowBg;
    c[ImGuiCol_ChildBg]              = kChildBg;
    c[ImGuiCol_PopupBg]              = kPopupBg;

    // Borders
    c[ImGuiCol_Border]               = kBorder;
    c[ImGuiCol_BorderShadow]         = ImVec4(0.f, 0.f, 0.f, 0.f);

    // Frame (sliders, inputs)
    c[ImGuiCol_FrameBg]              = kFrameBg;
    c[ImGuiCol_FrameBgHovered]       = kFrameBgHover;
    c[ImGuiCol_FrameBgActive]        = kFrameBgActive;

    // Title bar
    c[ImGuiCol_TitleBg]              = kTitleBg;
    c[ImGuiCol_TitleBgActive]        = kTitleBgActive;
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.03f, 0.04f, 0.06f, 0.60f);

    // Menu bar
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.05f, 0.06f, 0.09f, 1.00f);

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.03f, 0.04f, 0.06f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.10f, 0.18f, 0.25f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.12f, 0.24f, 0.32f, 0.90f);
    c[ImGuiCol_ScrollbarGrabActive]  = kAccentDim;

    // Checkmark, slider grab
    c[ImGuiCol_CheckMark]            = kAccent;
    c[ImGuiCol_SliderGrab]           = kAccentDim;
    c[ImGuiCol_SliderGrabActive]     = kAccent;

    // Buttons
    c[ImGuiCol_Button]               = kButton;
    c[ImGuiCol_ButtonHovered]        = kButtonHover;
    c[ImGuiCol_ButtonActive]         = kButtonActive;

    // Headers (collapsing headers, selectable)
    c[ImGuiCol_Header]               = kHeader;
    c[ImGuiCol_HeaderHovered]        = kHeaderHover;
    c[ImGuiCol_HeaderActive]         = kHeaderActive;

    // Separators
    c[ImGuiCol_Separator]            = kSeparator;
    c[ImGuiCol_SeparatorHovered]     = kAccentDim;
    c[ImGuiCol_SeparatorActive]      = kAccent;

    // Resize grip
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.00f, 0.55f, 0.65f, 0.15f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.00f, 0.65f, 0.75f, 0.40f);
    c[ImGuiCol_ResizeGripActive]     = kAccent;

    // Tabs
    c[ImGuiCol_Tab]                  = kTab;
    c[ImGuiCol_TabHovered]           = kTabHover;
    c[ImGuiCol_TabSelected]          = kTabActive;
    c[ImGuiCol_TabDimmed]            = ImVec4(0.04f, 0.08f, 0.12f, 0.80f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.06f, 0.16f, 0.22f, 1.00f);

    // Tables
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.06f, 0.10f, 0.16f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = kBorder;
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.00f, 0.40f, 0.50f, 0.15f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.04f, 0.06f, 0.10f, 0.40f);

    // Nav
    c[ImGuiCol_NavCursor]            = kAccent;
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.00f, 0.75f, 0.85f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.10f, 0.10f, 0.12f, 0.50f);

    // Modal dim
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.02f, 0.03f, 0.06f, 0.60f);

    // ── Font (only load once — can't rebuild atlas after init) ────────────────
    if (!s_defaultFont) {
        ImGuiIO& io = ImGui::GetIO();

        const char* fontPaths[] = {
            "data/fonts/Roboto-Medium.ttf",
            "build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf",
        };

        for (const char* path : fontPaths) {
            FILE* f = sv::FOpen(path, "rb");
            if (f) {
                fclose(f);
                s_defaultFont = io.Fonts->AddFontFromFileTTF(path, 15.0f);
                if (s_defaultFont) {
                    printf("[Style] Loaded font: %s (15px)\n", path);
                    return;
                }
            }
        }

        printf("[Style] Font not found, using ImGui default\n");
    }
}

// ── JSON style loading ───────────────────────────────────────────────────────

static void applyJSONColors(const nlohmann::json& colors)
{
    ImVec4* c = ImGui::GetStyle().Colors;

    // Map JSON keys → ImGui color indices (only override if present)
    struct ColorMapping { const char* key; int idx; ImVec4 def; };
    const ColorMapping mappings[] = {
        {"windowBg",      ImGuiCol_WindowBg,             kWindowBg},
        {"childBg",       ImGuiCol_ChildBg,              kChildBg},
        {"popupBg",       ImGuiCol_PopupBg,              kPopupBg},
        {"frameBg",       ImGuiCol_FrameBg,              kFrameBg},
        {"frameBgHover",  ImGuiCol_FrameBgHovered,       kFrameBgHover},
        {"frameBgActive", ImGuiCol_FrameBgActive,        kFrameBgActive},
        {"text",          ImGuiCol_Text,                 kText},
        {"textDim",       ImGuiCol_TextDisabled,         kTextDim},
        {"border",        ImGuiCol_Border,               kBorder},
        {"separator",     ImGuiCol_Separator,            kSeparator},
        {"header",        ImGuiCol_Header,               kHeader},
        {"headerHover",   ImGuiCol_HeaderHovered,        kHeaderHover},
        {"headerActive",  ImGuiCol_HeaderActive,         kHeaderActive},
        {"tab",           ImGuiCol_Tab,                  kTab},
        {"tabHover",      ImGuiCol_TabHovered,           kTabHover},
        {"tabActive",     ImGuiCol_TabSelected,          kTabActive},
        {"button",        ImGuiCol_Button,               kButton},
        {"buttonHover",   ImGuiCol_ButtonHovered,        kButtonHover},
        {"buttonActive",  ImGuiCol_ButtonActive,         kButtonActive},
        {"titleBg",       ImGuiCol_TitleBg,              kTitleBg},
        {"titleBgActive", ImGuiCol_TitleBgActive,        kTitleBgActive},
        {"accent",        ImGuiCol_CheckMark,            kAccent},
        {"accentDim",     ImGuiCol_SliderGrab,           kAccentDim},
    };

    for (auto& m : mappings) {
        if (colors.contains(m.key))
            c[m.idx] = getVec4(colors, m.key, m.def);
    }

    // Accent also drives several other colors
    if (colors.contains("accent")) {
        ImVec4 a = getVec4(colors, "accent", kAccent);
        c[ImGuiCol_CheckMark]         = a;
        c[ImGuiCol_SliderGrabActive]  = a;
        c[ImGuiCol_ResizeGripActive]  = a;
        c[ImGuiCol_SeparatorActive]   = a;
        c[ImGuiCol_NavCursor]         = a;
    }
    if (colors.contains("accentDim")) {
        ImVec4 ad = getVec4(colors, "accentDim", kAccentDim);
        c[ImGuiCol_SliderGrab]          = ad;
        c[ImGuiCol_ScrollbarGrabActive] = ad;
        c[ImGuiCol_SeparatorHovered]    = ad;
    }
}

static void applyJSONGeometry(const nlohmann::json& geo)
{
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = getFloat(geo, "windowRounding",    s.WindowRounding);
    s.FrameRounding     = getFloat(geo, "frameRounding",     s.FrameRounding);
    s.GrabRounding      = getFloat(geo, "grabRounding",      s.GrabRounding);
    s.TabRounding       = getFloat(geo, "tabRounding",       s.TabRounding);
    s.ScrollbarRounding = getFloat(geo, "scrollbarRounding", s.ScrollbarRounding);
    s.ChildRounding     = getFloat(geo, "childRounding",     s.ChildRounding);
    s.PopupRounding     = getFloat(geo, "popupRounding",     s.PopupRounding);

    s.WindowBorderSize  = getFloat(geo, "windowBorderSize",  s.WindowBorderSize);
    s.FrameBorderSize   = getFloat(geo, "frameBorderSize",   s.FrameBorderSize);
    s.PopupBorderSize   = getFloat(geo, "popupBorderSize",   s.PopupBorderSize);
    s.TabBorderSize     = getFloat(geo, "tabBorderSize",     s.TabBorderSize);

    s.WindowPadding     = getVec2(geo, "windowPadding",      s.WindowPadding);
    s.FramePadding      = getVec2(geo, "framePadding",       s.FramePadding);
    s.ItemSpacing       = getVec2(geo, "itemSpacing",        s.ItemSpacing);
    s.ItemInnerSpacing  = getVec2(geo, "itemInnerSpacing",   s.ItemInnerSpacing);
    s.IndentSpacing     = getFloat(geo, "indentSpacing",     s.IndentSpacing);
    s.ScrollbarSize     = getFloat(geo, "scrollbarSize",     s.ScrollbarSize);
    s.GrabMinSize       = getFloat(geo, "grabMinSize",       s.GrabMinSize);
}

bool loadStyleFromJSON(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        printf("[Style] ui_style.json not found at %s, using compiled defaults\n", path.c_str());
        return false;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        printf("[Style] ui_style.json parse error: %s\n", e.what());
        return false;
    }

    s_stylePath = path;

    // Re-apply compiled defaults first, then overlay JSON values
    applyDefaultStyle();

    if (root.contains("colors") && root["colors"].is_object())
        applyJSONColors(root["colors"]);

    if (root.contains("geometry") && root["geometry"].is_object())
        applyJSONGeometry(root["geometry"]);

    // Cache decorations section as string for DLL consumption
    if (root.contains("decorations") && root["decorations"].is_object())
        s_decorationsJSON = root["decorations"].dump();
    else
        s_decorationsJSON = "{}";

    // Cache background section as string for DLL consumption
    if (root.contains("background") && root["background"].is_object())
        s_backgroundJSON = root["background"].dump();
    else
        s_backgroundJSON = "{}";

    // Cache glass section as string for DLL consumption
    if (root.contains("glass") && root["glass"].is_object())
        s_glassJSON = root["glass"].dump();
    else
        s_glassJSON = "{}";

    // Cache atmosphere section as string for DLL consumption
    if (root.contains("atmosphere") && root["atmosphere"].is_object())
        s_atmosphereJSON = root["atmosphere"].dump();
    else
        s_atmosphereJSON = "{}";

    ++s_styleVersion;
    printf("[Style] Style loaded from %s (v%u)\n", path.c_str(), s_styleVersion);
    return true;
}

void reloadStyle()
{
    if (s_stylePath.empty()) return;
    if (s_saveSuppressed) {
        s_saveSuppressed = false;
        return;  // skip reload triggered by our own saveStyleSection() write
    }
    printf("[Style] Reloading style from %s\n", s_stylePath.c_str());
    loadStyleFromJSON(s_stylePath);
}

const char* getDecorationsJSON()
{
    return s_decorationsJSON.c_str();
}

const char* getBackgroundJSON()
{
    return s_backgroundJSON.c_str();
}

const char* getGlassJSON()
{
    return s_glassJSON.c_str();
}

const char* getAtmosphereJSON()
{
    return s_atmosphereJSON.c_str();
}

uint32_t getStyleVersion()
{
    return s_styleVersion;
}

ImFont* getDefaultFont()
{
    return s_defaultFont;
}

void saveStyleSection(const char* section, const char* jsonStr)
{
    if (s_stylePath.empty() || !section || !jsonStr) return;

    // Read current file
    nlohmann::json root;
    {
        std::ifstream file(s_stylePath);
        if (file.is_open()) {
            try { root = nlohmann::json::parse(file); }
            catch (...) { root = nlohmann::json::object(); }
        }
    }

    // Merge the section
    try {
        root[section] = nlohmann::json::parse(jsonStr);
    } catch (const nlohmann::json::exception& e) {
        printf("[Style] saveStyleSection: JSON parse error for '%s': %s\n", section, e.what());
        return;
    }

    // Update cached JSON strings so other systems see the new values immediately
    const std::string key(section);
    if (key == "background")   s_backgroundJSON   = root[section].dump();
    else if (key == "glass")   s_glassJSON         = root[section].dump();
    else if (key == "atmosphere") s_atmosphereJSON  = root[section].dump();
    else if (key == "decorations") s_decorationsJSON = root[section].dump();

    ++s_styleVersion;

    // Write to disk (suppress the AssetWatcher reload this triggers)
    s_saveSuppressed = true;
    std::ofstream out(s_stylePath);
    if (out.is_open()) {
        out << root.dump(2);
        printf("[Style] Saved style section '%s' to %s (v%u)\n", section, s_stylePath.c_str(), s_styleVersion);
    } else {
        printf("[Style] saveStyleSection: failed to open %s for writing\n", s_stylePath.c_str());
        s_saveSuppressed = false;
    }
}

} // namespace style
} // namespace sv
