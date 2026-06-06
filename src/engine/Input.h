// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "InputAction.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace sv {

class InputBindings;

class Input {
public:
    void init(GLFWwindow* window);

    // Call once per frame after glfwPollEvents()
    void update();

    bool isKeyDown(int key) const;       // held
    bool isKeyPressed(int key) const;    // only on the frame it was first pressed
    bool isMouseDown(int button) const;

    glm::vec2 mousePos()    const { return m_mousePos; }
    glm::vec2 mouseDelta()  const { return m_mouseDelta; }
    float     scrollDelta() const { return m_scrollDelta; }

    void setCursorLocked(bool locked);
    bool isCursorLocked() const { return m_cursorLocked; }

    void beginMouseSuppression(float seconds = 0.2f);

    // ── Action queries (use InputBindings for remapping) ─────────
    void setBindings(InputBindings* bindings) { m_bindings = bindings; }
    bool  isActionDown(Action a) const;
    bool  isActionPressed(Action a) const;
    float actionAxis(Action a) const;   // 0.0..1.0 magnitude (analog stick or binary 0/1)

    // ── Gamepad state ────────────────────────────────────────────
    struct GamepadState {
        bool  connected = false;
        int   jid       = -1;
        char  name[128] = {};

        bool  buttons    [GLFW_GAMEPAD_BUTTON_LAST + 1] = {};
        bool  buttonsPrev[GLFW_GAMEPAD_BUTTON_LAST + 1] = {};
        float axes       [GLFW_GAMEPAD_AXIS_LAST + 1]   = {};

        // Deadzone-filtered sticks (range: -1..+1)
        float stickLeftX  = 0.f, stickLeftY  = 0.f;
        float stickRightX = 0.f, stickRightY = 0.f;
        // Triggers remapped to 0..1
        float triggerLeft  = 0.f, triggerRight = 0.f;
    };

    const GamepadState& gamepad() const { return m_gamepad; }
    bool isGamepadConnected() const { return m_gamepad.connected; }
    bool isGamepadButtonDown(int btn) const;
    bool isGamepadButtonPressed(int btn) const;
    float gamepadAxis(int axis) const;

    void setStickDeadzone(float dz)   { m_stickDeadzone = dz; }
    void setTriggerDeadzone(float dz) { m_triggerDeadzone = dz; }

    // ── Remote input injection (DevServer) ───────────────────────
    void injectKey(int key);
    void injectMouseDelta(float dx, float dy);
    void injectScroll(float delta);
    void injectMouseClick(float x, float y, int button = 0); // 0=left, 1=right
    void injectGamepadButton(int btn);
    void injectGamepadAxis(int axis, float value);

private:
    // Evaluate a single InputBinding against current state
    bool  isBindingDown(const InputBinding& b) const;
    bool  isBindingPressed(const InputBinding& b) const;
    float bindingAxis(const InputBinding& b) const;

    // Gamepad polling
    void updateGamepad();

    // Injection buffers — merged with GLFW state in update(), auto-cleared
    bool      m_injectedKeys[GLFW_KEY_LAST + 1] = {};
    glm::vec2 m_injectedMouseDelta{0.f};
    float     m_injectedScroll = 0.f;
    bool      m_hasInjectedInput = false;

    // Gamepad injection
    bool      m_injectedGpButtons[GLFW_GAMEPAD_BUTTON_LAST + 1] = {};
    float     m_injectedGpAxes[GLFW_GAMEPAD_AXIS_LAST + 1] = {};
    bool      m_hasInjectedGpButtons = false;
    bool      m_hasInjectedGpAxes    = false;

    // Mouse click injection (position + button, consumed over 2 frames: press then release)
    bool      m_injectedClick       = false;
    bool      m_injectedClickActive = false;  // true = button held this frame
    float     m_injectedClickX      = 0.f;
    float     m_injectedClickY      = 0.f;
    int       m_injectedClickButton = 0;
    int       m_injectedClickFrames = 0;      // countdown: 2=press, 1=release, 0=done

    GLFWwindow* m_window      = nullptr;
    glm::vec2   m_mousePos    {0.f};
    glm::vec2   m_lastMousePos{0.f};
    glm::vec2   m_mouseDelta  {0.f};
    float       m_scrollDelta = 0.f;
    float       m_scrollAccum = 0.f;   // raw callback accumulator
    bool        m_cursorLocked = false;
    bool        m_firstMouse   = true;
    float       m_mouseSuppressionTimer    = 0.f;
    float       m_mouseSuppressionDuration = 0.f;
    double      m_mouseSuppressionStart    = 0.0;

    bool m_keys    [GLFW_KEY_LAST + 1] = {};
    bool m_keysPrev[GLFW_KEY_LAST + 1] = {};

    // Gamepad
    GamepadState m_gamepad;
    float        m_stickDeadzone  = 0.15f;
    float        m_triggerDeadzone = 0.05f;

    // Bindings (non-owning, set by Engine)
    InputBindings* m_bindings = nullptr;

    static void scrollCallback(GLFWwindow* w, double xoff, double yoff);
};

} // namespace sv
