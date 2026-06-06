// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkComputePipeline.h"
#include <cstdio>

namespace sv {

VkComputePipe VkComputePipe::create(VkDevice device, VkShaderModule comp, VkPipelineLayout pipeLayout)
{
    VkComputePipe cp;
    cp.layout = pipeLayout;

    VkPipelineShaderStageCreateInfo stageCI{};
    stageCI.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stageCI.module = comp;
    stageCI.pName  = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = stageCI;
    ci.layout = pipeLayout;

    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &cp.pipeline) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create compute pipeline\n");
    }

    return cp;
}

void VkComputePipe::destroy(VkDevice device)
{
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
}

} // namespace sv
