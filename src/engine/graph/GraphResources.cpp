// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "GraphResources.h"

namespace sv {

ResourceId ResourceRegistry::registerImage(const char* name, VkImage image,
                                            VkImageLayout initialLayout,
                                            VkImageAspectFlags aspect)
{
    uint32_t idx = static_cast<uint32_t>(m_resources.size());
    TrackedResource r{};
    r.name          = name;
    r.image         = image;
    r.initialLayout = initialLayout;
    r.currentLayout = initialLayout;
    r.aspectMask    = aspect;
    r.isImage       = true;
    m_resources.push_back(r);
    m_nameMap[name] = idx;
    return ResourceId{idx};
}

ResourceId ResourceRegistry::registerBuffer(const char* name, VkBuffer buffer)
{
    uint32_t idx = static_cast<uint32_t>(m_resources.size());
    TrackedResource r{};
    r.name    = name;
    r.buffer  = buffer;
    r.isImage = false;
    m_resources.push_back(r);
    m_nameMap[name] = idx;
    return ResourceId{idx};
}

void ResourceRegistry::updateImage(ResourceId id, VkImage newImage)
{
    if (id.valid() && id.index < m_resources.size())
        m_resources[id.index].image = newImage;
}

void ResourceRegistry::resetLayouts()
{
    for (auto& r : m_resources)
        r.currentLayout = r.initialLayout;
}

} // namespace sv
