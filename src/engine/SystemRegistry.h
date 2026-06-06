// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once
#include "EngineSystem.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <mutex>

namespace sv {

class SystemRegistry {
public:
    static SystemRegistry& get();

    void              registerCoreSystem(EngineSystem* sys);  // survives clear()
    void              registerSystem(EngineSystem* sys);      // DLL systems — wiped on clear()
    void              clear();  // clears DLL systems only (core systems persist)
    nlohmann::json    serializeGraph() const;
    nlohmann::json    serializeHealth() const;

private:
    mutable std::mutex          m_mutex;
    std::vector<EngineSystem*>  m_coreSystems;  // frozen-core wrappers (survive hot-reload)
    std::vector<EngineSystem*>  m_dllSystems;   // DLL plugins (cleared on hot-reload)
};

} // namespace sv
