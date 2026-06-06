// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkRTPipeline.h"
#include "VkContext.h"
#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>

namespace sv {

static uint32_t alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

VkRTPipeline VkRTPipeline::create(VkCtx& ctx, VkPipelineLayout layout,
    VkShaderModule rgen, VkShaderModule rmiss, VkShaderModule rchit,
    uint32_t maxRecursionDepth, bool allowClusterAS)
{
    VkDevice device = ctx.device();
    VkRTPipeline rt{};
    rt.layout = layout;

    // ── Shader stages ────────────────────────────────────────────
    VkPipelineShaderStageCreateInfo stages[3]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgen;
    stages[0].pName  = "main";

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmiss;
    stages[1].pName  = "main";

    stages[2].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage  = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchit;
    stages[2].pName  = "main";

    // ── Shader groups ────────────────────────────────────────────
    VkRayTracingShaderGroupCreateInfoKHR groups[3]{};

    // Group 0: raygen (GENERAL)
    groups[0].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader      = 0;
    groups[0].closestHitShader   = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 1: miss (GENERAL)
    groups[1].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader      = 1;
    groups[1].closestHitShader   = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 2: closest-hit (TRIANGLES_HIT_GROUP)
    groups[2].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader      = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader   = 2;
    groups[2].anyHitShader       = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // ── Create pipeline ──────────────────────────────────────────
    VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV clusterASCI{};
    clusterASCI.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CLUSTER_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
    clusterASCI.allowClusterAccelerationStructure = allowClusterAS ? VK_TRUE : VK_FALSE;

    VkRayTracingPipelineCreateInfoKHR pipeCI{};
    pipeCI.sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipeCI.pNext                        = allowClusterAS ? &clusterASCI : nullptr;
    pipeCI.stageCount                   = 3;
    pipeCI.pStages                      = stages;
    pipeCI.groupCount                   = 3;
    pipeCI.pGroups                      = groups;
    pipeCI.maxPipelineRayRecursionDepth = maxRecursionDepth;
    pipeCI.layout                       = layout;

    VkResult result = vkCreateRayTracingPipelinesKHR(
        device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &rt.pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create ray tracing pipeline (VkResult %d)\n", result);
        return rt;
    }

    // ── Build SBT ────────────────────────────────────────────────
    const uint32_t handleSize      = ctx.rtShaderGroupHandleSize();
    const uint32_t handleAlignment = ctx.rtShaderGroupHandleAlignment();
    const uint32_t baseAlignment   = ctx.rtShaderGroupBaseAlignment();
    const uint32_t handleSizeAligned = alignUp(handleSize, handleAlignment);

    // Each group has exactly 1 entry; stride must be aligned to baseAlignment
    const uint32_t groupStride = alignUp(handleSizeAligned, baseAlignment);
    const uint32_t groupCount  = 3;
    const VkDeviceSize sbtSize = (VkDeviceSize)groupStride * groupCount;

    // Retrieve all shader group handles
    std::vector<uint8_t> handles(handleSize * groupCount);
    vkGetRayTracingShaderGroupHandlesKHR(device, rt.pipeline, 0, groupCount,
        handles.size(), handles.data());

    // Allocate SBT buffer (host-visible for simplicity, tiny size ~192 bytes)
    rt.sbtBuffer = VkBuf::create(ctx.allocator(), sbtSize,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Map and copy handles at aligned offsets
    void* mapped = nullptr;
    vmaMapMemory(ctx.allocator(), rt.sbtBuffer.allocation, &mapped);
    memset(mapped, 0, sbtSize);
    for (uint32_t i = 0; i < groupCount; i++) {
        memcpy((uint8_t*)mapped + i * groupStride,
               handles.data() + i * handleSize,
               handleSize);
    }
    vmaUnmapMemory(ctx.allocator(), rt.sbtBuffer.allocation);

    // Compute device address regions
    VkDeviceAddress sbtAddr = ctx.getBufferDeviceAddress(rt.sbtBuffer.buffer);

    rt.rgenRegion.deviceAddress = sbtAddr + 0 * groupStride;
    rt.rgenRegion.stride        = groupStride;
    rt.rgenRegion.size          = groupStride;

    rt.missRegion.deviceAddress = sbtAddr + 1 * groupStride;
    rt.missRegion.stride        = groupStride;
    rt.missRegion.size          = groupStride;

    rt.hitRegion.deviceAddress  = sbtAddr + 2 * groupStride;
    rt.hitRegion.stride         = groupStride;
    rt.hitRegion.size           = groupStride;

    rt.callRegion = {};  // unused

    printf("[StratumV] RT pipeline created (SBT: %u bytes, handle=%u, stride=%u)\n",
        (uint32_t)sbtSize, handleSize, groupStride);

    return rt;
}

void VkRTPipeline::destroy(VkDevice device, VmaAllocator alloc)
{
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    sbtBuffer.destroy(alloc);
    pipeline = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    rgenRegion = {};
    missRegion = {};
    hitRegion  = {};
    callRegion = {};
}

} // namespace sv
