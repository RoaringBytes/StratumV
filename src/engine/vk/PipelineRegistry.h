// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "VkPipeline.h"
#include "VkComputePipeline.h"
#include "VkShader.h"
#include "PipelineCache.h"
#include <unordered_map>
#include <string>
#include <vector>

namespace sv {

// ── Hashable pipeline descriptors ────────────────────────────────

struct GraphicsPipelineDesc {
    std::string     vertShaderPath;
    std::string     fragShaderPath;
    std::string     tescShaderPath;   // optional
    std::string     tesePath;         // optional
    uint32_t        patchControlPoints = 0;
    VkPrimitiveTopology topology    = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags     cullMode    = VK_CULL_MODE_BACK_BIT;
    VkFrontFace         frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode       polygonMode = VK_POLYGON_MODE_FILL;
    bool depthTest = true, depthWrite = true;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    bool depthOnly = false;
    bool depthBias = false;
    float depthBiasConstant = 0.f, depthBiasSlope = 0.f;
    bool blendEnabled = true;
    // Vertex layout (binding 0: per-vertex)
    uint32_t vertexStride = 0;
    std::vector<VkVertexInputAttributeDescription> vertexAttrs;
    // Optional instance layout (binding 1: per-instance)
    uint32_t instanceStride = 0;
    std::vector<VkVertexInputAttributeDescription> instanceAttrs;
    // Pipeline layout (not owned by registry)
    VkPipelineLayout layout = VK_NULL_HANDLE;

    bool operator==(const GraphicsPipelineDesc& o) const {
        return vertShaderPath == o.vertShaderPath && fragShaderPath == o.fragShaderPath
            && tescShaderPath == o.tescShaderPath && tesePath == o.tesePath
            && patchControlPoints == o.patchControlPoints
            && topology == o.topology && cullMode == o.cullMode
            && frontFace == o.frontFace && polygonMode == o.polygonMode
            && depthTest == o.depthTest && depthWrite == o.depthWrite
            && depthCompareOp == o.depthCompareOp
            && colorFormats == o.colorFormats && depthFormat == o.depthFormat
            && depthOnly == o.depthOnly && depthBias == o.depthBias
            && depthBiasConstant == o.depthBiasConstant
            && depthBiasSlope == o.depthBiasSlope
            && blendEnabled == o.blendEnabled && vertexStride == o.vertexStride
            && vertexAttrs.size() == o.vertexAttrs.size()
            && instanceStride == o.instanceStride
            && instanceAttrs.size() == o.instanceAttrs.size()
            && layout == o.layout;
    }
};

struct ComputePipelineDesc {
    std::string      shaderPath;
    VkPipelineLayout layout = VK_NULL_HANDLE;  // not owned

    bool operator==(const ComputePipelineDesc& o) const {
        return shaderPath == o.shaderPath && layout == o.layout;
    }
};

// ── Pipeline Registry ────────────────────────────────────────────

class PipelineRegistry {
public:
    void init(VkDevice device, const std::string& shaderDir);

    // Overload: also load a persistent VkPipelineCache blob
    // from `cacheFilePath`. The cache is validated against the current
    // GPU via `physicalDevice` and saved back to disk on shutdown().
    // Pass an empty `cacheFilePath` to disable persistence.
    void init(VkDevice           device,
              const std::string& shaderDir,
              VkPhysicalDevice   physicalDevice,
              const std::string& cacheFilePath);

    void shutdown();

    // Get or create. Returns cached pipeline if desc hash matches.
    VkPipeline    getGraphics(const GraphicsPipelineDesc& desc);
    VkComputePipe getCompute(const ComputePipelineDesc& desc);

    // Invalidate all pipelines that use the given shader path.
    // Next get*() call will rebuild them.
    void invalidateShader(const std::string& shaderPath);

    // Destroy all cached pipelines
    void destroyAll();

    size_t cachedGraphicsCount() const { return m_graphicsCache.size(); }
    size_t cachedComputeCount()  const { return m_computeCache.size(); }

    // Diagnostic accessor for the persistent pipeline cache.
    const PipelineCache& pipelineCache() const { return m_pipelineCache; }

private:
    struct CachedGraphicsPipeline {
        VkPipeline          pipeline = VK_NULL_HANDLE;
        GraphicsPipelineDesc desc;     // stored for rebuild on invalidation
    };

    struct CachedComputePipeline {
        VkComputePipe        pipe{};
        ComputePipelineDesc  desc;
    };

    VkDevice    m_device = VK_NULL_HANDLE;
    std::string m_shaderDir;

    // Persistent VkPipelineCache (load on init, save on shutdown)
    PipelineCache m_pipelineCache;
    std::string   m_pipelineCachePath;

    std::unordered_map<size_t, CachedGraphicsPipeline> m_graphicsCache;
    std::unordered_map<size_t, CachedComputePipeline>  m_computeCache;

    // Shader module cache (path → loaded shader)
    std::unordered_map<std::string, VkShader> m_shaderCache;

    size_t      hashDesc(const GraphicsPipelineDesc& desc) const;
    size_t      hashDesc(const ComputePipelineDesc& desc) const;
    VkPipeline  buildGraphics(const GraphicsPipelineDesc& desc);
    VkComputePipe buildCompute(const ComputePipelineDesc& desc);

    VkShaderModule getOrLoadShader(const std::string& path, VkShaderStageFlagBits stage);

    // Hash combine helper
    static void hashCombine(size_t& seed, size_t value) {
        seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
};

} // namespace sv
