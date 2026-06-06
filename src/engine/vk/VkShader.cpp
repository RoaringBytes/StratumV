// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkShader.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <filesystem>
#include <fstream>
#include <cstdio>

namespace sv {

// ── glslang #include resolver ──────────────���────────────────────
class FileIncluder : public glslang::TShader::Includer {
public:
    FileIncluder(const std::string& shaderDir) : m_shaderDir(shaderDir) {}

    IncludeResult* includeLocal(const char* headerName, const char* includerName,
                                size_t /*inclusionDepth*/) override
    {
        // Try relative to the includer's directory first (for nested includes)
        if (includerName && includerName[0] != '\0') {
            std::string includerPath(includerName);
            auto lastSlash = includerPath.find_last_of("/\\");
            if (lastSlash != std::string::npos) {
                // Resolve search paths to find the includer's actual directory
                std::string includerDir = includerPath.substr(0, lastSlash);
                // Try includer's directory directly
                std::string path = m_shaderDir + "/" + includerDir + "/" + headerName;
                auto result = readFile(path, headerName);
                if (result) return result;
                // Try includer's directory from each search path
                for (auto& dir : VkShader::s_includePaths) {
                    path = dir + "/" + includerDir + "/" + headerName;
                    result = readFile(path, headerName);
                    if (result) return result;
                }
            }
        }

        // Try relative to shader directory
        std::string path = m_shaderDir + "/" + headerName;
        auto result = readFile(path, headerName);
        if (result) return result;

        // Then try each include path
        for (auto& dir : VkShader::s_includePaths) {
            path = dir + "/" + headerName;
            result = readFile(path, headerName);
            if (result) return result;
        }
        return nullptr;
    }

    IncludeResult* includeSystem(const char* headerName, const char* /*includerName*/,
                                 size_t /*inclusionDepth*/) override
    {
        // Search include paths only (not relative to shader)
        for (auto& dir : VkShader::s_includePaths) {
            std::string path = dir + "/" + headerName;
            auto result = readFile(path, headerName);
            if (result) return result;
        }
        return nullptr;
    }

    void releaseInclude(IncludeResult* result) override
    {
        if (result) {
            delete[] result->headerData;
            delete result;
        }
    }

private:
    // Make s_includePaths accessible from this nested class
    friend class VkShader;

    IncludeResult* readFile(const std::string& path, const char* headerName)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return nullptr;

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        size_t len = content.size();
        char* data = new char[len];
        memcpy(data, content.data(), len);

        return new IncludeResult(headerName, data, len, nullptr);
    }

    std::string m_shaderDir;
};

// ── Compiler lifecycle ──────────────────────────────────────────
void VkShader::initCompiler()
{
    glslang::InitializeProcess();
}

void VkShader::shutdownCompiler()
{
    glslang::FinalizeProcess();
}

void VkShader::addIncludePath(const std::string& dir)
{
    s_includePaths.push_back(dir);
}

// ── Public ──────────��──────────────────────────────���────────────
bool VkShader::loadFromFile(VkDevice device, const std::string& path, VkShaderStageFlagBits stage)
{
    m_path  = path;
    m_stage = stage;

    // Read source
    std::ifstream file(path);
    if (!file.is_open()) {
        fprintf(stderr, "[StratumV] Cannot open shader: %s\n", path.c_str());
        return false;
    }
    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // Extract shader directory for relative #include resolution
    std::string shaderDir;
    auto lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        shaderDir = path.substr(0, lastSlash);
    else
        shaderDir = ".";

    auto spirv = compileGLSL(source, path, shaderDir, stage);
    if (spirv.empty()) return false;

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode    = spirv.data();

    if (vkCreateShaderModule(device, &ci, nullptr, &m_module) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create shader module: %s\n", path.c_str());
        return false;
    }

    m_lastModified = getFileTimestamp(path);
    printf("[StratumV] Shader compiled: %s (%zu SPIR-V words)\n", path.c_str(), spirv.size());
    return true;
}

bool VkShader::checkReload(VkDevice device)
{
    if (m_path.empty() || m_module == VK_NULL_HANDLE) return false;

    int64_t ts = getFileTimestamp(m_path);
    if (ts == m_lastModified) return false;

    printf("[StratumV] Shader changed, recompiling: %s\n", m_path.c_str());

    // Try to recompile
    std::ifstream file(m_path);
    if (!file.is_open()) return false;

    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    std::string shaderDir;
    auto lastSlash = m_path.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        shaderDir = m_path.substr(0, lastSlash);
    else
        shaderDir = ".";

    auto spirv = compileGLSL(source, m_path, shaderDir, m_stage);
    if (spirv.empty()) {
        m_lastModified = ts; // Don't retry until next change
        return false;
    }

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode    = spirv.data();

    VkShaderModule newModule;
    if (vkCreateShaderModule(device, &ci, nullptr, &newModule) != VK_SUCCESS) {
        m_lastModified = ts;
        return false;
    }

    vkDestroyShaderModule(device, m_module, nullptr);
    m_module       = newModule;
    m_lastModified = ts;
    printf("[StratumV] Shader reloaded: %s\n", m_path.c_str());
    return true;
}

void VkShader::destroy(VkDevice device)
{
    if (m_module) {
        vkDestroyShaderModule(device, m_module, nullptr);
        m_module = VK_NULL_HANDLE;
    }
}

// ── Private ────────────────────────────────────────────��────────
std::vector<uint32_t> VkShader::compileGLSL(const std::string& source,
    const std::string& name, const std::string& shaderDir,
    VkShaderStageFlagBits stage)
{
    EShLanguage lang;
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:                  lang = EShLangVertex; break;
        case VK_SHADER_STAGE_FRAGMENT_BIT:                lang = EShLangFragment; break;
        case VK_SHADER_STAGE_COMPUTE_BIT:                 lang = EShLangCompute; break;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    lang = EShLangTessControl; break;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: lang = EShLangTessEvaluation; break;
        case VK_SHADER_STAGE_GEOMETRY_BIT:                lang = EShLangGeometry; break;
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:             lang = EShLangRayGen; break;
        case VK_SHADER_STAGE_MISS_BIT_KHR:               lang = EShLangMiss; break;
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:        lang = EShLangClosestHit; break;
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:            lang = EShLangAnyHit; break;
        default:
            fprintf(stderr, "[StratumV] Unsupported shader stage\n");
            return {};
    }

    glslang::TShader shader(lang);
    const char* src = source.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    const TBuiltInResource* resources = GetDefaultResources();
    FileIncluder includer(shaderDir);

    if (!shader.parse(resources, 450, false, EShMsgDefault, includer)) {
        fprintf(stderr, "[StratumV] Shader compile error (%s):\n%s\n", name.c_str(), shader.getInfoLog());
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        fprintf(stderr, "[StratumV] Shader link error (%s):\n%s\n", name.c_str(), program.getInfoLog());
        return {};
    }

    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);
    return spirv;
}

int64_t VkShader::getFileTimestamp(const std::string& path)
{
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return ftime.time_since_epoch().count();
}

} // namespace sv
