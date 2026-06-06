// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "MorphTargetTypes.h"
#include <cstdio>

namespace sv {

bool MorphTargetData::createDescriptorSet(VkDevice device, VkDescriptorSetLayout layout)
{
    // One-set pool for this mesh's morph target SSBO
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;

    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &descPool) != VK_SUCCESS) {
        fprintf(stderr, "[MorphTargetData] Failed to create descriptor pool\n");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout;

    if (vkAllocateDescriptorSets(device, &allocInfo, &descSet) != VK_SUCCESS) {
        fprintf(stderr, "[MorphTargetData] Failed to allocate descriptor set\n");
        return false;
    }

    // Point descriptor at the delta SSBO
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = deltaSSBO.buffer;
    bufInfo.offset = 0;
    bufInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = descSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return true;
}

void MorphTargetData::destroy(VkDevice device, VmaAllocator allocator)
{
    if (descPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descPool, nullptr);
        descPool = VK_NULL_HANDLE;
        descSet  = VK_NULL_HANDLE;
    }
    deltaSSBO.destroy(allocator);
    targets.clear();
    vertexCount = 0;
}

VkDescriptorSetLayout createMorphTargetDescSetLayout(VkDevice device)
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings    = &binding;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout) != VK_SUCCESS) {
        fprintf(stderr, "[MorphTarget] Failed to create descriptor set layout\n");
    }
    return layout;
}

} // namespace sv
