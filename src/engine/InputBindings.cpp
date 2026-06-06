// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "InputBindings.h"
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>

namespace sv {

// Helper: make a keyboard binding
static InputBinding key(int code) { return { BindSource::Key, code, 1.0f }; }
// Helper: make a gamepad button binding
static InputBinding gpBtn(int code) { return { BindSource::GamepadButton, code, 1.0f }; }
// Helper: make a gamepad axis binding
static InputBinding gpAxis(int code, float sign) { return { BindSource::GamepadAxis, code, sign }; }

void InputBindings::loadDefaults()
{
    // MoveForward: W, gamepad left stick Y negative
    m_bindings[(size_t)Action::MoveForward]  = { key(GLFW_KEY_W), {}, gpAxis(GLFW_GAMEPAD_AXIS_LEFT_Y, -1.0f) };
    // MoveBack: S, gamepad left stick Y positive
    m_bindings[(size_t)Action::MoveBack]     = { key(GLFW_KEY_S), {}, gpAxis(GLFW_GAMEPAD_AXIS_LEFT_Y, +1.0f) };
    // MoveLeft: A, gamepad left stick X negative
    m_bindings[(size_t)Action::MoveLeft]     = { key(GLFW_KEY_A), {}, gpAxis(GLFW_GAMEPAD_AXIS_LEFT_X, -1.0f) };
    // MoveRight: D, gamepad left stick X positive
    m_bindings[(size_t)Action::MoveRight]    = { key(GLFW_KEY_D), {}, gpAxis(GLFW_GAMEPAD_AXIS_LEFT_X, +1.0f) };
    // Jump: Space / A button
    m_bindings[(size_t)Action::Jump]         = { key(GLFW_KEY_SPACE), {}, gpBtn(GLFW_GAMEPAD_BUTTON_A) };
    // Sprint: Left Shift / Left bumper
    m_bindings[(size_t)Action::Sprint]       = { key(GLFW_KEY_LEFT_SHIFT), {}, gpBtn(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER) };
    // Crouch: Left Ctrl / B button
    m_bindings[(size_t)Action::Crouch]       = { key(GLFW_KEY_LEFT_CONTROL), {}, gpBtn(GLFW_GAMEPAD_BUTTON_B) };
    // Interact: E / X button
    m_bindings[(size_t)Action::Interact]     = { key(GLFW_KEY_E), {}, gpBtn(GLFW_GAMEPAD_BUTTON_X) };
    // Menu: Escape / Start
    m_bindings[(size_t)Action::Menu]         = { key(GLFW_KEY_ESCAPE), {}, gpBtn(GLFW_GAMEPAD_BUTTON_START) };
    // Admin toggle: F2 (no gamepad)
    m_bindings[(size_t)Action::AdminToggle]  = { key(GLFW_KEY_F2), {}, {} };
    // Player mode toggle: F1 (no gamepad)
    m_bindings[(size_t)Action::PlayerModeToggle] = { key(GLFW_KEY_F1), {}, {} };
}

// ── JSON helpers ─────────────────────────────────────────────────

static const char* sourceToStr(BindSource s) {
    switch (s) {
        case BindSource::Key:           return "key";
        case BindSource::MouseButton:   return "mouse";
        case BindSource::GamepadButton: return "gamepad_button";
        case BindSource::GamepadAxis:   return "gamepad_axis";
        default:                        return "none";
    }
}

static BindSource sourceFromStr(const std::string& s) {
    if (s == "key")            return BindSource::Key;
    if (s == "mouse")          return BindSource::MouseButton;
    if (s == "gamepad_button") return BindSource::GamepadButton;
    if (s == "gamepad_axis")   return BindSource::GamepadAxis;
    return BindSource::None;
}

static nlohmann::json bindingToJson(const InputBinding& b) {
    if (b.source == BindSource::None) return nullptr;
    nlohmann::json j;
    j["source"] = sourceToStr(b.source);
    j["code"]   = b.code;
    if (b.source == BindSource::GamepadAxis) j["sign"] = b.sign;
    return j;
}

static InputBinding bindingFromJson(const nlohmann::json& j) {
    if (j.is_null()) return {};
    InputBinding b;
    b.source = sourceFromStr(j.value("source", "none"));
    b.code   = j.value("code", 0);
    b.sign   = j.value("sign", 1.0f);
    return b;
}

bool InputBindings::loadFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;

    try {
        nlohmann::json root;
        f >> root;
        for (auto& [name, obj] : root.items()) {
            Action a = actionFromName(name.c_str());
            if (a == Action::COUNT) continue;
            ActionBinding& ab = m_bindings[(size_t)a];
            if (obj.contains("primary"))   ab.primary   = bindingFromJson(obj["primary"]);
            if (obj.contains("alternate")) ab.alternate = bindingFromJson(obj["alternate"]);
            if (obj.contains("gamepad"))   ab.gamepad   = bindingFromJson(obj["gamepad"]);
        }
        return true;
    } catch (...) {
        fprintf(stderr, "[InputBindings] Failed to parse %s\n", path.c_str());
        return false;
    }
}

bool InputBindings::saveToFile(const std::string& path) const
{
    nlohmann::json root;
    for (size_t i = 0; i < (size_t)Action::COUNT; i++) {
        const auto& ab = m_bindings[i];
        nlohmann::json obj;
        auto p = bindingToJson(ab.primary);
        auto a = bindingToJson(ab.alternate);
        auto g = bindingToJson(ab.gamepad);
        if (!p.is_null()) obj["primary"]   = p;
        if (!a.is_null()) obj["alternate"] = a;
        if (!g.is_null()) obj["gamepad"]   = g;
        root[actionName((Action)i)] = obj;
    }

    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << root.dump(2);
    return true;
}

} // namespace sv
