// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vector>
#include <cstdint>

namespace sv {

// Builder for VkDescriptorSetLayout
class VkDescLayoutBuilder {
public:
    VkDescLayoutBuilder& addBinding(uint32_t binding, VkDescriptorType type,
        VkShaderStageFlags stages, uint32_t count = 1);
    VkDescriptorSetLayout build(VkDevice device);

private:
    std::vector<VkDescriptorSetLayoutBinding> m_bindings;
};

// Simple descriptor pool + allocation
class VkDescPool {
public:
    bool init(VkDevice device, uint32_t maxSets,
        const VkDescriptorPoolSize* sizes, uint32_t sizeCount);
    VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout);
    void destroy(VkDevice device);

private:
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
};

} // namespace sv
