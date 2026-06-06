// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <cstdint>

namespace sv {

// Logical gameplay actions — abstracted from physical keys/buttons.
// Used by Input::isActionDown() and InputBindings for rebinding.
enum class Action : uint8_t {
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    Sprint,
    Crouch,
    Interact,
    Menu,
    AdminToggle,
    PlayerModeToggle,
    COUNT
};

// What physical input source an action can be bound to
enum class BindSource : uint8_t {
    None,
    Key,           // GLFW keyboard key code
    MouseButton,   // GLFW mouse button (0=left, 1=right, 2=middle)
    GamepadButton, // GLFW gamepad button
    GamepadAxis    // GLFW gamepad axis (sign determines direction)
};

// A single physical binding (one key, button, or axis direction)
struct InputBinding {
    BindSource source = BindSource::None;
    int        code   = 0;     // GLFW key/button/axis constant
    float      sign   = 1.0f;  // for GamepadAxis: +1.0 or -1.0 (which direction triggers)
};

// Full binding for one action: primary keyboard, alternate keyboard, gamepad
struct ActionBinding {
    InputBinding primary;    // default keyboard/mouse
    InputBinding alternate;  // alt keyboard/mouse
    InputBinding gamepad;    // gamepad button or axis
};

// String conversion (for JSON persistence)
const char* actionName(Action a);
Action      actionFromName(const char* name);

} // namespace sv
