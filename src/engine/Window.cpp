// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#define GLFW_INCLUDE_NONE
#include "Window.h"
#include <GLFW/glfw3.h>
#include <cstdio>

namespace sv {

bool Window::init(const WindowConfig& cfg)
{
    if (!glfwInit()) {
        fprintf(stderr, "[StratumV] Failed to initialize GLFW\n");
        return false;
    }

    // No OpenGL context — we use Vulkan
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWmonitor* monitor = nullptr;
    if (cfg.fullscreen) {
        monitor = glfwGetPrimaryMonitor();
        m_borderlessFullscreen = true;
    }

    m_window = glfwCreateWindow(cfg.width, cfg.height, cfg.title.c_str(), monitor, nullptr);

    if (!m_window) {
        fprintf(stderr, "[StratumV] Failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    glfwSetFramebufferSizeCallback(m_window, framebufferSizeCallback);
    glfwSetWindowFocusCallback(m_window, windowFocusCallback);
    glfwSetWindowUserPointer(m_window, this);
    glfwGetFramebufferSize(m_window, &m_width, &m_height);
    m_lastTime = glfwGetTime();

    // Store initial windowed geometry for fullscreen restore
    if (!m_borderlessFullscreen) {
        glfwGetWindowPos(m_window, &m_savedX, &m_savedY);
        m_savedW = cfg.width;
        m_savedH = cfg.height;
    }

    printf("[StratumV] Window created: %dx%d (Vulkan)%s\n",
           m_width, m_height, m_borderlessFullscreen ? " [fullscreen]" : "");

    return true;
}

void Window::shutdown()
{
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}

bool Window::shouldClose() const
{
    return glfwWindowShouldClose(m_window);
}

void Window::requestClose()
{
    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void Window::pollEvents()
{
    glfwPollEvents();
}

double Window::time() const
{
    return glfwGetTime();
}

void Window::updateTiming()
{
    double now = glfwGetTime();
    m_dt = (float)(now - m_lastTime);
    m_lastTime = now;
    if (m_dt > 0.1f) m_dt = 0.016f; // Clamp spikes
}

// ── Debounced resize ─────────────────────────────────────────────
void Window::updateResize()
{
    if (!m_resizePending) return;

    double now = glfwGetTime();
    if (now - m_lastResizeTime >= 0.2) {
        // Debounce expired — commit the pending size
        m_width  = m_pendingWidth;
        m_height = m_pendingHeight;
        m_resizePending = false;
        m_resizeStable  = true;
    }
}

// ── Fullscreen toggle ────────────────────────────────────────────
void Window::toggleBorderlessFullscreen()
{
    if (m_borderlessFullscreen) {
        // Restore windowed mode
        glfwSetWindowMonitor(m_window, nullptr,
            m_savedX, m_savedY, m_savedW, m_savedH, 0);
        m_borderlessFullscreen = false;
        printf("[StratumV] Switched to windowed mode (%dx%d)\n", m_savedW, m_savedH);
    } else {
        // Save current windowed pos/size
        glfwGetWindowPos(m_window, &m_savedX, &m_savedY);
        glfwGetWindowSize(m_window, &m_savedW, &m_savedH);

        // Enter borderless fullscreen on current monitor
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(m_window, monitor,
            0, 0, mode->width, mode->height, mode->refreshRate);
        m_borderlessFullscreen = true;
        printf("[StratumV] Switched to borderless fullscreen (%dx%d)\n",
               mode->width, mode->height);
    }
    // glfwSetWindowMonitor triggers framebufferSizeCallback,
    // which feeds into the debounce. The 200ms timer will fire
    // and trigger a full re-sync.
}

// ── GLFW callbacks ───────────────────────────────────────────────
void Window::framebufferSizeCallback(GLFWwindow* w, int width, int height)
{
    auto* self = (Window*)glfwGetWindowUserPointer(w);
    if (!self) return;

    self->m_pendingWidth  = width;
    self->m_pendingHeight = height;
    self->m_resizePending = true;
    self->m_resizeStable  = false;
    self->m_lastResizeTime = glfwGetTime();
}

void Window::windowFocusCallback(GLFWwindow* w, int focused)
{
    auto* self = (Window*)glfwGetWindowUserPointer(w);
    if (!self) return;
    bool wasFocused = self->m_focused;
    self->m_focused = (focused == GLFW_TRUE);
    if (!wasFocused && self->m_focused)
        self->m_focusRegained = true;
}

} // namespace sv
