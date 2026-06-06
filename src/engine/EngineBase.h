// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ============================================================
// EngineBase.h — Layer 6: Abstract base class for game engines
// ============================================================
//
// Games subclass EngineBase and override onInit()/onShutdown()/onFrame()
// to implement their main loop.  EngineBase owns the common engine
// services (Config, AssetWatcher, DLLLoader, DevServer) and orchestrates
// their lifecycle.
//
// Usage:
//
//   class GameEngine : public sv::EngineBase {
//   protected:
//       bool onInit() override;
//       void onShutdown() override;
//       bool onFrame(float dt) override;
//   };
//
//   int main(int argc, char** argv) {
//       GameEngine engine;
//       return engine.run(argc, argv);
//   }
//
// Lifecycle order:
//   1. Constructor — call setConfigPath / setPluginDLLPath / setDevServerPort
//   2. run(argc, argv)
//      a. initServices() — Config, AssetWatcher, DLLLoader init, DevServer start
//      b. onInit()       — game creates window, Vulkan device, fills SystemContext
//      c. main loop      — assetWatcher.checkAll(), devServer.pollCommands(), onFrame(dt)
//      d. onShutdown()   — game teardown (save state, destroy GPU resources, window)
//      e. shutdownServices() — DLLLoader shutdown, DevServer stop, watcher clear
//   3. Destructor

#include "EngineSystem.h"
#include "Config.h"          // for sv::WorldBounds (small POD, safe to include)

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace sv {

// Forward declarations — include the actual headers in your .cpp
class AssetWatcher;
class DevServer;
class DLLLoader;
class EngineLog;
class SystemRegistry;
class IModularSystem;
class AssetManifest;

class EngineBase : public EngineSystem {
public:
    EngineBase();
    virtual ~EngineBase();

    EngineBase(const EngineBase&) = delete;
    EngineBase& operator=(const EngineBase&) = delete;

    // ── Entry point ────────────────────────────────────────────
    // Call from main().  Runs the full lifecycle (init -> loop -> shutdown).
    // Returns process exit code (0 = success).
    int run(int argc, char** argv);

    // ── Connectome ─────────────────────────────────────────────
    NodeMeta     getMeta()   const override;
    HealthReport getHealth() const override;

protected:
    // ── Game overrides (required) ──────────────────────────────

    // Called after engine services are initialized.
    // Game should: create window, init Vulkan, build RenderGraph,
    //              fill SystemContext, load DLL plugins via dllLoader().load().
    // Return false to abort startup.
    virtual bool onInit() = 0;

    // Called after the main loop exits, before engine services shut down.
    // Game should: save scene state, destroy render resources,
    //              destroy Vulkan device, destroy window.
    virtual void onShutdown() = 0;

    // Called each frame inside the main loop.
    // dt = seconds since last frame.
    // Return false to exit the main loop.
    virtual bool onFrame(float dt) = 0;

    // ── Screenshot capture ──────────────────────────────────────
    // Games override to implement swapchain readback + PNG encode.
    // Default returns false.  Wire into SystemContext.captureScreenshot.
    virtual bool captureScreenshot(const std::string& path);

    // ── Engine services ────────────────────────────────────────
    // Available between initServices() and shutdownServices() — i.e.
    // inside onInit(), onFrame(), and onShutdown().

    Config&         config();
    AssetWatcher&   assetWatcher();
    DLLLoader&      dllLoader();
    DevServer&      devServer();
    EngineLog&      engineLog();          // convenience — returns singleton
    SystemRegistry& systemRegistry();     // convenience — returns singleton

    // ── World bounds ───────────────────────────────────────────────
    // Snapshot of config's "world.boundsMin/Max", refreshed on
    // initServices() and refreshWorldBounds(). Games point
    // SystemContext::worldBounds at this.
    const WorldBounds& worldBounds() const;
    void               refreshWorldBounds();   // re-read from Config

    // ── Asset manifest ─────────────────────────────────────────────
    // Engine-owned preload list. Populated by setAssetManifestPath()
    // + initServices(). Games point SystemContext::assetManifest at
    // this (may be null if no path was set).
    const AssetManifest* assetManifest() const;
    void setAssetManifestPath(const std::string& path);

    // DLL plugin instances managed by DLLLoader
    std::vector<IModularSystem*>& plugins();

    // ── Configuration (call before run()) ──────────────────────

    // Path to JSON config file.  Default: "config.json".
    void setConfigPath(const std::string& path);

    // Path to the game's plugin DLL.  If empty, DLLLoader is not initialized.
    void setPluginDLLPath(const std::string& path);

    // DevServer debug socket port.  Default: 0 (DISABLED).  The loopback
    // debug socket is opt-in so shipped games never expose it by default;
    // call setDevServerPort(9999) during setup to enable it for development.
    void setDevServerPort(uint16_t port);

private:
    bool initServices();
    void shutdownServices();

    // Owned engine services (allocated in constructor, destroyed in destructor)
    std::unique_ptr<Config>        m_config;
    std::unique_ptr<AssetWatcher>  m_assetWatcher;
    std::unique_ptr<DLLLoader>     m_dllLoader;
    std::unique_ptr<DevServer>     m_devServer;
    std::unique_ptr<AssetManifest> m_assetManifest;

    // Snapshot of world bounds. Refreshed from Config on init.
    WorldBounds m_worldBounds;

    // DLL plugin instances
    std::vector<IModularSystem*> m_plugins;

    // Settings (set before run())
    std::string m_configPath     = "config.json";
    std::string m_pluginDLLPath;
    std::string m_assetManifestPath;
    uint16_t    m_devServerPort  = 0;        // 0 = DevServer disabled (opt-in)

    bool m_running     = false;
    bool m_initialized = false;
};

} // namespace sv
