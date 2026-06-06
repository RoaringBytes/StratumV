// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ============================================================
// EngineBase.cpp — Layer 6: Engine lifecycle orchestration
// ============================================================

#include "EngineBase.h"

#include "AssetManifest.h"
#include "AssetWatcher.h"
#include "Config.h"
#include "DevServer.h"
#include "DLLLoader.h"
#include "EngineLog.h"
#include "SystemRegistry.h"

#include <chrono>

namespace sv {

// ═══════════════════════════════════════════════════════════════
// Construction / destruction
// ═══════════════════════════════════════════════════════════════

EngineBase::EngineBase()
    : m_config(std::make_unique<Config>())
    , m_assetWatcher(std::make_unique<AssetWatcher>())
    , m_dllLoader(std::make_unique<DLLLoader>())
    , m_devServer(std::make_unique<DevServer>())
    , m_assetManifest(std::make_unique<AssetManifest>())
{
}

EngineBase::~EngineBase() = default;

// ═══════════════════════════════════════════════════════════════
// run — full lifecycle: init -> loop -> shutdown
// ═══════════════════════════════════════════════════════════════

int EngineBase::run(int argc, char** argv)
{
    SV_LOG_INFO("Engine", "StratumV EngineBase starting");

    if (!initServices()) {
        SV_LOG_ERROR("Engine", "Failed to initialize engine services");
        return 1;
    }

    if (!onInit()) {
        SV_LOG_ERROR("Engine", "Game initialization failed");
        shutdownServices();
        return 1;
    }

    m_running     = true;
    m_initialized = true;
    SV_LOG_INFO("Engine", "Entering main loop");

    auto prev = std::chrono::steady_clock::now();

    while (m_running) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;

        // Engine service polling
        m_assetWatcher->checkAll();
        m_devServer->pollCommands();

        // Game frame — return false to exit
        if (!onFrame(dt)) {
            m_running = false;
        }
    }

    SV_LOG_INFO("Engine", "Shutting down");
    onShutdown();
    shutdownServices();
    m_initialized = false;

    return 0;
}

// ═══════════════════════════════════════════════════════════════
// initServices — Config, AssetWatcher, DLLLoader, DevServer
// ═══════════════════════════════════════════════════════════════

bool EngineBase::initServices()
{
    // Load config
    if (!m_configPath.empty()) {
        if (!m_config->loadFromFile(m_configPath)) {
            SV_LOG_WARN("Engine", "Config not found: %s — using defaults",
                        m_configPath.c_str());
            m_config->loadDefaults();
        }
        m_config->enableAutoReload(*m_assetWatcher);
    }

    // Snapshot world bounds from config
    refreshWorldBounds();

    // Load asset manifest if a path was provided
    if (!m_assetManifestPath.empty()) {
        if (!m_assetManifest->loadFromFile(m_assetManifestPath)) {
            SV_LOG_WARN("Engine", "AssetManifest load failed: %s",
                        m_assetManifestPath.c_str());
        }
    }

    // Init DLLLoader (registers plugin DLL path with AssetWatcher)
    if (!m_pluginDLLPath.empty()) {
        if (!m_dllLoader->init(m_pluginDLLPath, *m_assetWatcher)) {
            SV_LOG_WARN("Engine", "DLLLoader init failed for: %s",
                        m_pluginDLLPath.c_str());
        }
    }

    // Start DevServer
    if (m_devServerPort > 0) {
        if (!m_devServer->start(m_devServerPort)) {
            SV_LOG_WARN("Engine", "DevServer failed to start on port %u",
                        m_devServerPort);
        }
    }

    // Register EngineBase in the system graph
    SystemRegistry::get().registerCoreSystem(this);

    SV_LOG_INFO("Engine", "Engine services initialized");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// shutdownServices — reverse of initServices
// ═══════════════════════════════════════════════════════════════

void EngineBase::shutdownServices()
{
    m_dllLoader->shutdown(m_plugins);
    m_devServer->stop();
    m_assetWatcher->clear();

    SV_LOG_INFO("Engine", "Engine services shut down");
}

// ═══════════════════════════════════════════════════════════════
// Service accessors
// ═══════════════════════════════════════════════════════════════

Config&         EngineBase::config()         { return *m_config; }
AssetWatcher&   EngineBase::assetWatcher()   { return *m_assetWatcher; }
DLLLoader&      EngineBase::dllLoader()      { return *m_dllLoader; }
DevServer&      EngineBase::devServer()      { return *m_devServer; }
EngineLog&      EngineBase::engineLog()      { return EngineLog::get(); }
SystemRegistry& EngineBase::systemRegistry() { return SystemRegistry::get(); }

std::vector<IModularSystem*>& EngineBase::plugins() { return m_plugins; }

// ── World bounds ───────────────────────────────────────────────────

const WorldBounds& EngineBase::worldBounds() const { return m_worldBounds; }

void EngineBase::refreshWorldBounds()
{
    m_worldBounds = m_config->worldBounds();
}

// ── Asset manifest ─────────────────────────────────────────────────

const AssetManifest* EngineBase::assetManifest() const
{
    return m_assetManifest.get();
}

void EngineBase::setAssetManifestPath(const std::string& path)
{
    m_assetManifestPath = path;
}

// ═══════════════════════════════════════════════════════════════
// Configuration (call before run())
// ═══════════════════════════════════════════════════════════════

void EngineBase::setConfigPath(const std::string& path)     { m_configPath     = path; }
void EngineBase::setPluginDLLPath(const std::string& path)  { m_pluginDLLPath  = path; }
void EngineBase::setDevServerPort(uint16_t port)             { m_devServerPort  = port; }

// ═══════════════════════════════════════════════════════════════
// Screenshot capture — default stub (games override)
// ═══════════════════════════════════════════════════════════════

bool EngineBase::captureScreenshot(const std::string& path)
{
    SV_LOG_WARN("Engine", "captureScreenshot() not implemented — override in game engine");
    return false;
}

// ═══════════════════════════════════════════════════════════════
// Connectome metadata
// ═══════════════════════════════════════════════════════════════

NodeMeta EngineBase::getMeta() const
{
    return {
        "engine_base",                         // id
        "Engine Base",                         // label
        "",                                    // icon
        6,                                     // layer
        "#c0a0ff",                             // color
        "Engine lifecycle orchestration",      // simple
        "Config, AssetWatcher, DLLLoader, "
        "DevServer lifecycle + main loop",     // detail
        "src/engine/EngineBase.h,"
        "src/engine/EngineBase.cpp",           // files
        "Games subclass and override "
        "onInit/onShutdown/onFrame",           // constraints
        0.f,                                   // budgetMs
        "",                                    // lastTouched
        "config,asset_watcher,dll_loader,"
        "dev_server,engine_log"                // dependencies
    };
}

HealthReport EngineBase::getHealth() const
{
    return { m_initialized ? 100.0f : 0.0f, m_initialized };
}

} // namespace sv
