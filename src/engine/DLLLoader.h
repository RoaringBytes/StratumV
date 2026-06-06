// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "IModularSystem.h"
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sv {

class AssetWatcher;

class DLLLoader {
public:
    // Initialize: set DLL path, register with asset watcher for change detection
    bool init(const std::string& dllPath, AssetWatcher& watcher);
    void shutdown(std::vector<IModularSystem*>& systems);

    // Check for DLL changes, perform reload cycle if needed.
    // IMPORTANT: Caller must ensure GPU is idle (vkDeviceWaitIdle) before calling.
    // Returns true if reload occurred.
    bool checkReload(const SystemContext& ctx,
                     std::vector<IModularSystem*>& systems);

    bool isLoaded() const { return m_handle != nullptr; }

    // Initial load (call once after init)
    bool load(const SystemContext& ctx,
              std::vector<IModularSystem*>& systems);

private:
    std::string m_dllPath;       // Original DLL path (build output)
    std::string m_shadowPath;    // Copy loaded at runtime (avoids lock)
    HMODULE     m_handle = nullptr;
    CreateSystemsFn  m_createFn  = nullptr;
    DestroySystemsFn m_destroyFn = nullptr;
    bool        m_pendingReload  = false;

    // Serialize + unload all systems
    std::vector<std::string> unloadSystems(std::vector<IModularSystem*>& systems);

    // Load DLL, create systems, optionally restore state
    bool loadSystems(const SystemContext& ctx,
                     std::vector<IModularSystem*>& systems,
                     const std::vector<std::string>& savedStates);

    // Copy DLL to shadow path before loading (avoids build lock)
    bool shadowCopy();

    // Resolve DLL path: CWD → exe-directory → original (for deferred build)
    static std::string resolveDLLPath(const std::string& path);
};

} // namespace sv
