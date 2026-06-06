// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkClusterAS.h"
#include "VkContext.h"
#include <meshoptimizer.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include <glm/glm.hpp>

namespace sv {

// ── Helpers ──────────────────────────────────────────────────

static VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static VkBuf createDeviceBuffer(VkCtx& ctx, VkDeviceSize size, VkBufferUsageFlags usage) {
    return VkBuf::create(ctx.allocator(), size,
        usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
}

// ── ClusterAS cleanup ─────────��──────────────────────────────

void ClusterAS::destroy(VmaAllocator alloc) {
    clusterData.destroy(alloc);
    clusterAddresses.destroy(alloc);
    clusterBLAS.destroy(alloc);
    blasAddress = 0;
    clusterCount = 0;
}

// ── Build cluster AS from mesh ──────���────────────────────────

ClusterAS ClusterAS::buildFromMesh(
    VkCtx& ctx,
    const float* positions, uint32_t vertexCount, uint32_t positionStride,
    const uint32_t* indices, uint32_t indexCount)
{
    assert(ctx.supportsClusterAS());
    VkDevice device = ctx.device();
    const auto& props = ctx.clusterASProperties();

    // ── Step 1: Generate meshlets using meshoptimizer ────────

    const size_t maxVerts = std::min((uint32_t)255, props.maxVerticesPerCluster);
    const size_t maxTris  = std::min((uint32_t)124, props.maxTrianglesPerCluster);
    // maxTris capped at 124 to stay safely within 9-bit field (max 511)

    size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, maxVerts, maxTris);
    std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
    std::vector<unsigned int> meshletVertices(maxMeshlets * maxVerts);
    std::vector<unsigned char> meshletTriangles(maxMeshlets * maxTris * 3);

    size_t meshletCount = meshopt_buildMeshlets(
        meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
        indices, indexCount,
        positions, vertexCount, positionStride,
        maxVerts, maxTris,
        0.0f); // cone_weight=0 for RT (no backface culling optimization)

    meshlets.resize(meshletCount);
    printf("[StratumV] ClusterAS: %zu meshlets from %u tris (maxVerts=%zu, maxTris=%zu)\n",
        meshletCount, indexCount / 3, maxVerts, maxTris);

    // ── Step 2: Pack per-cluster vertex/index data ───────────
    // For each meshlet, create packed float3 positions and uint8 local indices

    // Calculate total packed sizes
    size_t totalPackedVerts = 0;
    size_t totalPackedIndices = 0;
    for (size_t i = 0; i < meshletCount; i++) {
        totalPackedVerts += meshlets[i].vertex_count;
        totalPackedIndices += meshlets[i].triangle_count * 3;
    }

    // Pack vertex positions (float3, 12 bytes each)
    std::vector<float> packedPositions(totalPackedVerts * 3);
    std::vector<uint8_t> packedIndices(totalPackedIndices);

    size_t vertOffset = 0;
    size_t idxOffset = 0;
    struct MeshletLayout {
        uint32_t vertexByteOffset;
        uint32_t indexByteOffset;
        uint32_t vertexCount;
        uint32_t triangleCount;
    };
    std::vector<MeshletLayout> layouts(meshletCount);

    for (size_t m = 0; m < meshletCount; m++) {
        const auto& ml = meshlets[m];
        layouts[m].vertexByteOffset = (uint32_t)(vertOffset * 3 * sizeof(float));
        layouts[m].indexByteOffset = (uint32_t)(idxOffset * sizeof(uint8_t));
        layouts[m].vertexCount = ml.vertex_count;
        layouts[m].triangleCount = ml.triangle_count;

        // Copy vertex positions
        for (uint32_t v = 0; v < ml.vertex_count; v++) {
            uint32_t origIdx = meshletVertices[ml.vertex_offset + v];
            const float* src = (const float*)((const char*)positions + origIdx * positionStride);
            packedPositions[(vertOffset + v) * 3 + 0] = src[0];
            packedPositions[(vertOffset + v) * 3 + 1] = src[1];
            packedPositions[(vertOffset + v) * 3 + 2] = src[2];
        }

        // Copy triangle indices (already local 0-based uint8)
        for (uint32_t t = 0; t < ml.triangle_count * 3; t++) {
            packedIndices[idxOffset + t] = meshletTriangles[ml.triangle_offset + t];
        }

        vertOffset += ml.vertex_count;
        idxOffset += ml.triangle_count * 3;
    }

    // Upload packed data to GPU
    auto packedVertBuf = VkBuf::createWithData(ctx,
        packedPositions.data(), packedPositions.size() * sizeof(float),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    auto packedIdxBuf = VkBuf::createWithData(ctx,
        packedIndices.data(), packedIndices.size() * sizeof(uint8_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    VkDeviceAddress vertBaseAddr = ctx.getBufferDeviceAddress(packedVertBuf.buffer);
    VkDeviceAddress idxBaseAddr  = ctx.getBufferDeviceAddress(packedIdxBuf.buffer);

    // ── Step 3: Fill BuildTriangleClusterInfo per meshlet ─────

    std::vector<VkClusterAccelerationStructureBuildTriangleClusterInfoNV> clusterInfos(meshletCount);
    memset(clusterInfos.data(), 0, clusterInfos.size() * sizeof(clusterInfos[0]));

    for (size_t m = 0; m < meshletCount; m++) {
        auto& ci = clusterInfos[m];
        ci.clusterID = (uint32_t)m;
        ci.clusterFlags = 0;
        ci.triangleCount = layouts[m].triangleCount;
        ci.vertexCount = layouts[m].vertexCount;
        ci.positionTruncateBitCount = 0; // full precision
        ci.indexType = 0; // VK_CLUSTER_ACCELERATION_STRUCTURE_INDEX_FORMAT_8BIT_NV = 0x1
        // indexType field uses the flag bits directly (8BIT=1, 16BIT=2, 32BIT=4)
        ci.indexType = 1; // 8-bit indices
        ci.opacityMicromapIndexType = 0;

        VkClusterAccelerationStructureGeometryIndexAndGeometryFlagsNV geomFlags{};
        geomFlags.geometryIndex = 0;
        geomFlags.reserved = 0;
        geomFlags.geometryFlags = 4; // OPAQUE (VK_CLUSTER_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT_NV)
        ci.baseGeometryIndexAndGeometryFlags = geomFlags;

        ci.indexBufferStride = sizeof(uint8_t);
        ci.vertexBufferStride = sizeof(float) * 3;
        ci.geometryIndexAndFlagsBufferStride = 0;
        ci.opacityMicromapIndexBufferStride = 0;

        ci.indexBuffer = idxBaseAddr + layouts[m].indexByteOffset;
        ci.vertexBuffer = vertBaseAddr + layouts[m].vertexByteOffset;
        ci.geometryIndexAndFlagsBuffer = 0;
        ci.opacityMicromapArray = 0;
        ci.opacityMicromapIndexBuffer = 0;
    }

    // Upload cluster infos to GPU
    auto clusterInfoBuf = VkBuf::createWithData(ctx,
        clusterInfos.data(),
        clusterInfos.size() * sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    // Upload cluster count to GPU (uint32_t)
    uint32_t clusterCountVal = (uint32_t)meshletCount;
    auto clusterCountBuf = VkBuf::createWithData(ctx,
        &clusterCountVal, sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    // ── Step 4: Query triangle cluster build sizes ───────────

    VkClusterAccelerationStructureTriangleClusterInputNV triInput{};
    triInput.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV;
    triInput.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triInput.maxGeometryIndexValue = 0;
    triInput.maxClusterUniqueGeometryCount = 1;
    triInput.maxClusterTriangleCount = (uint32_t)maxTris;
    triInput.maxClusterVertexCount = (uint32_t)maxVerts;
    triInput.maxTotalTriangleCount = indexCount / 3;
    triInput.maxTotalVertexCount = (uint32_t)totalPackedVerts;
    triInput.minPositionTruncateBitCount = 0;

    VkClusterAccelerationStructureInputInfoNV inputInfo{};
    inputInfo.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV;
    inputInfo.maxAccelerationStructureCount = (uint32_t)meshletCount;
    inputInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    inputInfo.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
    inputInfo.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
    inputInfo.opInput.pTriangleClusters = &triInput;

    VkAccelerationStructureBuildSizesInfoKHR clusterSizes{};
    clusterSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetClusterAccelerationStructureBuildSizesNV(device, &inputInfo, &clusterSizes);

    printf("[StratumV] ClusterAS: triangle cluster build: dst=%llu bytes, scratch=%llu bytes\n",
        (unsigned long long)clusterSizes.accelerationStructureSize,
        (unsigned long long)clusterSizes.buildScratchSize);

    // Allocate cluster destination + scratch + output addresses
    uint32_t clusterAlign = props.clusterByteAlignment;
    uint32_t scratchAlign = props.clusterScratchByteAlignment;

    VkDeviceSize dstSize = alignUp(clusterSizes.accelerationStructureSize, clusterAlign);
    VkDeviceSize scratchSize = alignUp(clusterSizes.buildScratchSize, scratchAlign);

    auto clusterDstBuf = createDeviceBuffer(ctx, dstSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto clusterScratchBuf = createDeviceBuffer(ctx, scratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto clusterAddrBuf = createDeviceBuffer(ctx,
        meshletCount * sizeof(VkDeviceAddress),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // ── Step 5: Build triangle clusters ──────────────────────

    {
        auto cmd = ctx.beginSingleTimeCommands();

        VkClusterAccelerationStructureCommandsInfoNV cmdsInfo{};
        cmdsInfo.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV;
        cmdsInfo.input = inputInfo;
        cmdsInfo.dstImplicitData = ctx.getBufferDeviceAddress(clusterDstBuf.buffer);
        cmdsInfo.scratchData = ctx.getBufferDeviceAddress(clusterScratchBuf.buffer);

        cmdsInfo.dstAddressesArray.deviceAddress = ctx.getBufferDeviceAddress(clusterAddrBuf.buffer);
        cmdsInfo.dstAddressesArray.stride = sizeof(VkDeviceAddress);
        cmdsInfo.dstAddressesArray.size = meshletCount * sizeof(VkDeviceAddress);

        cmdsInfo.dstSizesArray = {}; // not needed for implicit mode

        cmdsInfo.srcInfosArray.deviceAddress = ctx.getBufferDeviceAddress(clusterInfoBuf.buffer);
        cmdsInfo.srcInfosArray.stride = sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV);
        cmdsInfo.srcInfosArray.size = meshletCount * sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV);

        cmdsInfo.srcInfosCount = ctx.getBufferDeviceAddress(clusterCountBuf.buffer);
        cmdsInfo.addressResolutionFlags = 0;

        vkCmdBuildClusterAccelerationStructureIndirectNV(cmd, &cmdsInfo);

        // Barrier: cluster build → BLAS build
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                              | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0, 1, &barrier, 0, nullptr, 0, nullptr);

        ctx.endSingleTimeCommands(cmd);
    }

    printf("[StratumV] ClusterAS: %zu triangle clusters built\n", meshletCount);

    // ── Step 6: Query cluster BLAS build sizes ───────────────

    VkClusterAccelerationStructureClustersBottomLevelInputNV blasInput{};
    blasInput.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV;
    blasInput.maxTotalClusterCount = (uint32_t)meshletCount;
    blasInput.maxClusterCountPerAccelerationStructure = (uint32_t)meshletCount;

    VkClusterAccelerationStructureInputInfoNV blasInputInfo{};
    blasInputInfo.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV;
    blasInputInfo.maxAccelerationStructureCount = 1;
    blasInputInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasInputInfo.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
    blasInputInfo.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
    blasInputInfo.opInput.pClustersBottomLevel = &blasInput;

    VkAccelerationStructureBuildSizesInfoKHR blasSizes{};
    blasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetClusterAccelerationStructureBuildSizesNV(device, &blasInputInfo, &blasSizes);

    printf("[StratumV] ClusterAS: cluster BLAS build: dst=%llu bytes, scratch=%llu bytes\n",
        (unsigned long long)blasSizes.accelerationStructureSize,
        (unsigned long long)blasSizes.buildScratchSize);

    // Allocate BLAS destination + scratch
    uint32_t blasAlign = props.clusterBottomLevelByteAlignment;
    VkDeviceSize blasDstSize = alignUp(blasSizes.accelerationStructureSize, blasAlign);
    VkDeviceSize blasScratchSize = alignUp(blasSizes.buildScratchSize, scratchAlign);

    auto blasDstBuf = createDeviceBuffer(ctx, blasDstSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto blasScratchBuf = createDeviceBuffer(ctx, blasScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // Build the BLAS build info struct on GPU
    VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV blasBuildInfo{};
    blasBuildInfo.clusterReferencesCount = (uint32_t)meshletCount;
    blasBuildInfo.clusterReferencesStride = sizeof(VkDeviceAddress);
    blasBuildInfo.clusterReferences = ctx.getBufferDeviceAddress(clusterAddrBuf.buffer);

    auto blasBuildInfoBuf = VkBuf::createWithData(ctx,
        &blasBuildInfo, sizeof(blasBuildInfo),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    // Upload BLAS count (1 BLAS)
    uint32_t blasCountVal = 1;
    auto blasCountBuf = VkBuf::createWithData(ctx,
        &blasCountVal, sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    // Output BLAS address buffer
    auto blasAddrBuf = createDeviceBuffer(ctx,
        sizeof(VkDeviceAddress),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    // ── Step 7: Build cluster BLAS ───────────────────────────

    {
        auto cmd = ctx.beginSingleTimeCommands();

        VkClusterAccelerationStructureCommandsInfoNV cmdsInfo{};
        cmdsInfo.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV;
        cmdsInfo.input = blasInputInfo;
        cmdsInfo.dstImplicitData = ctx.getBufferDeviceAddress(blasDstBuf.buffer);
        cmdsInfo.scratchData = ctx.getBufferDeviceAddress(blasScratchBuf.buffer);

        cmdsInfo.dstAddressesArray.deviceAddress = ctx.getBufferDeviceAddress(blasAddrBuf.buffer);
        cmdsInfo.dstAddressesArray.stride = sizeof(VkDeviceAddress);
        cmdsInfo.dstAddressesArray.size = sizeof(VkDeviceAddress);

        cmdsInfo.dstSizesArray = {};

        cmdsInfo.srcInfosArray.deviceAddress = ctx.getBufferDeviceAddress(blasBuildInfoBuf.buffer);
        cmdsInfo.srcInfosArray.stride = sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV);
        cmdsInfo.srcInfosArray.size = sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV);

        cmdsInfo.srcInfosCount = ctx.getBufferDeviceAddress(blasCountBuf.buffer);
        cmdsInfo.addressResolutionFlags = 0;

        vkCmdBuildClusterAccelerationStructureIndirectNV(cmd, &cmdsInfo);

        ctx.endSingleTimeCommands(cmd);
    }

    // Read back BLAS address from GPU
    // Create a host-visible staging buffer to read the address
    auto blasAddrReadback = VkBuf::create(ctx.allocator(),
        sizeof(VkDeviceAddress),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_CPU_ONLY);

    {
        auto cmd = ctx.beginSingleTimeCommands();

        VkBufferCopy copyRegion{};
        copyRegion.size = sizeof(VkDeviceAddress);
        vkCmdCopyBuffer(cmd, blasAddrBuf.buffer, blasAddrReadback.buffer, 1, &copyRegion);

        ctx.endSingleTimeCommands(cmd);
    }

    VkDeviceAddress blasDevAddr = 0;
    void* mapped = nullptr;
    vmaMapMemory(ctx.allocator(), blasAddrReadback.allocation, &mapped);
    memcpy(&blasDevAddr, mapped, sizeof(VkDeviceAddress));
    vmaUnmapMemory(ctx.allocator(), blasAddrReadback.allocation);
    blasAddrReadback.destroy(ctx.allocator());

    printf("[StratumV] ClusterAS: cluster BLAS built at address 0x%llx\n",
        (unsigned long long)blasDevAddr);

    // ── Cleanup temporaries, assemble result ─────────────────

    clusterScratchBuf.destroy(ctx.allocator());
    blasScratchBuf.destroy(ctx.allocator());
    clusterInfoBuf.destroy(ctx.allocator());
    clusterCountBuf.destroy(ctx.allocator());
    blasBuildInfoBuf.destroy(ctx.allocator());
    blasCountBuf.destroy(ctx.allocator());
    packedVertBuf.destroy(ctx.allocator());
    packedIdxBuf.destroy(ctx.allocator());
    blasAddrBuf.destroy(ctx.allocator());

    ClusterAS result{};
    result.clusterData = clusterDstBuf;
    result.clusterAddresses = clusterAddrBuf;
    result.clusterBLAS = blasDstBuf;
    result.blasAddress = blasDevAddr;
    result.clusterCount = (uint32_t)meshletCount;

    return result;
}

} // namespace sv
