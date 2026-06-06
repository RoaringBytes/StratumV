// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <string>

struct GLFWwindow;

namespace sv {

struct WindowConfig {
    int width       = 1920;
    int height      = 1080;
    std::string title = "StratumV";
    bool fullscreen = false;
    bool vsync      = true;
};

class Window {
public:
    bool init(const WindowConfig& cfg);
    void shutdown();
    bool shouldClose() const;
    void requestClose();
    void pollEvents();

    GLFWwindow* handle() const { return m_window; }
    int    width()     const { return m_width; }
    int    height()    const { return m_height; }
    float  aspect()    const { return (float)m_width / (float)m_height; }
    double time()      const;
    float  deltaTime() const { return m_dt; }

    // Debounced resize
    void   updateResize();                        // call each frame — ticks debounce timer
    bool   isResizePending() const { return m_resizePending; }
    bool   isResizeStable()  const { return m_resizeStable; }
    void   clearResizeStable()     { m_resizeStable = false; }

    // Focus
    bool   isFocused()         const { return m_focused; }
    bool   focusJustRegained() const { return m_focusRegained; }
    void   clearFocusRegained()      { m_focusRegained = false; }

    // Fullscreen
    void   toggleBorderlessFullscreen();
    bool   isBorderlessFullscreen() const { return m_borderlessFullscreen; }

    void updateTiming();

private:
    GLFWwindow* m_window = nullptr;
    int m_width  = 0;
    int m_height = 0;
    double m_lastTime = 0.0;
    float  m_dt = 0.016f;
    bool   m_focused        = true;
    bool   m_focusRegained  = false;

    // Debounced resize state
    double m_lastResizeTime = 0.0;
    int    m_pendingWidth   = 0;
    int    m_pendingHeight  = 0;
    bool   m_resizePending  = false;
    bool   m_resizeStable   = false;

    // Fullscreen state
    bool   m_borderlessFullscreen = false;
    int    m_savedX = 100, m_savedY = 100;
    int    m_savedW = 1920, m_savedH = 1080;

    static void framebufferSizeCallback(GLFWwindow* w, int width, int height);
    static void windowFocusCallback(GLFWwindow* w, int focused);
};

} // namespace sv
