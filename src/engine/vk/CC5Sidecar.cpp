// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "CC5Sidecar.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>

namespace sv {

CC5MatMap parseCC5Sidecar(const std::string& fbxPath)
{
    CC5MatMap cc5Map;

    std::string jsonPath = fbxPath;
    auto dotPos = jsonPath.find_last_of('.');
    if (dotPos != std::string::npos)
        jsonPath = jsonPath.substr(0, dotPos) + ".json";

    std::ifstream jf(jsonPath);
    if (!jf.good()) return cc5Map;

    try {
        nlohmann::json root = nlohmann::json::parse(jf);
        // CC5 JSON: root[basename].Object[basename].Meshes[meshName].Materials[matName]
        for (auto& [topKey, topVal] : root.items()) {
            if (!topVal.contains("Object")) continue;
            for (auto& [objKey, objVal] : topVal["Object"].items()) {
                if (!objVal.contains("Meshes")) continue;
                for (auto& [meshKey, meshVal] : objVal["Meshes"].items()) {
                    if (!meshVal.contains("Materials")) continue;
                    for (auto& [matKey, matVal] : meshVal["Materials"].items()) {
                        CC5MatInfo info;
                        if (matVal.contains("Node Type") && matVal["Node Type"].is_string())
                            info.nodeType = matVal["Node Type"].get<std::string>();
                        if (matVal.contains("Two Side") && matVal["Two Side"].is_boolean())
                            info.twoSide = matVal["Two Side"].get<bool>();
                        // Texture paths from JSON Textures section
                        if (matVal.contains("Textures")) {
                            auto& texObj = matVal["Textures"];
                            if (texObj.contains("Opacity"))
                                info.hasOpacity = true;
                            for (auto& [texName, texData] : texObj.items()) {
                                if (texData.contains("Texture Path") && texData["Texture Path"].is_string()) {
                                    std::string tp = texData["Texture Path"].get<std::string>();
                                    if (!tp.empty())
                                        info.texturePaths[texName] = tp;
                                }
                            }
                        }
                        // Custom Shader: shader name + color variables
                        if (matVal.contains("Custom Shader")) {
                            auto& cs = matVal["Custom Shader"];
                            if (cs.contains("Shader Name") && cs["Shader Name"].is_string())
                                info.shaderName = cs["Shader Name"].get<std::string>();
                            if (cs.contains("Variable")) {
                                auto& vars = cs["Variable"];
                                if (vars.contains("RootColor")) {
                                    auto& rc = vars["RootColor"];
                                    if (rc.is_array() && rc.size() >= 3) {
                                        info.hasRootColor = true;
                                        info.rootColor = glm::vec3(
                                            rc[0].get<float>() / 255.0f,
                                            rc[1].get<float>() / 255.0f,
                                            rc[2].get<float>() / 255.0f);
                                    }
                                }
                                if (vars.contains("TipColor")) {
                                    auto& tc = vars["TipColor"];
                                    if (tc.is_array() && tc.size() >= 3) {
                                        info.hasTipColor = true;
                                        info.tipColor = glm::vec3(
                                            tc[0].get<float>() / 255.0f,
                                            tc[1].get<float>() / 255.0f,
                                            tc[2].get<float>() / 255.0f);
                                    }
                                }
                            }
                        }
                        cc5Map[matKey] = info;
                    }
                }
            }
        }
        int texPathCount = 0;
        for (auto& [k, v] : cc5Map)
            texPathCount += (int)v.texturePaths.size();
        printf("[CC5Sidecar] Parsed: %d materials, %d texture paths from '%s'\n",
               (int)cc5Map.size(), texPathCount, jsonPath.c_str());
    } catch (const std::exception& e) {
        printf("[CC5Sidecar] JSON parse failed: %s\n", e.what());
    }

    return cc5Map;
}

} // namespace sv
