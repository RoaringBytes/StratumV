// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "PipelineRegistry.h"
#include <cstdio>
#include <functional>

namespace sv {

// ═══════════════════════════════════════════════════════════════════
// init / shutdown
// ═══════════════════════════════════════════════════════════════════

void PipelineRegistry::init(VkDevice device, const std::string& shaderDir)
{
    m_device    = device;
    m_shaderDir = shaderDir;
    m_pipelineCachePath.clear();
    printf("[StratumV] PipelineRegistry initialized (shader dir: %s)\n", shaderDir.c_str());
}

void PipelineRegistry::init(VkDevice           device,
                            const std::string& shaderDir,
                            VkPhysicalDevice   physicalDevice,
                            const std::string& cacheFilePath)
{
    m_device            = device;
    m_shaderDir         = shaderDir;
    m_pipelineCachePath = cacheFilePath;

    if (!cacheFilePath.empty() && physicalDevice != VK_NULL_HANDLE) {
        m_pipelineCache.load(device, physicalDevice, cacheFilePath);
    }
    printf("[StratumV] PipelineRegistry initialized (shader dir: %s, pipeline cache: %s)\n",
           shaderDir.c_str(),
           cacheFilePath.empty() ? "disabled" : cacheFilePath.c_str());
}

void PipelineRegistry::shutdown()
{
    destroyAll();
    // Destroy cached shader modules
    for (auto& [path, shader] : m_shaderCache)
        shader.destroy(m_device);
    m_shaderCache.clear();

    // Persist VkPipelineCache before destroying it.
    if (!m_pipelineCachePath.empty()
        && m_pipelineCache.handle() != VK_NULL_HANDLE)
    {
        m_pipelineCache.save(m_device, m_pipelineCachePath);
    }
    m_pipelineCache.destroy(m_device);
}

// ═══════════════════════════════════════════════════════════════════
// getGraphics — lazy create or return cached
// ═══════════════════════════════════════════════════════════════════

VkPipeline PipelineRegistry::getGraphics(const GraphicsPipelineDesc& desc)
{
    size_t h = hashDesc(desc);
    auto it = m_graphicsCache.find(h);
    if (it != m_graphicsCache.end() && it->second.pipeline != VK_NULL_HANDLE) {
        // Full equality check to guard against hash collisions
        if (it->second.desc == desc)
            return it->second.pipeline;
        fprintf(stderr, "[StratumV] PipelineRegistry: HASH COLLISION on graphics pipeline (hash=%zu)\n", h);
    }

    VkPipeline pipeline = buildGraphics(desc);
    if (pipeline) {
        m_graphicsCache[h] = { pipeline, desc };
        printf("[StratumV] PipelineRegistry: cached graphics pipeline (hash=%zu, total=%zu)\n",
               h, m_graphicsCache.size());
    }
    return pipeline;
}

// ═══════════════════════════════════════════════════════════════════
// getCompute — lazy create or return cached
// ═══════════════════════════════════════════════════════════════════

VkComputePipe PipelineRegistry::getCompute(const ComputePipelineDesc& desc)
{
    size_t h = hashDesc(desc);
    auto it = m_computeCache.find(h);
    if (it != m_computeCache.end() && it->second.pipe.pipeline != VK_NULL_HANDLE) {
        if (it->second.desc == desc)
            return it->second.pipe;
        fprintf(stderr, "[StratumV] PipelineRegistry: HASH COLLISION on compute pipeline (hash=%zu)\n", h);
    }

    VkComputePipe pipe = buildCompute(desc);
    if (pipe.pipeline) {
        m_computeCache[h] = { pipe, desc };
        printf("[StratumV] PipelineRegistry: cached compute pipeline (hash=%zu, total=%zu)\n",
               h, m_computeCache.size());
    }
    return pipe;
}

// ═══════════════════════════════════════════════════════════════════
// invalidateShader — destroy pipelines that reference a given shader
// ═══════════════════════════════════════════════════════════════════

void PipelineRegistry::invalidateShader(const std::string& shaderPath)
{
    // Invalidate graphics pipelines
    for (auto it = m_graphicsCache.begin(); it != m_graphicsCache.end(); ) {
        auto& d = it->second.desc;
        if (d.vertShaderPath == shaderPath || d.fragShaderPath == shaderPath ||
            d.tescShaderPath == shaderPath || d.tesePath == shaderPath) {
            if (it->second.pipeline)
                vkDestroyPipeline(m_device, it->second.pipeline, nullptr);
            it = m_graphicsCache.erase(it);
        } else {
            ++it;
        }
    }

    // Invalidate compute pipelines
    for (auto it = m_computeCache.begin(); it != m_computeCache.end(); ) {
        if (it->second.desc.shaderPath == shaderPath) {
            it->second.pipe.destroy(m_device);
            it = m_computeCache.erase(it);
        } else {
            ++it;
        }
    }

    // Reload the shader module itself
    auto sit = m_shaderCache.find(shaderPath);
    if (sit != m_shaderCache.end()) {
        VkShaderStageFlagBits stage = sit->second.stage();
        sit->second.destroy(m_device);
        sit->second.loadFromFile(m_device, shaderPath, stage);
    }
}

// ═══════════════════════════════════════════════════════════════════
// destroyAll
// ═══════════════════════════════════════════════════════════════════

void PipelineRegistry::destroyAll()
{
    for (auto& [hash, cached] : m_graphicsCache) {
        if (cached.pipeline)
            vkDestroyPipeline(m_device, cached.pipeline, nullptr);
    }
    m_graphicsCache.clear();

    for (auto& [hash, cached] : m_computeCache)
        cached.pipe.destroy(m_device);
    m_computeCache.clear();
}

// ═══════════════════════════════════════════════════════════════════
// buildGraphics — compile shaders + VkPipeBuilder
// ═══════════════════════════════════════════════════════════════════

VkPipeline PipelineRegistry::buildGraphics(const GraphicsPipelineDesc& desc)
{
    VkShaderModule vert = getOrLoadShader(desc.vertShaderPath, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = getOrLoadShader(desc.fragShaderPath, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!vert || !frag) {
        fprintf(stderr, "[StratumV] PipelineRegistry: failed to load shaders (%s, %s)\n",
                desc.vertShaderPath.c_str(), desc.fragShaderPath.c_str());
        return VK_NULL_HANDLE;
    }

    VkPipeBuilder builder;
    builder.setShaders(vert, frag);

    // Tessellation (optional)
    if (!desc.tescShaderPath.empty() && !desc.tesePath.empty()) {
        VkShaderModule tesc = getOrLoadShader(desc.tescShaderPath, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
        VkShaderModule tese = getOrLoadShader(desc.tesePath, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
        if (tesc && tese) {
            builder.setTessShaders(tesc, tese);
            builder.setPatchControlPoints(desc.patchControlPoints);
        }
    }

    // Vertex layout
    if (desc.vertexStride > 0 && !desc.vertexAttrs.empty()) {
        builder.setVertexBinding(desc.vertexStride, desc.vertexAttrs.data(),
                                 (uint32_t)desc.vertexAttrs.size());
    }
    // Instance layout (binding 1, per-instance)
    if (desc.instanceStride > 0 && !desc.instanceAttrs.empty()) {
        builder.addInstanceBinding(desc.instanceStride, desc.instanceAttrs.data(),
                                    (uint32_t)desc.instanceAttrs.size());
    }

    builder.setTopology(desc.topology);
    builder.setCullMode(desc.cullMode, desc.frontFace);
    builder.setDepthTest(desc.depthTest, desc.depthWrite);
    builder.setDepthCompareOp(desc.depthCompareOp);
    builder.setPolygonMode(desc.polygonMode);
    builder.setBlendEnabled(desc.blendEnabled);

    if (desc.depthOnly) {
        builder.setDepthOnly(true);
    } else {
        // Color formats
        if (!desc.colorFormats.empty()) {
            builder.setColorFormat(desc.colorFormats[0]);
            for (size_t i = 1; i < desc.colorFormats.size(); i++)
                builder.addColorFormat(desc.colorFormats[i]);
        }
    }

    builder.setDepthFormat(desc.depthFormat);

    if (desc.depthBias)
        builder.setDepthBias(desc.depthBiasConstant, desc.depthBiasSlope);

    builder.setLayout(desc.layout);

    return builder.build(m_device, m_pipelineCache.handle());
}

// ═══════════════════════════════════════════════════════════════════
// buildCompute
// ═══════════════════════════════════════════════════════════════════

VkComputePipe PipelineRegistry::buildCompute(const ComputePipelineDesc& desc)
{
    VkShaderModule comp = getOrLoadShader(desc.shaderPath, VK_SHADER_STAGE_COMPUTE_BIT);
    if (!comp) {
        fprintf(stderr, "[StratumV] PipelineRegistry: failed to load compute shader (%s)\n",
                desc.shaderPath.c_str());
        return {};
    }
    return VkComputePipe::create(m_device, comp, desc.layout);
}

// ═══════════════════════════════════════════════════════════════════
// getOrLoadShader — load shader from file, cache by path
// ═══════════════════════════════════════════════════════════════════

VkShaderModule PipelineRegistry::getOrLoadShader(const std::string& path, VkShaderStageFlagBits stage)
{
    auto it = m_shaderCache.find(path);
    if (it != m_shaderCache.end())
        return it->second.module();

    VkShader shader;
    if (!shader.loadFromFile(m_device, path, stage)) {
        fprintf(stderr, "[StratumV] PipelineRegistry: shader load failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModule mod = shader.module();
    m_shaderCache.emplace(path, std::move(shader));
    return mod;
}

// ═══════════════════════════════════════════════════════════════════
// Hash functions
// ═══════════════════════════════════════════════════════════════════

size_t PipelineRegistry::hashDesc(const GraphicsPipelineDesc& desc) const
{
    size_t h = 0;
    hashCombine(h, std::hash<std::string>{}(desc.vertShaderPath));
    hashCombine(h, std::hash<std::string>{}(desc.fragShaderPath));
    hashCombine(h, std::hash<std::string>{}(desc.tescShaderPath));
    hashCombine(h, std::hash<std::string>{}(desc.tesePath));
    hashCombine(h, (size_t)desc.patchControlPoints);
    hashCombine(h, (size_t)desc.topology);
    hashCombine(h, (size_t)desc.cullMode);
    hashCombine(h, (size_t)desc.frontFace);
    hashCombine(h, (size_t)desc.polygonMode);
    hashCombine(h, (size_t)desc.depthTest);
    hashCombine(h, (size_t)desc.depthWrite);
    hashCombine(h, (size_t)desc.depthCompareOp);
    for (auto fmt : desc.colorFormats)
        hashCombine(h, (size_t)fmt);
    hashCombine(h, (size_t)desc.depthFormat);
    hashCombine(h, (size_t)desc.depthOnly);
    hashCombine(h, (size_t)desc.depthBias);
    hashCombine(h, std::hash<float>{}(desc.depthBiasConstant));
    hashCombine(h, std::hash<float>{}(desc.depthBiasSlope));
    hashCombine(h, (size_t)desc.blendEnabled);
    hashCombine(h, (size_t)desc.vertexStride);
    for (auto& attr : desc.vertexAttrs) {
        hashCombine(h, (size_t)attr.location);
        hashCombine(h, (size_t)attr.format);
        hashCombine(h, (size_t)attr.offset);
    }
    hashCombine(h, (size_t)desc.instanceStride);
    for (auto& attr : desc.instanceAttrs) {
        hashCombine(h, (size_t)attr.location);
        hashCombine(h, (size_t)attr.format);
        hashCombine(h, (size_t)attr.offset);
    }
    hashCombine(h, (size_t)desc.layout);
    return h;
}

size_t PipelineRegistry::hashDesc(const ComputePipelineDesc& desc) const
{
    size_t h = 0;
    hashCombine(h, std::hash<std::string>{}(desc.shaderPath));
    hashCombine(h, (size_t)desc.layout);
    return h;
}

} // namespace sv
