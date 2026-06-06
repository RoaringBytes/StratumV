// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkBuffer.h"
#include "VkContext.h"
#include <cstring>
#include <cstdio>

namespace sv {

VkBuf VkBuf::create(VmaAllocator alloc, VkDeviceSize size,
    VkBufferUsageFlags usage, VmaMemoryUsage memUsage)
{
    VkBuf buf;
    buf.size = size;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size  = size;
    bufInfo.usage = usage;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = memUsage;
    if (memUsage == VMA_MEMORY_USAGE_CPU_TO_GPU)
        allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(alloc, &bufInfo, &allocCI,
            &buf.buffer, &buf.allocation, &buf.info) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create buffer (size=%llu)\n",
            (unsigned long long)size);
    }

    return buf;
}

VkBuf VkBuf::createWithData(VkCtx& ctx, const void* data,
    VkDeviceSize size, VkBufferUsageFlags usage)
{
    // Staging buffer (CPU-visible)
    auto staging = create(ctx.allocator(), size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    void* mapped;
    vmaMapMemory(ctx.allocator(), staging.allocation, &mapped);
    memcpy(mapped, data, (size_t)size);
    vmaUnmapMemory(ctx.allocator(), staging.allocation);

    // Device-local buffer
    auto buf = create(ctx.allocator(), size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    // Copy via single-time command
    auto cmd = ctx.beginSingleTimeCommands();
    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, buf.buffer, 1, &region);
    ctx.endSingleTimeCommands(cmd);

    staging.destroy(ctx.allocator());
    return buf;
}

void VkBuf::destroy(VmaAllocator alloc)
{
    if (buffer) {
        vmaDestroyBuffer(alloc, buffer, allocation);
        buffer     = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
}

} // namespace sv
