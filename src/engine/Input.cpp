// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "Input.h"
#include "InputBindings.h"
#include "CrtCompat.h"
#include <imgui.h>
#include <cstring>
#include <cmath>

namespace sv {

// Per-process singleton so the static scroll callback can reach the instance
static Input* s_instance = nullptr;

void Input::init(GLFWwindow* window)
{
    m_window = window;
    memset(m_keys,     0, sizeof(m_keys));
    memset(m_keysPrev, 0, sizeof(m_keysPrev));
    m_firstMouse = true;
    m_scrollDelta = 0.f;
    m_scrollAccum = 0.f;

    s_instance = this;
    glfwSetScrollCallback(window, scrollCallback);
}

// ── Gamepad deadzone rescaling ──────────────────────────────────
static float applyDeadzone(float value, float deadzone)
{
    if (std::abs(value) < deadzone) return 0.f;
    float sign = value > 0.f ? 1.f : -1.f;
    return sign * (std::abs(value) - deadzone) / (1.f - deadzone);
}

void Input::updateGamepad()
{
    // Save previous button state
    memcpy(m_gamepad.buttonsPrev, m_gamepad.buttons, sizeof(m_gamepad.buttons));

    // Scan for first connected gamepad
    bool found = false;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
        if (glfwJoystickIsGamepad(jid)) {
            GLFWgamepadstate state;
            if (glfwGetGamepadState(jid, &state)) {
                found = true;
                if (!m_gamepad.connected || m_gamepad.jid != jid) {
                    // New connection
                    m_gamepad.connected = true;
                    m_gamepad.jid = jid;
                    const char* n = glfwGetGamepadName(jid);
                    if (n) {
                        sv::StrCopy(m_gamepad.name, n);
                    }
                    fprintf(stderr, "[Input] Gamepad connected: %s (jid=%d)\n", m_gamepad.name, jid);
                }

                // Copy button state
                for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; i++)
                    m_gamepad.buttons[i] = (state.buttons[i] == GLFW_PRESS);

                // Copy raw axes
                for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; i++)
                    m_gamepad.axes[i] = state.axes[i];

                // Apply deadzone to sticks
                m_gamepad.stickLeftX  = applyDeadzone(state.axes[GLFW_GAMEPAD_AXIS_LEFT_X],  m_stickDeadzone);
                m_gamepad.stickLeftY  = applyDeadzone(state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y],  m_stickDeadzone);
                m_gamepad.stickRightX = applyDeadzone(state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], m_stickDeadzone);
                m_gamepad.stickRightY = applyDeadzone(state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y], m_stickDeadzone);

                // Remap triggers from [-1,+1] to [0,1]
                float rawLT = (state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]  + 1.f) * 0.5f;
                float rawRT = (state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.f) * 0.5f;
                m_gamepad.triggerLeft  = rawLT < m_triggerDeadzone ? 0.f : (rawLT - m_triggerDeadzone) / (1.f - m_triggerDeadzone);
                m_gamepad.triggerRight = rawRT < m_triggerDeadzone ? 0.f : (rawRT - m_triggerDeadzone) / (1.f - m_triggerDeadzone);

                break; // use first connected gamepad
            }
        }
    }

    if (!found && m_gamepad.connected) {
        fprintf(stderr, "[Input] Gamepad disconnected: %s\n", m_gamepad.name);
        m_gamepad.connected = false;
        m_gamepad.jid = -1;
        memset(m_gamepad.buttons, 0, sizeof(m_gamepad.buttons));
        memset(m_gamepad.axes, 0, sizeof(m_gamepad.axes));
        m_gamepad.stickLeftX = m_gamepad.stickLeftY = 0.f;
        m_gamepad.stickRightX = m_gamepad.stickRightY = 0.f;
        m_gamepad.triggerLeft = m_gamepad.triggerRight = 0.f;
    }

    // Merge injected gamepad input
    if (m_hasInjectedGpButtons) {
        for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; i++)
            m_gamepad.buttons[i] = m_gamepad.buttons[i] || m_injectedGpButtons[i];
        memset(m_injectedGpButtons, 0, sizeof(m_injectedGpButtons));
        m_hasInjectedGpButtons = false;
    }
    if (m_hasInjectedGpAxes) {
        for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; i++) {
            if (m_injectedGpAxes[i] != 0.f) {
                m_gamepad.axes[i] = m_injectedGpAxes[i];
                // Re-apply deadzone for injected stick values
                if (i == GLFW_GAMEPAD_AXIS_LEFT_X)  m_gamepad.stickLeftX  = applyDeadzone(m_gamepad.axes[i], m_stickDeadzone);
                if (i == GLFW_GAMEPAD_AXIS_LEFT_Y)  m_gamepad.stickLeftY  = applyDeadzone(m_gamepad.axes[i], m_stickDeadzone);
                if (i == GLFW_GAMEPAD_AXIS_RIGHT_X) m_gamepad.stickRightX = applyDeadzone(m_gamepad.axes[i], m_stickDeadzone);
                if (i == GLFW_GAMEPAD_AXIS_RIGHT_Y) m_gamepad.stickRightY = applyDeadzone(m_gamepad.axes[i], m_stickDeadzone);
            }
        }
        memset(m_injectedGpAxes, 0, sizeof(m_injectedGpAxes));
        m_hasInjectedGpAxes = false;
    }
}

void Input::update()
{
    // Tick down mouse suppression timer (uses suppression start time, not frame dt)
    if (m_mouseSuppressionTimer > 0.f) {
        double elapsed = glfwGetTime() - m_mouseSuppressionStart;
        m_mouseSuppressionTimer = m_mouseSuppressionDuration - (float)elapsed;
        if (m_mouseSuppressionTimer < 0.f) m_mouseSuppressionTimer = 0.f;
    }

    // Snapshot per-frame scroll (accumulated by callback since last update)
    m_scrollDelta = m_scrollAccum;
    m_scrollAccum = 0.f;

    // Snapshot key states
    memcpy(m_keysPrev, m_keys, sizeof(m_keys));
    for (int i = 0; i <= GLFW_KEY_LAST; i++) {
        m_keys[i] = glfwGetKey(m_window, i) == GLFW_PRESS;
    }

    // Merge injected input (from DevServer remote commands)
    if (m_hasInjectedInput) {
        for (int i = 0; i <= GLFW_KEY_LAST; i++)
            m_keys[i] = m_keys[i] || m_injectedKeys[i];
        m_scrollDelta += m_injectedScroll;
        // Mouse delta merged after position calc below
    }

    // Mouse position + delta (in framebuffer pixels)
    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);
    // glfwGetCursorPos returns screen coordinates; scale to framebuffer pixels
    // to match Window::width()/height() which are framebuffer size.
    int winW, winH, fbW, fbH;
    glfwGetWindowSize(m_window, &winW, &winH);
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    if (winW > 0 && winH > 0) {
        mx *= (double)fbW / winW;
        my *= (double)fbH / winH;
    }
    glm::vec2 pos((float)mx, (float)my);
    if (m_firstMouse) {
        m_lastMousePos = pos;
        m_firstMouse   = false;
    }
    m_mouseDelta   = pos - m_lastMousePos;
    m_lastMousePos = pos;
    m_mousePos     = pos;

    // Merge injected mouse delta (after real delta computed)
    if (m_hasInjectedInput) {
        m_mouseDelta += m_injectedMouseDelta;
        memset(m_injectedKeys, 0, sizeof(m_injectedKeys));
        m_injectedMouseDelta = {0.f, 0.f};
        m_injectedScroll = 0.f;
        m_hasInjectedInput = false;
    }

    // ── Injected mouse click (2-frame press/release for ImGui) ──
    if (m_injectedClickFrames > 0) {
        // Convert framebuffer coords back to screen coords for glfwSetCursorPos
        int wW, wH, fW, fH;
        glfwGetWindowSize(m_window, &wW, &wH);
        glfwGetFramebufferSize(m_window, &fW, &fH);
        double sx = m_injectedClickX, sy = m_injectedClickY;
        if (fW > 0 && fH > 0) {
            sx *= (double)wW / fW;
            sy *= (double)wH / fH;
        }
        glfwSetCursorPos(m_window, sx, sy);

        // Update our own mouse pos to match
        m_mousePos     = {m_injectedClickX, m_injectedClickY};
        m_lastMousePos = m_mousePos;
        m_mouseDelta   = {0.f, 0.f};

        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(m_injectedClickX, m_injectedClickY);

        if (m_injectedClickFrames == 2) {
            // Press frame
            m_injectedClickActive = true;
            io.AddMouseButtonEvent(m_injectedClickButton, true);
        } else {
            // Release frame
            m_injectedClickActive = false;
            io.AddMouseButtonEvent(m_injectedClickButton, false);
        }
        m_injectedClickFrames--;
    }

    // ── Gamepad polling ─────────────────────────────────────────
    updateGamepad();
}

bool Input::isKeyDown(int key) const
{
    if (key < 0 || key > GLFW_KEY_LAST) return false;
    return m_keys[key];
}

bool Input::isKeyPressed(int key) const
{
    if (key < 0 || key > GLFW_KEY_LAST) return false;
    return m_keys[key] && !m_keysPrev[key];
}

bool Input::isMouseDown(int button) const
{
    if (m_mouseSuppressionTimer > 0.f) return false;
    if (m_injectedClickActive && button == m_injectedClickButton) return true;
    return glfwGetMouseButton(m_window, button) == GLFW_PRESS;
}

void Input::setCursorLocked(bool locked)
{
    m_cursorLocked = locked;
    glfwSetInputMode(m_window, GLFW_CURSOR,
        locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Reset mouse delta on lock/unlock to prevent a large jump
    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);
    m_lastMousePos = glm::vec2((float)mx, (float)my);
    m_mouseDelta   = glm::vec2(0.f);
}

void Input::beginMouseSuppression(float seconds)
{
    m_mouseSuppressionTimer    = seconds;
    m_mouseSuppressionDuration = seconds;
    m_mouseSuppressionStart    = glfwGetTime();
}

void Input::scrollCallback(GLFWwindow* /*w*/, double /*xoff*/, double yoff)
{
    if (s_instance) s_instance->m_scrollAccum += (float)yoff;
}

// ── Gamepad queries ─────────────────────────────────────────────

bool Input::isGamepadButtonDown(int btn) const
{
    if (btn < 0 || btn > GLFW_GAMEPAD_BUTTON_LAST) return false;
    return m_gamepad.buttons[btn];
}

bool Input::isGamepadButtonPressed(int btn) const
{
    if (btn < 0 || btn > GLFW_GAMEPAD_BUTTON_LAST) return false;
    return m_gamepad.buttons[btn] && !m_gamepad.buttonsPrev[btn];
}

float Input::gamepadAxis(int axis) const
{
    if (axis < 0 || axis > GLFW_GAMEPAD_AXIS_LAST) return 0.f;
    return m_gamepad.axes[axis];
}

// ── Action binding evaluation ───────────────────────────────────

bool Input::isBindingDown(const InputBinding& b) const
{
    switch (b.source) {
        case BindSource::Key:           return isKeyDown(b.code);
        case BindSource::MouseButton:   return isMouseDown(b.code);
        case BindSource::GamepadButton: return isGamepadButtonDown(b.code);
        case BindSource::GamepadAxis: {
            if (b.code < 0 || b.code > GLFW_GAMEPAD_AXIS_LAST) return false;
            float v = m_gamepad.axes[b.code];
            // Check if axis is pushed in the correct direction past threshold
            return (v * b.sign) > 0.3f;
        }
        default: return false;
    }
}

bool Input::isBindingPressed(const InputBinding& b) const
{
    switch (b.source) {
        case BindSource::Key:           return isKeyPressed(b.code);
        case BindSource::MouseButton:   return false; // mouse buttons don't have pressed tracking yet
        case BindSource::GamepadButton: return isGamepadButtonPressed(b.code);
        case BindSource::GamepadAxis: {
            if (b.code < 0 || b.code > GLFW_GAMEPAD_AXIS_LAST) return false;
            float v    = m_gamepad.axes[b.code];
            float prev = m_gamepad.buttonsPrev[0]; // not usable for axes
            // For axis press detection: currently past threshold, and was below last frame
            // We don't track previous axis values, so treat analog as held-only for now
            (void)v; (void)prev;
            return false;
        }
        default: return false;
    }
}

float Input::bindingAxis(const InputBinding& b) const
{
    switch (b.source) {
        case BindSource::Key:
        case BindSource::MouseButton:
        case BindSource::GamepadButton:
            return isBindingDown(b) ? 1.0f : 0.0f;
        case BindSource::GamepadAxis: {
            if (b.code < 0 || b.code > GLFW_GAMEPAD_AXIS_LAST) return 0.f;
            float v = m_gamepad.axes[b.code] * b.sign;
            return v > 0.f ? v : 0.f; // only return positive contribution
        }
        default: return 0.f;
    }
}

bool Input::isActionDown(Action a) const
{
    if (!m_bindings || a >= Action::COUNT) return false;
    const auto& ab = m_bindings->get(a);
    return isBindingDown(ab.primary) || isBindingDown(ab.alternate) || isBindingDown(ab.gamepad);
}

bool Input::isActionPressed(Action a) const
{
    if (!m_bindings || a >= Action::COUNT) return false;
    const auto& ab = m_bindings->get(a);
    return isBindingPressed(ab.primary) || isBindingPressed(ab.alternate) || isBindingPressed(ab.gamepad);
}

float Input::actionAxis(Action a) const
{
    if (!m_bindings || a >= Action::COUNT) return 0.f;
    const auto& ab = m_bindings->get(a);
    float v = bindingAxis(ab.primary);
    v = std::max(v, bindingAxis(ab.alternate));
    v = std::max(v, bindingAxis(ab.gamepad));
    return v;
}

// ── Remote input injection ───────────────────────────────────────

void Input::injectKey(int key)
{
    if (key >= 0 && key <= GLFW_KEY_LAST) {
        m_injectedKeys[key] = true;
        m_hasInjectedInput = true;
    }
}

void Input::injectMouseDelta(float dx, float dy)
{
    m_injectedMouseDelta += glm::vec2(dx, dy);
    m_hasInjectedInput = true;
}

void Input::injectScroll(float delta)
{
    m_injectedScroll += delta;
    m_hasInjectedInput = true;
}

void Input::injectMouseClick(float x, float y, int button)
{
    m_injectedClickX      = x;
    m_injectedClickY      = y;
    m_injectedClickButton = button;
    m_injectedClickFrames = 2;  // press on next frame, release on frame after
    m_injectedClick       = true;

    // Unlock cursor if locked (ImGui needs visible cursor for UI clicks)
    if (m_cursorLocked) setCursorLocked(false);
}

void Input::injectGamepadButton(int btn)
{
    if (btn >= 0 && btn <= GLFW_GAMEPAD_BUTTON_LAST) {
        m_injectedGpButtons[btn] = true;
        m_hasInjectedGpButtons = true;
    }
}

void Input::injectGamepadAxis(int axis, float value)
{
    if (axis >= 0 && axis <= GLFW_GAMEPAD_AXIS_LAST) {
        m_injectedGpAxes[axis] = value;
        m_hasInjectedGpAxes = true;
    }
}

} // namespace sv
