// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>
#include "VkBuffer.h"

namespace sv {

class VkCtx;

struct ClusterAS {
    VkBuf clusterData{};      // packed CLAS triangle cluster data (dstImplicitData)
    VkBuf clusterAddresses{}; // array of VkDeviceAddress per cluster (dstAddressesArray output)
    VkBuf clusterBLAS{};      // cluster BLAS data (dstImplicitData for BLAS build)
    VkDeviceAddress blasAddress = 0;
    uint32_t clusterCount = 0;

    // Build cluster AS from terrain mesh vertex/index data.
    // Splits mesh into meshlets, builds triangle clusters, then builds cluster BLAS.
    // Requires VkCtx::supportsClusterAS() == true.
    static ClusterAS buildFromMesh(
        VkCtx& ctx,
        const float* positions, uint32_t vertexCount, uint32_t positionStride,
        const uint32_t* indices, uint32_t indexCount);

    void destroy(VmaAllocator alloc);
};

} // namespace sv
