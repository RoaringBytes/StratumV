// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkDescriptors.h"
#include <cstdio>

namespace sv {

// ── VkDescLayoutBuilder ─────────────────────────────────────────
VkDescLayoutBuilder& VkDescLayoutBuilder::addBinding(uint32_t binding,
    VkDescriptorType type, VkShaderStageFlags stages, uint32_t count)
{
    VkDescriptorSetLayoutBinding b{};
    b.binding         = binding;
    b.descriptorType  = type;
    b.descriptorCount = count;
    b.stageFlags      = stages;
    m_bindings.push_back(b);
    return *this;
}

VkDescriptorSetLayout VkDescLayoutBuilder::build(VkDevice device)
{
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = (uint32_t)m_bindings.size();
    ci.pBindings    = m_bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create descriptor set layout\n");
    }
    return layout;
}

// ── VkDescPool ──────────────────────────────────────────────────
bool VkDescPool::init(VkDevice device, uint32_t maxSets,
    const VkDescriptorPoolSize* sizes, uint32_t sizeCount)
{
    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets       = maxSets;
    ci.poolSizeCount = sizeCount;
    ci.pPoolSizes    = sizes;

    if (vkCreateDescriptorPool(device, &ci, nullptr, &m_pool) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create descriptor pool\n");
        return false;
    }
    return true;
}

VkDescriptorSet VkDescPool::allocate(VkDevice device, VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to allocate descriptor set\n");
    }
    return set;
}

void VkDescPool::destroy(VkDevice device)
{
    if (m_pool) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

} // namespace sv
