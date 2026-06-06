// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// Winsock must come before any header that pulls in windows.h (volk, etc.)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "DevServer.h"
#include "EngineLog.h"
#include "SystemRegistry.h"
#include "vk/VkContext.h"
#include "Input.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <chrono>

namespace sv {

// ── Key name → GLFW mapping ────────────────────────────────────────
static int keyNameToGLFW(const std::string& name)
{
    static const std::unordered_map<std::string, int> map = {
        {"F1", GLFW_KEY_F1}, {"F2", GLFW_KEY_F2}, {"F3", GLFW_KEY_F3},
        {"F4", GLFW_KEY_F4}, {"F5", GLFW_KEY_F5}, {"F6", GLFW_KEY_F6},
        {"F7", GLFW_KEY_F7}, {"F8", GLFW_KEY_F8}, {"F9", GLFW_KEY_F9},
        {"F10", GLFW_KEY_F10}, {"F11", GLFW_KEY_F11}, {"F12", GLFW_KEY_F12},
        {"W", GLFW_KEY_W}, {"A", GLFW_KEY_A}, {"S", GLFW_KEY_S}, {"D", GLFW_KEY_D},
        {"Q", GLFW_KEY_Q}, {"E", GLFW_KEY_E}, {"R", GLFW_KEY_R}, {"T", GLFW_KEY_T},
        {"G", GLFW_KEY_G}, {"X", GLFW_KEY_X},
        {"SPACE", GLFW_KEY_SPACE}, {"ESCAPE", GLFW_KEY_ESCAPE},
        {"LSHIFT", GLFW_KEY_LEFT_SHIFT}, {"LCTRL", GLFW_KEY_LEFT_CONTROL},
        {"TAB", GLFW_KEY_TAB}, {"DELETE", GLFW_KEY_DELETE},
        {"UP", GLFW_KEY_UP}, {"DOWN", GLFW_KEY_DOWN},
        {"LEFT", GLFW_KEY_LEFT}, {"RIGHT", GLFW_KEY_RIGHT},
        {"PGUP", GLFW_KEY_PAGE_UP}, {"PGDN", GLFW_KEY_PAGE_DOWN},
    };
    auto it = map.find(name);
    return (it != map.end()) ? it->second : -1;
}

// ── Startup / Shutdown ──────────────────────────────────────────
bool DevServer::start(uint16_t port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[DevServer] WSAStartup failed\n");
        return false;
    }

    // Optional auth token for introspection endpoints
    const char* envToken = std::getenv("STRATUMV_ADMIN_TOKEN");
    if (envToken && envToken[0]) {
        m_authToken = envToken;
        printf("[DevServer] Admin token configured (%zu chars)\n", m_authToken.size());
    }

    m_running = true;
    m_thread = std::thread(&DevServer::serverLoop, this, port);
    return true;
}

void DevServer::stop()
{
    m_running = false;

    // Close listen socket to unblock accept()
    if (m_listenSocket != ~(uintptr_t)0) {
        closesocket((SOCKET)m_listenSocket);
        m_listenSocket = ~(uintptr_t)0;
    }

    // Wake any waiting screenshot or pending commands
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_screenshotDone = true;
    }
    m_screenshotCV.notify_all();

    if (m_thread.joinable()) m_thread.join();
    WSACleanup();
}

// ── Main-thread interface ───────────────────────────────────────

void DevServer::updateSnapshot(const DevSnapshot& snap)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = snap;
}

void DevServer::screenshotDone(bool ok, uint32_t width, uint32_t height)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_screenshotOk     = ok;
        m_screenshotWidth  = width;
        m_screenshotHeight = height;
        m_screenshotDone   = true;
        m_screenshotPending = false;
    }
    m_screenshotCV.notify_all();
}

void DevServer::pollCommands()
{
    std::vector<PendingCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cmds.swap(m_pendingCommands);
    }
    for (auto& pc : cmds) {
        pc.response.set_value(executeMainThreadCommand(pc.cmd));
    }

    // Tick key holds — inject each active key every frame until its timer expires
    if (!m_keyHolds.empty() && m_input) {
        auto now = std::chrono::steady_clock::now();
        m_keyHolds.erase(
            std::remove_if(m_keyHolds.begin(), m_keyHolds.end(),
                [&](const KeyHoldEntry& e) { return now >= e.expiresAt; }),
            m_keyHolds.end());
        for (const auto& e : m_keyHolds)
            m_input->injectKey(e.keyCode);
    }
}

// ── Command handler registration ────────────────────────────────

void DevServer::registerCommand(const std::string& name, DevCommandHandler handler)
{
    m_commandHandlers[name] = std::move(handler);
}

void DevServer::registerMainThreadCommand(const std::string& name, DevCommandHandler handler)
{
    m_mainThreadHandlers[name] = std::move(handler);
}

// ── Screenshot staging buffer ───────────────────────────────────
void DevServer::ensureStagingBuffer(VkCtx& ctx, uint32_t width, uint32_t height)
{
    VkDeviceSize needed = (VkDeviceSize)width * height * 4;
    if (m_stagingBuffer && m_stagingSize >= needed) return;

    if (m_stagingBuffer) {
        vmaDestroyBuffer(ctx.allocator(), m_stagingBuffer, m_stagingAlloc);
    }

    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size  = needed;
    bufCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;

    vmaCreateBuffer(ctx.allocator(), &bufCI, &allocCI,
        &m_stagingBuffer, &m_stagingAlloc, nullptr);
    m_stagingSize = needed;
}

void DevServer::destroyStagingBuffer(VmaAllocator alloc)
{
    if (m_stagingBuffer) {
        vmaDestroyBuffer(alloc, m_stagingBuffer, m_stagingAlloc);
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingAlloc  = VK_NULL_HANDLE;
        m_stagingSize   = 0;
    }
}

bool DevServer::writeScreenshot(VmaAllocator alloc, uint32_t width, uint32_t height)
{
    void* mapped;
    if (vmaMapMemory(alloc, m_stagingAlloc, &mapped) != VK_SUCCESS) {
        fprintf(stderr, "[DevServer] Failed to map staging buffer\n");
        screenshotDone(false, width, height);
        return false;
    }

    // Copy raw pixels to heap buffer then unmap immediately
    std::vector<uint8_t> pixels(static_cast<uint8_t*>(mapped),
        static_cast<uint8_t*>(mapped) + (size_t)width * height * 4);
    vmaUnmapMemory(alloc, m_stagingAlloc);

    // BGRA → RGBA swizzle + PNG write on a background thread
    std::string path = m_screenshotPath;
    std::thread([this, pixels = std::move(pixels), width, height, path]() mutable {
        for (uint32_t i = 0; i < width * height; i++) {
            uint8_t* p = pixels.data() + i * 4;
            std::swap(p[0], p[2]);
        }

        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());

        int ok = stbi_write_png(path.c_str(),
            (int)width, (int)height, 4, pixels.data(), (int)(width * 4));

        if (ok)
            printf("[DevServer] Screenshot saved: %s (%ux%u)\n", path.c_str(), width, height);
        else
            fprintf(stderr, "[DevServer] Failed to write: %s\n", path.c_str());

        screenshotDone(ok != 0, width, height);
    }).detach();

    return true; // completes asynchronously
}

bool DevServer::writeDepthScreenshot(VmaAllocator alloc, uint32_t width, uint32_t height)
{
    void* mapped;
    if (vmaMapMemory(alloc, m_stagingAlloc, &mapped) != VK_SUCCESS) {
        fprintf(stderr, "[DevServer] Failed to map staging buffer (depth)\n");
        screenshotDone(false, width, height);
        return false;
    }

    // D32_SFLOAT: 4 bytes per pixel (float), copy to heap
    size_t byteCount = (size_t)width * height * sizeof(float);
    std::vector<float> depthData(width * height);
    memcpy(depthData.data(), mapped, byteCount);
    vmaUnmapMemory(alloc, m_stagingAlloc);

    std::string path = m_screenshotPath;
    std::thread([this, depthData = std::move(depthData), width, height, path]() mutable {
        // Find min/max for normalization (skip 0.0 = sky)
        float minD = 1.0f, maxD = 0.0f;
        for (float d : depthData) {
            if (d > 0.0f && d < 1.0f) {
                minD = std::min(minD, d);
                maxD = std::max(maxD, d);
            }
        }
        if (maxD <= minD) { minD = 0.0f; maxD = 1.0f; }

        // Convert to grayscale: near=white, far=black
        std::vector<uint8_t> pixels(width * height);
        for (uint32_t i = 0; i < width * height; i++) {
            float d = depthData[i];
            if (d <= 0.0f || d >= 1.0f) {
                pixels[i] = 0; // sky/far = black
            } else {
                float normalized = 1.0f - (d - minD) / (maxD - minD);
                pixels[i] = (uint8_t)(normalized * 255.0f);
            }
        }

        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());

        int ok = stbi_write_png(path.c_str(),
            (int)width, (int)height, 1, pixels.data(), (int)width);

        if (ok)
            printf("[DevServer] Depth screenshot saved: %s (%ux%u)\n", path.c_str(), width, height);
        else
            fprintf(stderr, "[DevServer] Failed to write depth: %s\n", path.c_str());

        screenshotDone(ok != 0, width, height);
    }).detach();

    return true;
}

bool DevServer::writeNormalsScreenshot(VmaAllocator alloc, uint32_t width, uint32_t height)
{
    void* mapped;
    if (vmaMapMemory(alloc, m_stagingAlloc, &mapped) != VK_SUCCESS) {
        fprintf(stderr, "[DevServer] Failed to map staging buffer (normals)\n");
        screenshotDone(false, width, height);
        return false;
    }

    // R16G16_SFLOAT: 4 bytes per pixel (2 x half-float), copy to heap
    size_t byteCount = (size_t)width * height * 4;
    std::vector<uint16_t> normalData(width * height * 2);
    memcpy(normalData.data(), mapped, byteCount);
    vmaUnmapMemory(alloc, m_stagingAlloc);

    std::string path = m_screenshotPath;
    std::thread([this, normalData = std::move(normalData), width, height, path]() mutable {
        // Decode R16G16 half-float octahedral normals to RGB
        std::vector<uint8_t> pixels(width * height * 3);
        for (uint32_t i = 0; i < width * height; i++) {
            // Convert half-float to float (IEEE 754 binary16)
            auto halfToFloat = [](uint16_t h) -> float {
                uint32_t sign = (h >> 15) & 1;
                uint32_t exp  = (h >> 10) & 0x1F;
                uint32_t mant = h & 0x3FF;
                if (exp == 0) {
                    if (mant == 0) return sign ? -0.0f : 0.0f;
                    float f = (float)mant / 1024.0f;
                    return sign ? -f * (1.0f / 16384.0f) : f * (1.0f / 16384.0f);
                }
                if (exp == 31) return sign ? -INFINITY : INFINITY;
                float f = (1.0f + (float)mant / 1024.0f) * powf(2.0f, (float)exp - 15.0f);
                return sign ? -f : f;
            };

            float octX = halfToFloat(normalData[i * 2 + 0]);
            float octY = halfToFloat(normalData[i * 2 + 1]);

            // Octahedral decode: oct to unit normal
            float nx = octX, ny = octY;
            float nz = 1.0f - fabsf(nx) - fabsf(ny);
            if (nz < 0.0f) {
                float tx = nx;
                nx = (1.0f - fabsf(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
                ny = (1.0f - fabsf(tx)) * (ny >= 0.0f ? 1.0f : -1.0f);
            }
            // Normalize
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (len > 0.0f) { nx /= len; ny /= len; nz /= len; }

            // Map [-1,1] to [0,255]
            pixels[i * 3 + 0] = (uint8_t)((nx * 0.5f + 0.5f) * 255.0f);
            pixels[i * 3 + 1] = (uint8_t)((ny * 0.5f + 0.5f) * 255.0f);
            pixels[i * 3 + 2] = (uint8_t)((nz * 0.5f + 0.5f) * 255.0f);
        }

        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());

        int ok = stbi_write_png(path.c_str(),
            (int)width, (int)height, 3, pixels.data(), (int)(width * 3));

        if (ok)
            printf("[DevServer] Normals screenshot saved: %s (%ux%u)\n", path.c_str(), width, height);
        else
            fprintf(stderr, "[DevServer] Failed to write normals: %s\n", path.c_str());

        screenshotDone(ok != 0, width, height);
    }).detach();

    return true;
}

// ── TCP server ──────────────────────────────────────────────────

void DevServer::serverLoop(uint16_t port)
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        fprintf(stderr, "[DevServer] socket() failed\n");
        return;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[DevServer] bind() failed on port %u\n", port);
        closesocket(listenSock);
        return;
    }

    if (listen(listenSock, 4) == SOCKET_ERROR) {
        fprintf(stderr, "[DevServer] listen() failed\n");
        closesocket(listenSock);
        return;
    }

    m_listenSocket = (uintptr_t)listenSock;
    printf("[DevServer] Listening on localhost:%u\n", port);

    while (m_running) {
        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!m_running) break;
            continue;
        }

        handleClient((uintptr_t)client);
        closesocket(client);
    }

    closesocket(listenSock);
}

void DevServer::handleClient(uintptr_t clientSock)
{
    SOCKET sock = (SOCKET)clientSock;

    int timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    const char* welcome = "{\"welcome\":\"StratumV DevServer v1.0\"}\n";
    send(sock, welcome, (int)strlen(welcome), 0);

    char buf[4096];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(sock, buf + total, (int)(sizeof(buf) - 1 - total), 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strchr(buf, '\n')) break;
    }

    if (total <= 0) return;

    nlohmann::json cmd;
    try {
        cmd = nlohmann::json::parse(buf);
    } catch (...) {
        const char* err = "{\"ok\":false,\"error\":\"invalid JSON\"}\n";
        send(sock, err, (int)strlen(err), 0);
        return;
    }

    nlohmann::json response = processCommand(cmd);

    std::string resp = response.dump() + "\n";
    send(sock, resp.c_str(), (int)resp.size(), 0);
}

// ── Command dispatch (server thread) ────────────────────────────

nlohmann::json DevServer::processCommand(const nlohmann::json& cmd)
{
    std::string type = cmd.value("cmd", "");

    // ── Read-only commands (server thread, reads snapshot under mutex) ──

    if (type == "status") {
        std::lock_guard<std::mutex> lock(m_mutex);
        return {
            {"ok", true},
            {"fps", m_snapshot.frame.fps},
            {"dt", m_snapshot.frame.dt},
            {"time", m_snapshot.frame.time},
            {"width", m_snapshot.frame.width},
            {"height", m_snapshot.frame.height},
            {"gpu", m_snapshot.frame.gpu}
        };
    }

    // Auth guard for introspection endpoints
    if (type == "get_graph" || type == "get_health") {
        if (!m_authToken.empty()) {
            std::string provided = cmd.value("auth", "");
            if (provided != m_authToken) {
                return {{"ok", false}, {"error", "unauthorized"}};
            }
        }
    }

    // Connectome graph + health queries
    if (type == "get_graph") {
        return {{"ok", true}, {"graph", SystemRegistry::get().serializeGraph()}};
    }
    if (type == "get_health") {
        return {{"ok", true}, {"health", SystemRegistry::get().serializeHealth()}};
    }

    if (type == "get_log") {
        uint64_t sinceId   = cmd.value("since_id", (uint64_t)0);
        std::string sevStr = cmd.value("min_severity", "debug");
        std::string tag    = cmd.value("tag", "");
        size_t limit       = cmd.value("limit", (size_t)100);

        auto entries = EngineLog::get().getEntries(
            sinceId, severityFromString(sevStr.c_str()),
            tag.empty() ? nullptr : tag.c_str(), limit);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : entries) {
            arr.push_back({
                {"id",       e.id},
                {"severity", severityToString(e.severity)},
                {"tag",      e.tag},
                {"message",  e.message},
                {"time_ms",  e.timestampMs}
            });
        }
        return {{"ok", true}, {"latest_id", EngineLog::get().latestId()}, {"entries", arr}};
    }

    if (type == "get_state") {
        std::string target = cmd.value("target", "all");
        std::lock_guard<std::mutex> lock(m_mutex);

        nlohmann::json resp = {{"ok", true}};

        if (target == "frame" || target == "all") {
            nlohmann::json frame;
            frame["fps"]       = m_snapshot.frame.fps;
            frame["dt"]        = m_snapshot.frame.dt;
            frame["time"]      = m_snapshot.frame.time;
            frame["adminOpen"] = m_snapshot.adminOpen;
            resp["frame"] = frame;
        }

        // Dedicated timing query
        if (target == "timing" || target == "all") {
            resp["gpuProfilingEnabled"] = m_snapshot.gpuProfilingEnabled;
            if (m_snapshot.gpuProfilingEnabled && m_snapshot.gpuTimerCount > 0) {
                nlohmann::json passes;
                float total = 0.f;
                for (int i = 0; i < m_snapshot.gpuTimerCount; i++) {
                    const char* name = m_snapshot.gpuPassNames[i] ? m_snapshot.gpuPassNames[i] : "unknown";
                    passes[name] = m_snapshot.gpuPassMs[i];
                    total += m_snapshot.gpuPassMs[i];
                }
                resp["passes"] = passes;
                resp["totalMs"] = total;
            }
        }

        // Dedicated VRAM query
        if (target == "vram" || target == "all") {
            resp["vram"] = {
                {"usedMB", m_snapshot.vramUsedMB},
                {"budgetMB", m_snapshot.vramBudgetMB},
                {"usagePercent", m_snapshot.vramBudgetMB > 0.f
                    ? (m_snapshot.vramUsedMB / m_snapshot.vramBudgetMB * 100.f) : 0.f}
            };
        }

        // Render state
        if (target == "render" || target == "all") {
            resp["render"] = {
                {"dlssEnabled", m_snapshot.dlssEnabled},
                {"restirEnabled", m_snapshot.restirEnabled},
                {"sharcEnabled", m_snapshot.sharcEnabled},
                {"dlssQualityIndex", m_snapshot.dlssQualityIndex},
                {"rtShadowManual", m_snapshot.rtShadowManual},
                {"sharcLogBase", m_snapshot.sharcLogBase},
                {"sharcLevelBias", m_snapshot.sharcLevelBias},
                {"sharcStaleFrameMax", m_snapshot.sharcStaleFrameMax}
            };
        }

        // Post-process state
        if (target == "postprocess" || target == "all") {
            auto& pp = m_snapshot.postProc;
            resp["postprocess"] = {
                {"bloomThreshold", pp.bloomThreshold},
                {"bloomIntensity", pp.bloomIntensity},
                {"exposure", pp.exposure},
                {"gamma", pp.gamma}
            };
        }

        // Allow game-specific targets via registered get_state handler
        {
            auto it = m_commandHandlers.find("get_state");
            if (it != m_commandHandlers.end()) {
                nlohmann::json ext = it->second(cmd);
                for (auto& [k, v] : ext.items())
                    if (k != "ok") resp[k] = v;
            }
        }

        return resp;
    }

    if (type == "screenshot" || type == "observe" ||
        type == "screenshot_depth" || type == "screenshot_normals") {
        std::string path = cmd.value("path", "screenshots/capture.png");

        ScreenshotMode mode = ScreenshotMode::Color;
        if (type == "screenshot_depth")   mode = ScreenshotMode::Depth;
        if (type == "screenshot_normals") mode = ScreenshotMode::Normals;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_screenshotPath    = path;
            m_screenshotMode    = mode;
            m_screenshotDone    = false;
            m_screenshotPending = true;
        }

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_screenshotCV.wait_for(lock, std::chrono::seconds(5),
                [this] { return m_screenshotDone; });
        }

        nlohmann::json resp;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_screenshotDone && m_screenshotOk) {
                resp["ok"]   = true;
                resp["path"] = path;
                resp["size"] = { m_screenshotWidth, m_screenshotHeight };
            } else {
                resp["ok"]    = false;
                resp["error"] = "screenshot failed or timed out";
            }
        }

        if (type == "observe") {
            std::lock_guard<std::mutex> lock(m_mutex);
            resp["fps"]    = m_snapshot.frame.fps;
            resp["dt"]     = m_snapshot.frame.dt;
            resp["time"]   = m_snapshot.frame.time;
            resp["width"]  = m_snapshot.frame.width;
            resp["height"] = m_snapshot.frame.height;
            resp["gpu"]    = m_snapshot.frame.gpu;
        }

        return resp;
    }

    if (type == "reload") {
        m_reloadPending = true;
        return {{"ok", true}, {"reload", "requested"}};
    }

    // ── Batch command mode (dispatch array of sub-commands) ──
    if (type == "batch") {
        if (!cmd.contains("commands") || !cmd["commands"].is_array())
            return {{"ok", false}, {"error", "batch requires 'commands' array"}};
        nlohmann::json results = nlohmann::json::array();
        for (auto& sub : cmd["commands"])
            results.push_back(processCommand(sub));
        return {{"ok", true}, {"results", results}};
    }

    // ── Check registered server-thread handlers ──
    {
        auto it = m_commandHandlers.find(type);
        if (it != m_commandHandlers.end())
            return it->second(cmd);
    }

    // ── Write commands (queued to main thread) ──

    // Built-in main-thread commands
    if (type == "inject_key" || type == "inject_mouse" || type == "inject_scroll" ||
        type == "inject_click" || type == "inject_key_hold" || type == "inject_key_release" ||
        type == "set_state" || type == "toggle_feature" || type == "set_admin" ||
        type == "save_scene" || type == "load_scene" || type == "reset_scene") {

        auto pc = std::make_unique<PendingCommand>();
        pc->cmd = cmd;
        auto future = pc->response.get_future();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingCommands.push_back(std::move(*pc));
        }

        // Wait for main thread to execute
        if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
            return future.get();
        }
        return {{"ok", false}, {"error", "command timed out waiting for main thread"}};
    }

    // Check registered main-thread handlers
    {
        auto it = m_mainThreadHandlers.find(type);
        if (it != m_mainThreadHandlers.end()) {
            auto pc = std::make_unique<PendingCommand>();
            pc->cmd = cmd;
            auto future = pc->response.get_future();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingCommands.push_back(std::move(*pc));
            }

            if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
                return future.get();
            }
            return {{"ok", false}, {"error", "command timed out waiting for main thread"}};
        }
    }

    return {{"ok", false}, {"error", "unknown command: " + type}};
}

// ── Main-thread command execution ───────────────────────────────

nlohmann::json DevServer::executeMainThreadCommand(const nlohmann::json& cmd)
{
    std::string type = cmd.value("cmd", "");

    if (type == "inject_key") {
        std::string keyName = cmd.value("key", "");
        int code = keyNameToGLFW(keyName);
        if (code < 0)
            return {{"ok", false}, {"error", "unknown key: " + keyName}};
        if (m_input) m_input->injectKey(code);
        return {{"ok", true}, {"key", keyName}};
    }

    if (type == "inject_mouse") {
        float dx = cmd.value("dx", 0.f);
        float dy = cmd.value("dy", 0.f);
        if (m_input) m_input->injectMouseDelta(dx, dy);
        return {{"ok", true}, {"dx", dx}, {"dy", dy}};
    }

    if (type == "inject_scroll") {
        float delta = cmd.value("delta", 0.f);
        if (m_input) m_input->injectScroll(delta);
        return {{"ok", true}, {"delta", delta}};
    }

    if (type == "inject_click") {
        float x = cmd.value("x", 0.f);
        float y = cmd.value("y", 0.f);
        int button = cmd.value("button", 0);
        if (m_input) m_input->injectMouseClick(x, y, button);
        return {{"ok", true}, {"x", x}, {"y", y}, {"button", button}};
    }

    if (type == "inject_key_hold") {
        std::string keyName = cmd.value("key", "");
        int code = keyNameToGLFW(keyName);
        if (code < 0)
            return {{"ok", false}, {"error", "unknown key: " + keyName}};
        int durationMs = cmd.value("duration_ms", 0);
        if (durationMs <= 0)
            return {{"ok", false}, {"error", "duration_ms must be > 0"}};
        // Replace any existing hold for this key
        m_keyHolds.erase(std::remove_if(m_keyHolds.begin(), m_keyHolds.end(),
            [code](const KeyHoldEntry& e) { return e.keyCode == code; }),
            m_keyHolds.end());
        m_keyHolds.push_back({code,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs)});
        if (m_input) m_input->injectKey(code);
        return {{"ok", true}, {"key", keyName}, {"duration_ms", durationMs}};
    }

    if (type == "inject_key_release") {
        std::string keyName = cmd.value("key", "");
        int code = keyNameToGLFW(keyName);
        if (code < 0)
            return {{"ok", false}, {"error", "unknown key: " + keyName}};
        m_keyHolds.erase(std::remove_if(m_keyHolds.begin(), m_keyHolds.end(),
            [code](const KeyHoldEntry& e) { return e.keyCode == code; }),
            m_keyHolds.end());
        return {{"ok", true}, {"key", keyName}};
    }

    // ── Feature toggles ────────────────────────────────────────
    if (type == "toggle_feature") {
        std::string feature = cmd.value("feature", "");
        if (feature == "dlss" && m_bindings.dlssEnabled) {
            *m_bindings.dlssEnabled = !*m_bindings.dlssEnabled;
            return {{"ok", true}, {"feature", feature}, {"enabled", *m_bindings.dlssEnabled}};
        }
        if (feature == "restir" && m_bindings.restirEnabled) {
            *m_bindings.restirEnabled = !*m_bindings.restirEnabled;
            return {{"ok", true}, {"feature", feature}, {"enabled", *m_bindings.restirEnabled}};
        }
        if (feature == "sharc" && m_bindings.sharcEnabled) {
            *m_bindings.sharcEnabled = !*m_bindings.sharcEnabled;
            return {{"ok", true}, {"feature", feature}, {"enabled", *m_bindings.sharcEnabled}};
        }
        return {{"ok", false}, {"error", "unknown feature: " + feature}};
    }

    if (type == "set_admin") {
        bool open = cmd.value("open", true);
        if (m_bindings.adminOpen) {
            *m_bindings.adminOpen = open;
            return {{"ok", true}, {"adminOpen", open}};
        }
        return {{"ok", false}, {"error", "adminOpen binding not set"}};
    }

    // ── set_state: engine-generic targets ──────────────────────
    if (type == "set_state") {
        std::string target = cmd.value("target", "");
        std::string field  = cmd.value("field", "");

        if (target == "postprocess" && m_bindings.postProc) {
            auto& pp = *m_bindings.postProc;
            float fv = cmd.value("value", 0.f);
            if      (field == "bloomThreshold") pp.bloomThreshold = fv;
            else if (field == "bloomIntensity") pp.bloomIntensity = fv;
            else if (field == "exposure")       pp.exposure = fv;
            else if (field == "gamma")          pp.gamma = fv;
            else return nlohmann::json({{"ok", false}, {"error", "unknown postprocess field: " + field}});
            return {{"ok", true}, {"target", target}, {"field", field}};
        }

        if (target == "render") {
            auto v = cmd.value("value", nlohmann::json());
            if (field == "dlssQualityIndex" && m_bindings.dlssQualityIndex)
                *m_bindings.dlssQualityIndex = v.get<int>();
            else if (field == "rtShadowManual" && m_bindings.rtShadowManual)
                *m_bindings.rtShadowManual = v.get<bool>();
            else if (field == "sharcLogBase" && m_bindings.sharcLogBase)
                *m_bindings.sharcLogBase = v.get<float>();
            else if (field == "sharcLevelBias" && m_bindings.sharcLevelBias)
                *m_bindings.sharcLevelBias = v.get<float>();
            else if (field == "sharcStaleFrameMax" && m_bindings.sharcStaleFrameMax)
                *m_bindings.sharcStaleFrameMax = v.get<uint32_t>();
            else if (field == "gpuProfilingEnabled" && m_bindings.gpuProfilingEnabled)
                *m_bindings.gpuProfilingEnabled = v.get<bool>();
            else return nlohmann::json({{"ok", false}, {"error", "unknown render field: " + field}});
            return {{"ok", true}, {"target", target}, {"field", field}};
        }

        // Allow game-specific targets via registered set_state handler
        {
            auto it = m_mainThreadHandlers.find("set_state");
            if (it != m_mainThreadHandlers.end())
                return it->second(cmd);
        }

        return {{"ok", false}, {"error", "unknown target: " + target}};
    }

    // ── Scene persistence ──────────────────────────────────────
    if (type == "save_scene") {
        if (m_bindings.onSaveScene) { m_bindings.onSaveScene(); return {{"ok", true}}; }
        return {{"ok", false}, {"error", "save callback not set"}};
    }
    if (type == "load_scene") {
        if (m_bindings.onLoadScene) { m_bindings.onLoadScene(); return {{"ok", true}}; }
        return {{"ok", false}, {"error", "load callback not set"}};
    }
    if (type == "reset_scene") {
        if (m_bindings.onResetScene) { m_bindings.onResetScene(); return {{"ok", true}}; }
        return {{"ok", false}, {"error", "reset callback not set"}};
    }

    // ── Check registered main-thread handlers ──
    {
        auto it = m_mainThreadHandlers.find(type);
        if (it != m_mainThreadHandlers.end())
            return it->second(cmd);
    }

    return {{"ok", false}, {"error", "unknown main-thread command: " + type}};
}

} // namespace sv
