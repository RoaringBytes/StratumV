// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>
#include "VkBuffer.h"
#include <vector>

namespace sv {

class VkCtx;

struct AccelStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkBuf  buffer{};          // backing storage
    VkDeviceAddress deviceAddress = 0;

    void destroy(VkDevice device, VmaAllocator alloc);
};

// Instance description for multi-instance TLAS builds.
struct TLASInstance {
    VkDeviceAddress blasAddress = 0;
    float           transform[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 }; // row-major 3x4 identity
    uint32_t        customIndex   = 0;
    uint8_t         mask          = 0xFF;
};

class VkAccelStructBuilder {
public:
    // Build a compacted BLAS from triangle mesh buffers.
    // Buffers must have SHADER_DEVICE_ADDRESS_BIT and
    // ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR.
    static AccelStructure buildBLAS(
        VkCtx& ctx,
        VkBuffer vertexBuffer, uint32_t vertexCount, VkDeviceSize vertexStride,
        VkBuffer indexBuffer,  uint32_t indexCount,
        VkFormat vertexFormat = VK_FORMAT_R32G32B32_SFLOAT);

    // Build a TLAS from a single BLAS instance (identity transform).
    static AccelStructure buildTLAS(VkCtx& ctx, const AccelStructure& blas);

    // Build a TLAS from a raw BLAS device address (for cluster BLAS).
    static AccelStructure buildTLAS(VkCtx& ctx, VkDeviceAddress blasAddress);

    // Build a TLAS from multiple BLAS instances.
    static AccelStructure buildTLAS(VkCtx& ctx, const std::vector<TLASInstance>& instances);

private:
    static VkBuf createScratchBuffer(VkCtx& ctx, VkDeviceSize size);
};

} // namespace sv
