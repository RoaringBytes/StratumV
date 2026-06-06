// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <string>
#include <vector>
#include <cstdint>

namespace sv {

class VkShader {
public:
    bool loadFromFile(VkDevice device, const std::string& path, VkShaderStageFlagBits stage);
    bool checkReload(VkDevice device); // returns true if shader was recompiled
    void destroy(VkDevice device);

    VkShaderModule        module() const { return m_module; }
    VkShaderStageFlagBits stage()  const { return m_stage; }

    // Call once at startup / shutdown
    static void initCompiler();
    static void shutdownCompiler();

    // Add a directory to the shader include search path (for #include resolution)
    static void addIncludePath(const std::string& dir);

    // Shader include search paths (accessed by FileIncluder in VkShader.cpp)
    static inline std::vector<std::string> s_includePaths;

private:
    static std::vector<uint32_t> compileGLSL(const std::string& source,
        const std::string& name, const std::string& shaderDir,
        VkShaderStageFlagBits stage);
    static int64_t getFileTimestamp(const std::string& path);

    VkShaderModule        m_module = VK_NULL_HANDLE;
    std::string           m_path;
    VkShaderStageFlagBits m_stage  = VK_SHADER_STAGE_VERTEX_BIT;
    int64_t               m_lastModified = 0;
};

} // namespace sv
