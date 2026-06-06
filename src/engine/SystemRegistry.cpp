// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "SystemRegistry.h"

namespace sv {

SystemRegistry& SystemRegistry::get()
{
    static SystemRegistry instance;
    return instance;
}

void SystemRegistry::registerCoreSystem(EngineSystem* sys)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_coreSystems.push_back(sys);
}

void SystemRegistry::registerSystem(EngineSystem* sys)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dllSystems.push_back(sys);
}

void SystemRegistry::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dllSystems.clear();  // core systems persist
}

nlohmann::json SystemRegistry::serializeGraph() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    nlohmann::json nodes = nlohmann::json::array();
    auto emitNode = [&](const EngineSystem* sys) {
        NodeMeta m = sys->getMeta();
        nlohmann::json node;
        node["id"]          = m.id          ? m.id          : "";
        node["label"]       = m.label       ? m.label       : "";
        node["icon"]        = m.icon        ? m.icon        : "";
        node["layer"]       = m.layer;
        node["color"]       = m.color       ? m.color       : "";
        node["simple"]      = m.simple      ? m.simple      : "";
        node["detail"]      = m.detail      ? m.detail      : "";
        node["files"]       = m.files       ? m.files       : "";
        node["constraints"] = m.constraints ? m.constraints : "";
        node["budgetMs"]    = m.budgetMs;
        node["lastTouched"] = m.lastTouched ? m.lastTouched : "";
        nodes.push_back(node);
    };
    for (const auto* sys : m_coreSystems)
        emitNode(sys);
    for (const auto* sys : m_dllSystems)
        emitNode(sys);

    // Build links from dependency declarations
    nlohmann::json links = nlohmann::json::array();
    auto emitLinks = [&](const EngineSystem* sys) {
        NodeMeta m = sys->getMeta();
        if (!m.dependencies || !m.dependencies[0]) return;
        const char* id = m.id ? m.id : "";
        std::string deps(m.dependencies);
        size_t start = 0;
        while (start < deps.size()) {
            size_t comma = deps.find(',', start);
            if (comma == std::string::npos) comma = deps.size();
            std::string dep = deps.substr(start, comma - start);
            // trim
            while (!dep.empty() && dep.front() == ' ') dep.erase(dep.begin());
            while (!dep.empty() && dep.back() == ' ') dep.pop_back();
            if (!dep.empty()) {
                links.push_back({{"source", id}, {"target", dep}});
            }
            start = comma + 1;
        }
    };
    for (const auto* sys : m_coreSystems)
        emitLinks(sys);
    for (const auto* sys : m_dllSystems)
        emitLinks(sys);

    nlohmann::json globalConstraints = nlohmann::json::array();
    globalConstraints.push_back("Render core is frozen -- no new render passes without engine version bump");
    globalConstraints.push_back("All new features must be DLL plugins implementing IModularSystem");
    globalConstraints.push_back("DLL plugins must not include internal engine headers");
    globalConstraints.push_back("New plugins must implement serialize()/deserialize()");

    return {
        {"global_constraints", globalConstraints},
        {"nodes", nodes},
        {"links", links}
    };
}

nlohmann::json SystemRegistry::serializeHealth() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    nlohmann::json health = nlohmann::json::object();
    auto emitHealth = [&](const EngineSystem* sys) {
        NodeMeta m = sys->getMeta();
        HealthReport h = sys->getHealth();
        const char* id = (m.id && m.id[0]) ? m.id : "unknown";
        health[id] = { {"health", h.health}, {"active", h.active} };
    };
    for (const auto* sys : m_coreSystems)
        emitHealth(sys);
    for (const auto* sys : m_dllSystems)
        emitHealth(sys);
    return health;
}

} // namespace sv
