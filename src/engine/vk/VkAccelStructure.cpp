// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkAccelStructure.h"
#include "VkContext.h"
#include <cstdio>
#include <cstring>
#include <cassert>

namespace sv {

// ── AccelStructure cleanup ──────────────────────────────────

void AccelStructure::destroy(VkDevice device, VmaAllocator alloc)
{
    if (handle) {
        vkDestroyAccelerationStructureKHR(device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    buffer.destroy(alloc);
    deviceAddress = 0;
}

// ── Scratch buffer helper ───────────────────────────────────

VkBuf VkAccelStructBuilder::createScratchBuffer(VkCtx& ctx, VkDeviceSize size)
{
    uint32_t alignment = ctx.accelStructScratchAlignment();
    VkDeviceSize alignedSize = (size + alignment - 1) & ~(VkDeviceSize)(alignment - 1);

    auto buf = VkBuf::create(ctx.allocator(), alignedSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // Verify device address alignment
    VkDeviceAddress addr = ctx.getBufferDeviceAddress(buf.buffer);
    assert((addr % alignment) == 0 && "Scratch buffer address not aligned");
    (void)addr;

    return buf;
}

// ── BLAS build with compaction ──────────────────────────────

AccelStructure VkAccelStructBuilder::buildBLAS(
    VkCtx& ctx,
    VkBuffer vertexBuffer, uint32_t vertexCount, VkDeviceSize vertexStride,
    VkBuffer indexBuffer,  uint32_t indexCount,
    VkFormat vertexFormat)
{
    VkDevice device = ctx.device();
    uint32_t primitiveCount = indexCount / 3;

    // Triangle geometry
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat  = vertexFormat;
    triangles.vertexData.deviceAddress = ctx.getBufferDeviceAddress(vertexBuffer);
    triangles.vertexStride  = vertexStride;
    triangles.maxVertex     = vertexCount - 1;
    triangles.indexType     = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = ctx.getBufferDeviceAddress(indexBuffer);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.geometry.triangles = triangles;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    // Build info
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type  = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    buildInfo.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geometry;

    // Query build sizes
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &primitiveCount, &sizeInfo);

    // Create uncompacted AS
    auto uncompactedBuf = VkBuf::create(ctx.allocator(), sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    VkAccelerationStructureCreateInfoKHR asCI{};
    asCI.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asCI.buffer = uncompactedBuf.buffer;
    asCI.size   = sizeInfo.accelerationStructureSize;
    asCI.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    VkAccelerationStructureKHR uncompactedAS = VK_NULL_HANDLE;
    vkCreateAccelerationStructureKHR(device, &asCI, nullptr, &uncompactedAS);

    // Scratch buffer
    auto scratch = createScratchBuffer(ctx, sizeInfo.buildScratchSize);

    // Query pool for compacted size
    VkQueryPoolCreateInfo qpCI{};
    qpCI.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpCI.queryType  = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    qpCI.queryCount = 1;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    vkCreateQueryPool(device, &qpCI, nullptr, &queryPool);

    // Build + query compacted size
    {
        auto cmd = ctx.beginSingleTimeCommands();

        vkCmdResetQueryPool(cmd, queryPool, 0, 1);

        buildInfo.dstAccelerationStructure  = uncompactedAS;
        buildInfo.scratchData.deviceAddress = ctx.getBufferDeviceAddress(scratch.buffer);

        VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = primitiveCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

        // Memory barrier before query
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0, 1, &barrier, 0, nullptr, 0, nullptr);

        vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &uncompactedAS,
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool, 0);

        ctx.endSingleTimeCommands(cmd);
    }

    // Read compacted size
    VkDeviceSize compactedSize = 0;
    vkGetQueryPoolResults(device, queryPool, 0, 1,
        sizeof(VkDeviceSize), &compactedSize, sizeof(VkDeviceSize),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    // Create compacted AS
    AccelStructure result{};
    result.buffer = VkBuf::create(ctx.allocator(), compactedSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    VkAccelerationStructureCreateInfoKHR compactCI{};
    compactCI.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    compactCI.buffer = result.buffer.buffer;
    compactCI.size   = compactedSize;
    compactCI.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    vkCreateAccelerationStructureKHR(device, &compactCI, nullptr, &result.handle);

    // Copy compacted
    {
        auto cmd = ctx.beginSingleTimeCommands();

        VkCopyAccelerationStructureInfoKHR copyInfo{};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
        copyInfo.src   = uncompactedAS;
        copyInfo.dst   = result.handle;
        copyInfo.mode  = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        vkCmdCopyAccelerationStructureKHR(cmd, &copyInfo);

        ctx.endSingleTimeCommands(cmd);
    }

    // Get device address
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = result.handle;
    result.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

    // Cleanup temporaries
    vkDestroyAccelerationStructureKHR(device, uncompactedAS, nullptr);
    uncompactedBuf.destroy(ctx.allocator());
    scratch.destroy(ctx.allocator());
    vkDestroyQueryPool(device, queryPool, nullptr);

    printf("[StratumV] BLAS built: %llu -> %llu bytes (compacted), %u tris\n",
        (unsigned long long)sizeInfo.accelerationStructureSize,
        (unsigned long long)compactedSize, primitiveCount);

    return result;
}

// ── TLAS build ──────────────────────────────────────────────

AccelStructure VkAccelStructBuilder::buildTLAS(VkCtx& ctx, const AccelStructure& blas)
{
    return buildTLAS(ctx, blas.deviceAddress);
}

AccelStructure VkAccelStructBuilder::buildTLAS(VkCtx& ctx, VkDeviceAddress blasAddress)
{
    VkDevice device = ctx.device();

    // Single instance — identity transform
    VkAccelerationStructureInstanceKHR instance{};
    // Row-major 3x4 identity
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[2][2] = 1.0f;
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = blasAddress;

    // Upload instance data
    auto instanceBuf = VkBuf::createWithData(ctx,
        &instance, sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);

    // Geometry info
    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = ctx.getBufferDeviceAddress(instanceBuf.buffer);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instancesData;

    // Build info
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type  = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geometry;

    // Query sizes
    uint32_t instanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &instanceCount, &sizeInfo);

    // Create TLAS
    AccelStructure result{};
    result.buffer = VkBuf::create(ctx.allocator(), sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    VkAccelerationStructureCreateInfoKHR asCI{};
    asCI.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asCI.buffer = result.buffer.buffer;
    asCI.size   = sizeInfo.accelerationStructureSize;
    asCI.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vkCreateAccelerationStructureKHR(device, &asCI, nullptr, &result.handle);

    // Scratch buffer
    auto scratch = createScratchBuffer(ctx, sizeInfo.buildScratchSize);

    // Build
    {
        auto cmd = ctx.beginSingleTimeCommands();

        buildInfo.dstAccelerationStructure  = result.handle;
        buildInfo.scratchData.deviceAddress = ctx.getBufferDeviceAddress(scratch.buffer);

        VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = instanceCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

        ctx.endSingleTimeCommands(cmd);
    }

    // Get device address
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = result.handle;
    result.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

    // Cleanup temporaries
    scratch.destroy(ctx.allocator());
    instanceBuf.destroy(ctx.allocator());

    printf("[StratumV] TLAS built: %llu bytes, %u instance(s)\n",
        (unsigned long long)sizeInfo.accelerationStructureSize, instanceCount);

    return result;
}

// ── Multi-instance TLAS build ────────────────────────────────

AccelStructure VkAccelStructBuilder::buildTLAS(VkCtx& ctx, const std::vector<TLASInstance>& instances)
{
    if (instances.empty()) return {};
    if (instances.size() == 1) return buildTLAS(ctx, instances[0].blasAddress);

    VkDevice device = ctx.device();
    uint32_t instanceCount = (uint32_t)instances.size();

    // Build VkAccelerationStructureInstanceKHR array
    std::vector<VkAccelerationStructureInstanceKHR> vkInstances(instanceCount);
    for (uint32_t i = 0; i < instanceCount; i++) {
        auto& vi = vkInstances[i];
        memset(&vi, 0, sizeof(vi));
        // Copy 3x4 row-major transform
        memcpy(&vi.transform, instances[i].transform, sizeof(float) * 12);
        vi.instanceCustomIndex = instances[i].customIndex & 0x00FFFFFF;
        vi.mask = instances[i].mask;
        vi.instanceShaderBindingTableRecordOffset = 0;
        vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        vi.accelerationStructureReference = instances[i].blasAddress;
    }

    // Upload instance data
    auto instanceBuf = VkBuf::createWithData(ctx,
        vkInstances.data(), sizeof(VkAccelerationStructureInstanceKHR) * instanceCount,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);

    // Geometry info
    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = ctx.getBufferDeviceAddress(instanceBuf.buffer);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instancesData;

    // Build info
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type  = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geometry;

    // Query sizes
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &instanceCount, &sizeInfo);

    // Create TLAS
    AccelStructure result{};
    result.buffer = VkBuf::create(ctx.allocator(), sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    VkAccelerationStructureCreateInfoKHR asCI{};
    asCI.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asCI.buffer = result.buffer.buffer;
    asCI.size   = sizeInfo.accelerationStructureSize;
    asCI.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vkCreateAccelerationStructureKHR(device, &asCI, nullptr, &result.handle);

    // Scratch buffer
    auto scratch = createScratchBuffer(ctx, sizeInfo.buildScratchSize);

    // Build
    {
        auto cmd = ctx.beginSingleTimeCommands();

        buildInfo.dstAccelerationStructure  = result.handle;
        buildInfo.scratchData.deviceAddress = ctx.getBufferDeviceAddress(scratch.buffer);

        VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = instanceCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

        ctx.endSingleTimeCommands(cmd);
    }

    // Get device address
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = result.handle;
    result.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

    // Cleanup temporaries
    scratch.destroy(ctx.allocator());
    instanceBuf.destroy(ctx.allocator());

    printf("[StratumV] TLAS built: %llu bytes, %u instance(s)\n",
        (unsigned long long)sizeInfo.accelerationStructureSize, instanceCount);

    return result;
}

} // namespace sv
