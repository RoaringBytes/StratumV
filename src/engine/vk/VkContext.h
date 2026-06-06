// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <string>
#include <cstdint>

struct GLFWwindow;

namespace sv {

struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;
    uint32_t compute  = UINT32_MAX;
    uint32_t present  = UINT32_MAX;
    bool isComplete() const {
        return graphics != UINT32_MAX && compute != UINT32_MAX && present != UINT32_MAX;
    }
};

class VkCtx {
public:
    bool init(GLFWwindow* window);
    void shutdown();

    // Request additional optional device extensions before init() (for DLSS etc.)
    // Extensions are enabled if available, silently skipped if not.
    void requestOptionalDeviceExtension(const char* extName);

    VkInstance       instance()      const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice         device()        const { return m_device; }
    VmaAllocator     allocator()     const { return m_allocator; }
    VkQueue          graphicsQueue() const { return m_graphicsQueue; }
    VkQueue          computeQueue()  const { return m_computeQueue; }
    VkQueue          presentQueue()  const { return m_presentQueue; }
    VkSurfaceKHR     surface()       const { return m_surface; }
    const QueueFamilies& queueFamilies() const { return m_queueFamilies; }

    // Extension/layer info (needed by NVIDIA SDKs like WaveWorks)
    const std::vector<std::string>& instanceExtensionNames()  const { return m_instanceExtNames; }
    const std::vector<std::string>& instanceLayerNames()      const { return m_instanceLayerNames; }
    const std::vector<const char*>& deviceExtensionNames()    const { return m_deviceExtensions; }

    VkCommandPool    commandPool()  const { return m_commandPool; }

    // Ray tracing support
    bool supportsRayTracing() const { return m_rtSupported; }
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer) const;
    uint32_t accelStructScratchAlignment() const;
    uint32_t rtShaderGroupHandleSize() const;
    uint32_t rtShaderGroupHandleAlignment() const;
    uint32_t rtShaderGroupBaseAlignment() const;
    uint32_t rtMaxRecursionDepth() const;

    // Cluster acceleration structure support (VK_NV_cluster_acceleration_structure)
    bool supportsClusterAS() const { return m_clusterASSupported; }
    const VkPhysicalDeviceClusterAccelerationStructurePropertiesNV& clusterASProperties() const { return m_clusterASProperties; }

    // Ray query support (VK_KHR_ray_query — inline RT in compute shaders)
    bool supportsRayQuery() const { return m_rayQuerySupported; }

    // Memory budget (VK_EXT_memory_budget via VMA)
    bool supportsMemoryBudget() const { return m_memoryBudgetSupported; }
    struct HeapBudget { uint64_t usageBytes; uint64_t budgetBytes; };
    HeapBudget getDeviceLocalBudget() const;

    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer cmd);

private:
    bool createInstance();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createAllocator();
    QueueFamilies findQueueFamilies(VkPhysicalDevice device);
    bool checkDeviceExtensions(VkPhysicalDevice device);

    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
    VmaAllocator             m_allocator      = VK_NULL_HANDLE;
    VkCommandPool            m_commandPool    = VK_NULL_HANDLE;

    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_computeQueue  = VK_NULL_HANDLE;
    VkQueue m_presentQueue  = VK_NULL_HANDLE;

    QueueFamilies m_queueFamilies;

    // Stored for NVIDIA SDK init (WaveWorks etc.)
    std::vector<std::string> m_instanceExtNames;
    std::vector<std::string> m_instanceLayerNames;

    // Required device extensions
    std::vector<const char*> m_deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    // Storage for dynamically added extension strings
    std::vector<std::string> m_extraDeviceExtNames;
    // Optional extensions (enabled if supported, silently skipped if not)
    std::vector<std::string> m_optionalDeviceExtNames;

    // Ray tracing
    bool m_rtSupported = false;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_asProperties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtPipelineProperties{};

    // Cluster acceleration structure (RTXMG)
    bool m_clusterASSupported = false;
    VkPhysicalDeviceClusterAccelerationStructurePropertiesNV m_clusterASProperties{};

    // Ray query (inline RT in compute)
    bool m_rayQuerySupported = false;

    // Memory budget
    bool m_memoryBudgetSupported = false;
};

} // namespace sv
