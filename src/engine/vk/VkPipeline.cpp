// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkPipeline.h"
#include <cstdio>

namespace sv {

VkPipeBuilder& VkPipeBuilder::setShaders(VkShaderModule vert, VkShaderModule frag)
{
    m_vert = vert;
    m_frag = frag;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setVertexBinding(uint32_t stride,
    const VkVertexInputAttributeDescription* attrs, uint32_t attrCount)
{
    m_vertBinding.binding   = 0;
    m_vertBinding.stride    = stride;
    m_vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    m_vertAttrs.assign(attrs, attrs + attrCount);
    return *this;
}

VkPipeBuilder& VkPipeBuilder::addInstanceBinding(uint32_t stride,
    const VkVertexInputAttributeDescription* attrs, uint32_t attrCount)
{
    m_instBinding.binding   = 1;
    m_instBinding.stride    = stride;
    m_instBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    m_instAttrs.assign(attrs, attrs + attrCount);
    m_hasInstBinding = true;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setTopology(VkPrimitiveTopology topo)
{
    m_topology = topo;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setCullMode(VkCullModeFlags cull, VkFrontFace front)
{
    m_cullMode  = cull;
    m_frontFace = front;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setDepthTest(bool enable, bool write)
{
    m_depthTest  = enable;
    m_depthWrite = write;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setDepthCompareOp(VkCompareOp op)
{
    m_depthCompareOp = op;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setColorFormat(VkFormat format)
{
    m_colorFormats.clear();
    m_colorFormats.push_back(format);
    return *this;
}

VkPipeBuilder& VkPipeBuilder::addColorFormat(VkFormat format)
{
    m_colorFormats.push_back(format);
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setDepthFormat(VkFormat format)
{
    m_depthFormat = format;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setLayout(VkPipelineLayout layout)
{
    m_layout = layout;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setPolygonMode(VkPolygonMode mode)
{
    m_polyMode = mode;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setDepthOnly(bool enable)
{
    m_depthOnly = enable;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setDepthBias(float constant, float slope)
{
    m_depthBiasEnable   = true;
    m_depthBiasConstant = constant;
    m_depthBiasSlope    = slope;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setBlendEnabled(bool enable)
{
    m_blendEnable = enable;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setTessShaders(VkShaderModule tesc, VkShaderModule tese)
{
    m_tesc = tesc;
    m_tese = tese;
    return *this;
}

VkPipeBuilder& VkPipeBuilder::setPatchControlPoints(uint32_t n)
{
    m_patchControlPoints = n;
    return *this;
}

VkPipeline VkPipeBuilder::build(VkDevice device, VkPipelineCache cache)
{
    // Shader stages (2 base + up to 2 tessellation)
    bool hasTess = (m_tesc != VK_NULL_HANDLE && m_tese != VK_NULL_HANDLE);
    VkPipelineShaderStageCreateInfo stages[4]{};
    uint32_t stageCount = 0;

    stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stageCount].module = m_vert;
    stages[stageCount].pName  = "main";
    stageCount++;

    if (hasTess) {
        stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        stages[stageCount].module = m_tesc;
        stages[stageCount].pName  = "main";
        stageCount++;

        stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        stages[stageCount].module = m_tese;
        stages[stageCount].pName  = "main";
        stageCount++;
    }

    stages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stageCount].module = m_frag;
    stages[stageCount].pName  = "main";
    stageCount++;

    // Vertex input
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // Vertex bindings (binding 0 = per-vertex, optional binding 1 = per-instance)
    VkVertexInputBindingDescription bindings[2];
    uint32_t bindingCount = 0;
    std::vector<VkVertexInputAttributeDescription> allAttrs;

    if (!m_vertAttrs.empty()) {
        bindings[bindingCount++] = m_vertBinding;
        allAttrs.insert(allAttrs.end(), m_vertAttrs.begin(), m_vertAttrs.end());
    }
    if (m_hasInstBinding && !m_instAttrs.empty()) {
        bindings[bindingCount++] = m_instBinding;
        allAttrs.insert(allAttrs.end(), m_instAttrs.begin(), m_instAttrs.end());
    }

    vertexInput.vertexBindingDescriptionCount   = bindingCount;
    vertexInput.pVertexBindingDescriptions      = bindingCount > 0 ? bindings : nullptr;
    vertexInput.vertexAttributeDescriptionCount = (uint32_t)allAttrs.size();
    vertexInput.pVertexAttributeDescriptions    = allAttrs.empty() ? nullptr : allAttrs.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = hasTess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : m_topology;

    // Tessellation state (only used when tessellation shaders are present)
    VkPipelineTessellationStateCreateInfo tessState{};
    tessState.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessState.patchControlPoints = m_patchControlPoints > 0 ? m_patchControlPoints : 3;

    // Dynamic viewport + scissor
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = m_polyMode;
    raster.cullMode    = m_cullMode;
    raster.frontFace   = m_frontFace;
    raster.lineWidth   = 1.0f;
    if (m_depthBiasEnable) {
        raster.depthBiasEnable         = VK_TRUE;
        raster.depthBiasConstantFactor = m_depthBiasConstant;
        raster.depthBiasSlopeFactor    = m_depthBiasSlope;
    }

    // Multisample (no MSAA)
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth/stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = m_depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = m_depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = m_depthCompareOp;

    // Color blend (one attachment state per color format)
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
    if (!m_depthOnly) {
        for (size_t i = 0; i < m_colorFormats.size(); i++) {
            VkPipelineColorBlendAttachmentState att{};
            att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            att.blendEnable         = m_blendEnable ? VK_TRUE : VK_FALSE;
            att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp        = VK_BLEND_OP_ADD;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            att.alphaBlendOp        = VK_BLEND_OP_ADD;
            blendAttachments.push_back(att);
        }
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = m_depthOnly ? 0u : (uint32_t)blendAttachments.size();
    colorBlend.pAttachments    = m_depthOnly ? nullptr : blendAttachments.data();

    // Dynamic states
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    // Dynamic rendering (Vulkan 1.3 — no VkRenderPass)
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = m_depthOnly ? 0u : (uint32_t)m_colorFormats.size();
    renderingInfo.pColorAttachmentFormats = m_depthOnly ? nullptr : m_colorFormats.data();
    renderingInfo.depthAttachmentFormat   = m_depthFormat;

    // Assemble pipeline
    VkGraphicsPipelineCreateInfo ci{};
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext               = &renderingInfo;
    ci.stageCount          = stageCount;
    ci.pStages             = stages;
    ci.pTessellationState  = hasTess ? &tessState : nullptr;
    ci.pVertexInputState   = &vertexInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState      = &viewportState;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState   = &multisample;
    ci.pDepthStencilState  = &depthStencil;
    ci.pColorBlendState    = &colorBlend;
    ci.pDynamicState       = &dynamicState;
    ci.layout              = m_layout;
    ci.renderPass          = VK_NULL_HANDLE; // Dynamic rendering

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, cache, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create graphics pipeline\n");
    }

    return pipeline;
}

} // namespace sv
