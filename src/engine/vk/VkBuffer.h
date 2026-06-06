// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

namespace sv {

class VkCtx;

struct VkBuf {
    VkBuffer        buffer     = VK_NULL_HANDLE;
    VmaAllocation   allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    VkDeviceSize    size = 0;

    // Create a buffer with given usage (no data upload)
    static VkBuf create(VmaAllocator alloc, VkDeviceSize size,
        VkBufferUsageFlags usage, VmaMemoryUsage memUsage);

    // Create a device-local buffer with data uploaded via staging
    static VkBuf createWithData(VkCtx& ctx, const void* data,
        VkDeviceSize size, VkBufferUsageFlags usage);

    void destroy(VmaAllocator alloc);
};

} // namespace sv
