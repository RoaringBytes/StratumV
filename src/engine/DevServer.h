// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>
#include <nlohmann/json.hpp>

#include "PostProcess.h"

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <future>
#include <functional>
#include <cstdint>
#include <chrono>
#include <unordered_map>

namespace sv {

class VkCtx;
class Input;

// Which GPU buffer to copy for the current screenshot request
enum class ScreenshotMode { Color, Depth, Normals };

// Frame state passed from main loop each frame
struct DevFrameState {
    float fps       = 0;
    float dt        = 0;
    float time      = 0;
    int   width     = 0;
    int   height    = 0;
    const char* gpu = "";
};

// Engine state snapshot (main thread copies each frame, server thread reads).
// Game-specific state is NOT included — games extend via command handlers.
struct DevSnapshot {
    DevFrameState frame;

    // Post-process (engine-owned)
    PostProcessUBO postProc{};

    // Render state
    bool dlssEnabled = false, restirEnabled = false, sharcEnabled = false;
    bool adminOpen = false;
    int dlssQualityIndex = 1;
    bool rtShadowManual = false;
    float sharcLogBase = 2.0f;
    float sharcLevelBias = 0.0f;
    uint32_t sharcStaleFrameMax = 128;

    // GPU pass timing (ms per pass, populated when gpuProfilingEnabled)
    static constexpr int kMaxGpuPasses = 16;
    float gpuPassMs[kMaxGpuPasses]{};
    const char* gpuPassNames[kMaxGpuPasses]{};
    int   gpuTimerCount       = 0;
    bool  gpuProfilingEnabled = false;
    float vramUsedMB          = 0.f;
    float vramBudgetMB        = 0.f;
};

// Live state pointers for main-thread write commands.
// Engine-generic bindings only — games add their own via command handlers.
struct DevBindings {
    // Post-processing
    PostProcessUBO* postProc = nullptr;

    // Feature toggles
    bool*     dlssEnabled        = nullptr;
    bool*     restirEnabled      = nullptr;
    bool*     sharcEnabled       = nullptr;
    int*      dlssQualityIndex   = nullptr;
    bool*     rtShadowManual     = nullptr;
    float*    sharcLogBase       = nullptr;
    float*    sharcLevelBias     = nullptr;
    uint32_t* sharcStaleFrameMax = nullptr;
    bool*     adminOpen          = nullptr;

    // GPU profiling control
    bool* gpuProfilingEnabled = nullptr;

    // Scene persistence callbacks (set once by Engine)
    std::function<void()> onSaveScene;
    std::function<void()> onLoadScene;
    std::function<void()> onResetScene;
};

// Command queued from server thread, executed on main thread
struct PendingCommand {
    nlohmann::json cmd;
    std::promise<nlohmann::json> response;
};

// Command handler: takes command JSON, returns response JSON.
// "Server-thread" handlers run on the TCP thread (read-only access).
// "Main-thread" handlers run on the game loop thread (can write state).
using DevCommandHandler = std::function<nlohmann::json(const nlohmann::json&)>;

// TCP debug server — JSON commands over loopback, game responds.
// Engine provides built-in commands (status, screenshot, input, render toggles).
// Games register additional commands via registerCommand/registerMainThreadCommand.
class DevServer {
public:
    bool start(uint16_t port = 9999);
    void stop();

    // ── Engine binding ──────────────────────────────────────────
    void setInput(Input* input) { m_input = input; }
    DevBindings& bindings() { return m_bindings; }

    // ── Main-thread interface (called each frame) ────────────
    void updateSnapshot(const DevSnapshot& snap);

    // Returns true if a screenshot should be captured this frame (clears flag)
    bool takeScreenshot() { return m_screenshotPending.exchange(false); }
    bool isScreenshotPending() const { return m_screenshotPending.load(); }
    std::string screenshotPath() const { return m_screenshotPath; }
    ScreenshotMode screenshotMode() const { return m_screenshotMode; }

    // Returns true if shader reload was requested
    bool reloadPending() const { return m_reloadPending; }
    void clearReload() { m_reloadPending = false; }

    // Call after screenshot write completes (signals waiting client)
    void screenshotDone(bool ok, uint32_t width, uint32_t height);

    // Call each frame to drain queued commands from server thread
    void pollCommands();

    // ── Screenshot staging buffer management ─────────────────
    void ensureStagingBuffer(VkCtx& ctx, uint32_t width, uint32_t height);
    void destroyStagingBuffer(VmaAllocator alloc);
    VkBuffer stagingBuffer() const { return m_stagingBuffer; }

    // Write staging buffer contents to PNG (BGRA→RGBA swizzle)
    bool writeScreenshot(VmaAllocator alloc, uint32_t width, uint32_t height);

    // Write D32_SFLOAT depth buffer as grayscale PNG (near=white, far=black)
    bool writeDepthScreenshot(VmaAllocator alloc, uint32_t width, uint32_t height);

    // Write R16G16_SFLOAT octahedral normals as RGB PNG
    bool writeNormalsScreenshot(VmaAllocator alloc, uint32_t width, uint32_t height);

    // ── Command handler registration (game extension point) ──
    // Server-thread handler: runs on TCP thread, must only read shared state.
    void registerCommand(const std::string& name, DevCommandHandler handler);

    // Main-thread handler: queued and executed on game loop thread.
    void registerMainThreadCommand(const std::string& name, DevCommandHandler handler);

private:
    void serverLoop(uint16_t port);
    void handleClient(uintptr_t clientSock);
    nlohmann::json processCommand(const nlohmann::json& cmd);
    nlohmann::json executeMainThreadCommand(const nlohmann::json& cmd);

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    uintptr_t         m_listenSocket = ~(uintptr_t)0;

    // Engine binding
    Input*      m_input    = nullptr;
    DevBindings m_bindings;

    // Shared state (protected by m_mutex)
    std::mutex              m_mutex;
    std::condition_variable m_screenshotCV;
    DevSnapshot             m_snapshot{};
    std::vector<PendingCommand> m_pendingCommands;

    // Screenshot request/response
    std::atomic<bool> m_screenshotPending{false};
    std::string       m_screenshotPath;
    ScreenshotMode    m_screenshotMode   = ScreenshotMode::Color;
    bool              m_screenshotOk     = false;
    uint32_t          m_screenshotWidth  = 0;
    uint32_t          m_screenshotHeight = 0;
    bool              m_screenshotDone   = false;

    // Shader reload request
    std::atomic<bool> m_reloadPending{false};

    // Optional auth token for introspection endpoints (from STRATUMV_ADMIN_TOKEN env)
    std::string m_authToken;

    // Key hold entries — main thread only, no lock needed
    struct KeyHoldEntry {
        int keyCode;
        std::chrono::steady_clock::time_point expiresAt;
    };
    std::vector<KeyHoldEntry> m_keyHolds;

    // Screenshot staging buffer (VMA, CPU-readable)
    VkBuffer      m_stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation m_stagingAlloc  = VK_NULL_HANDLE;
    VkDeviceSize  m_stagingSize   = 0;

    // Registered command handlers (game extension)
    std::unordered_map<std::string, DevCommandHandler> m_commandHandlers;
    std::unordered_map<std::string, DevCommandHandler> m_mainThreadHandlers;
};

} // namespace sv
