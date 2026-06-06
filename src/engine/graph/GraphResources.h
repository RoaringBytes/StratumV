// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace sv {

// Opaque handle into the resource registry
struct ResourceId {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};

// Per-resource tracking state
struct TrackedResource {
    const char*         name           = nullptr;
    VkImage             image          = VK_NULL_HANDLE;
    VkBuffer            buffer         = VK_NULL_HANDLE;
    VkImageLayout       initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout       currentLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags  aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    bool                isImage        = true;
};

// Declares how a pass accesses a resource
struct ResourceUsage {
    ResourceId              resource;
    VkAccessFlags           accessMask      = 0;
    VkPipelineStageFlags    stageMask       = 0;
    VkImageLayout           requiredLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags      aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t                layerCount      = 1;
};

// Maps string names to physical Vulkan handles plus per-frame layout tracking
class ResourceRegistry {
public:
    ResourceId registerImage(const char* name, VkImage image,
                             VkImageLayout initialLayout,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    ResourceId registerBuffer(const char* name, VkBuffer buffer);

    void updateImage(ResourceId id, VkImage newImage);

    TrackedResource&       get(ResourceId id)       { return m_resources[id.index]; }
    const TrackedResource& get(ResourceId id) const { return m_resources[id.index]; }

    // Reset all tracked layouts to initial state (call at frame start)
    void resetLayouts();

    size_t count() const { return m_resources.size(); }

private:
    std::vector<TrackedResource>                m_resources;
    std::unordered_map<std::string, uint32_t>   m_nameMap;
};

} // namespace sv
