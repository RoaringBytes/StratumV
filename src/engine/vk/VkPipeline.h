// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vector>

namespace sv {

// Graphics pipeline builder for Vulkan 1.3 dynamic rendering
class VkPipeBuilder {
public:
    VkPipeBuilder& setShaders(VkShaderModule vert, VkShaderModule frag);
    VkPipeBuilder& setVertexBinding(uint32_t stride,
        const VkVertexInputAttributeDescription* attrs, uint32_t attrCount);
    VkPipeBuilder& setTopology(VkPrimitiveTopology topo);
    VkPipeBuilder& setCullMode(VkCullModeFlags cull, VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE);
    VkPipeBuilder& setDepthTest(bool enable, bool write);
    VkPipeBuilder& setDepthCompareOp(VkCompareOp op);
    VkPipeBuilder& setColorFormat(VkFormat format);
    VkPipeBuilder& addColorFormat(VkFormat format);
    VkPipeBuilder& setDepthFormat(VkFormat format);
    VkPipeBuilder& setLayout(VkPipelineLayout layout);
    VkPipeBuilder& setPolygonMode(VkPolygonMode mode);
    VkPipeBuilder& setDepthOnly(bool enable);
    VkPipeBuilder& setDepthBias(float constant, float slope);
    VkPipeBuilder& setBlendEnabled(bool enable);
    VkPipeBuilder& setTessShaders(VkShaderModule tesc, VkShaderModule tese);
    VkPipeBuilder& setPatchControlPoints(uint32_t n);
    // Add a second vertex binding for instanced data (binding 1, per-instance)
    VkPipeBuilder& addInstanceBinding(uint32_t stride,
        const VkVertexInputAttributeDescription* attrs, uint32_t attrCount);

    // Build the pipeline. `cache` is an optional VkPipelineCache handle;
    // pass VK_NULL_HANDLE to skip persistent caching.
    VkPipeline build(VkDevice device,
                     VkPipelineCache cache = VK_NULL_HANDLE);

private:
    VkShaderModule m_vert = VK_NULL_HANDLE;
    VkShaderModule m_frag = VK_NULL_HANDLE;
    VkShaderModule m_tesc = VK_NULL_HANDLE;
    VkShaderModule m_tese = VK_NULL_HANDLE;
    uint32_t       m_patchControlPoints = 0; // 0 = no tessellation

    VkVertexInputBindingDescription                m_vertBinding{};
    std::vector<VkVertexInputAttributeDescription>  m_vertAttrs;
    VkVertexInputBindingDescription                m_instBinding{};
    std::vector<VkVertexInputAttributeDescription>  m_instAttrs;
    bool                                            m_hasInstBinding = false;
    VkPrimitiveTopology m_topology   = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags     m_cullMode   = VK_CULL_MODE_BACK_BIT;
    VkFrontFace         m_frontFace  = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode       m_polyMode   = VK_POLYGON_MODE_FILL;
    bool                m_depthTest  = true;
    bool                m_depthWrite = true;
    VkCompareOp         m_depthCompareOp = VK_COMPARE_OP_LESS;
    std::vector<VkFormat> m_colorFormats = { VK_FORMAT_B8G8R8A8_SRGB };
    VkFormat            m_depthFormat = VK_FORMAT_D32_SFLOAT;
    VkPipelineLayout    m_layout     = VK_NULL_HANDLE;
    bool                m_depthOnly  = false;
    bool                m_depthBiasEnable = false;
    float               m_depthBiasConstant = 0.0f;
    float               m_depthBiasSlope    = 0.0f;
    bool                m_blendEnable = true;
};

} // namespace sv
