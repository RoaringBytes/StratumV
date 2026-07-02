// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkContext.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <algorithm>

namespace sv {

// ── Debug messenger callback ─────────────────────────────────────
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[VK] %s\n", data->pMessage);
    return VK_FALSE;
}

void VkCtx::requestOptionalDeviceExtension(const char* extName) {
    m_optionalDeviceExtNames.push_back(extName);
}

// ── Public ───────────────────────────────────────────────────────
bool VkCtx::init(GLFWwindow* window)
{
    // Initialize volk (loads vkGetInstanceProcAddr from the Vulkan loader)
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to initialize volk\n");
        return false;
    }

    if (!createInstance()) return false;

    // Load instance-level functions
    volkLoadInstance(m_instance);

    // Create surface via GLFW
    if (glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create Vulkan surface\n");
        return false;
    }

    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;

    // Load device-level functions
    volkLoadDevice(m_device);

    if (!createAllocator()) return false;

    // Create command pool for single-time commands
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamilies.graphics;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create command pool\n");
        return false;
    }

    return true;
}

void VkCtx::shutdown()
{
    if (m_device) vkDeviceWaitIdle(m_device);

    if (m_commandPool) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    if (m_allocator)   vmaDestroyAllocator(m_allocator);
    if (m_device)      vkDestroyDevice(m_device, nullptr);
    if (m_surface)     vkDestroySurfaceKHR(m_instance, m_surface, nullptr);

    if (m_debugMessenger) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
    }

    if (m_instance) vkDestroyInstance(m_instance, nullptr);

    m_commandPool    = VK_NULL_HANDLE;
    m_allocator      = VK_NULL_HANDLE;
    m_device         = VK_NULL_HANDLE;
    m_surface        = VK_NULL_HANDLE;
    m_debugMessenger = VK_NULL_HANDLE;
    m_instance       = VK_NULL_HANDLE;
}

VkCommandBuffer VkCtx::beginSingleTimeCommands()
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    return cmd;
}

void VkCtx::endSingleTimeCommands(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

// ── Private ──────────────────────────────────────────────────────
bool VkCtx::createInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "StratumV";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.pEngineName = "StratumV Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // Get GLFW required extensions
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

    // Validation layers in debug
    std::vector<const char*> layers;
#ifdef _DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = (uint32_t)layers.size();
    createInfo.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create Vulkan instance\n");
        return false;
    }

    // Store extension/layer names for NVIDIA SDK init
    m_instanceExtNames.clear();
    for (uint32_t i = 0; i < (uint32_t)extensions.size(); i++)
        m_instanceExtNames.push_back(extensions[i]);
    m_instanceLayerNames.clear();
    for (uint32_t i = 0; i < (uint32_t)layers.size(); i++)
        m_instanceLayerNames.push_back(layers[i]);

#ifdef _DEBUG
    VkDebugUtilsMessengerCreateInfoEXT dbgInfo{};
    dbgInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbgInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbgInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbgInfo.pfnUserCallback = debugCallback;

    // This may fail if validation layers aren't installed — that's OK
    vkCreateDebugUtilsMessengerEXT(m_instance, &dbgInfo, nullptr, &m_debugMessenger);
#endif

    return true;
}

bool VkCtx::pickPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "[StratumV] No Vulkan-capable GPU found\n");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    // Prefer discrete GPU with required extensions
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;

    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        auto families = findQueueFamilies(dev);
        if (!families.isComplete()) continue;
        if (!checkDeviceExtensions(dev)) continue;

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        score += (int)(props.limits.maxImageDimension2D / 1000);

        // Prefer GPUs with ray tracing support (for future sessions)
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
        for (auto& e : exts) {
            if (strcmp(e.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0)
                score += 500;
        }

        if (score > bestScore) {
            bestScore = score;
            best = dev;
        }
    }

    if (best == VK_NULL_HANDLE) {
        fprintf(stderr, "[StratumV] No suitable GPU found\n");
        return false;
    }

    m_physicalDevice = best;
    m_queueFamilies = findQueueFamilies(best);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    printf("[StratumV] GPU: %s\n", props.deviceName);
    printf("[StratumV] Vulkan API: %u.%u.%u\n",
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    return true;
}

bool VkCtx::createLogicalDevice()
{
    std::set<uint32_t> uniqueFamilies = {
        m_queueFamilies.graphics,
        m_queueFamilies.compute,
        m_queueFamilies.present
    };

    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    // Enable features we need now + features we'll need for RT later
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = VK_TRUE;
    features.fillModeNonSolid = VK_TRUE;  // wireframe for debug view
    features.tessellationShader = VK_TRUE; // WaveWorks quadtree LOD tessellation
    features.shaderInt64 = VK_TRUE;        // SHaRC 64-bit hash grid types

    // Vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;          // needed for RT acceleration structures
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.timelineSemaphore = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;            // SHaRC buffer layout
    features12.shaderBufferInt64Atomics = VK_TRUE;     // SHaRC hash grid atomics

    // Vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = &features12;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // glslang 16 emits OpCapability DemoteToHelperInvocation for
    // `discard` in Vulkan-targeted GLSL; the feature must be enabled
    // (core in 1.3) or vkCreateShaderModule trips
    // VUID-VkShaderModuleCreateInfo-pCode-08740.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    // Always request memory budget as optional
    m_optionalDeviceExtNames.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    // Check optional extensions and enable the ones available
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, available.data());

        for (auto& optExt : m_optionalDeviceExtNames) {
            bool found = false;
            for (auto& avail : available) {
                if (optExt == avail.extensionName) { found = true; break; }
            }
            if (found) {
                m_extraDeviceExtNames.push_back(optExt);
                m_deviceExtensions.push_back(m_extraDeviceExtNames.back().c_str());
                printf("[StratumV] Enabled optional extension: %s\n", optExt.c_str());
            } else {
                printf("[StratumV] Optional extension not available: %s\n", optExt.c_str());
            }
        }
    }

    // Check if memory budget extension was enabled
    {
        for (auto& ext : m_deviceExtensions) {
            if (strcmp(ext, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                m_memoryBudgetSupported = true;
                break;
            }
        }
    }

    // Check if all RT extensions were enabled
    {
        bool hasAS = false, hasRTPipeline = false, hasDeferred = false, hasClusterAS = false, hasRayQuery = false;
        for (auto& ext : m_deviceExtensions) {
            if (strcmp(ext, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) hasAS = true;
            if (strcmp(ext, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) hasRTPipeline = true;
            if (strcmp(ext, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) hasDeferred = true;
            if (strcmp(ext, VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) hasClusterAS = true;
            if (strcmp(ext, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) hasRayQuery = true;
        }
        m_rtSupported = hasAS && hasRTPipeline && hasDeferred;
        m_clusterASSupported = m_rtSupported && hasClusterAS;
        m_rayQuerySupported = m_rtSupported && hasRayQuery;
    }

    // RT feature structs (only chained if RT is available)
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};

    // Ray query feature struct (only chained if ray query is available)
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};

    // Cluster AS feature struct (only chained if cluster AS is available)
    VkPhysicalDeviceClusterAccelerationStructureFeaturesNV clusterASFeatures{};

    if (m_rtSupported) {
        // Query AS + RT pipeline properties
        m_asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        m_rtPipelineProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        m_rtPipelineProperties.pNext = &m_asProperties;

        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &m_rtPipelineProperties;

        // Chain cluster AS properties query if supported
        if (m_clusterASSupported) {
            m_clusterASProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV;
            m_asProperties.pNext = &m_clusterASProperties;
        }

        vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

        // Chain RT features into pNext
        asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asFeatures.accelerationStructure = VK_TRUE;

        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.pNext = &asFeatures;
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;

        features12.pNext = &rtPipelineFeatures;

        // Chain cluster AS feature if supported
        if (m_clusterASSupported) {
            clusterASFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV;
            clusterASFeatures.clusterAccelerationStructure = VK_TRUE;
            asFeatures.pNext = &clusterASFeatures;
        }

        // Chain ray query feature if supported (for inline RT in compute shaders)
        if (m_rayQuerySupported) {
            rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            rayQueryFeatures.rayQuery = VK_TRUE;
            // Append to end of current pNext chain
            if (m_clusterASSupported)
                clusterASFeatures.pNext = &rayQueryFeatures;
            else
                asFeatures.pNext = &rayQueryFeatures;
        }

        printf("[StratumV] Ray tracing supported (scratch align=%u, SBT handle=%u, align=%u, base=%u)\n",
            m_asProperties.minAccelerationStructureScratchOffsetAlignment,
            m_rtPipelineProperties.shaderGroupHandleSize,
            m_rtPipelineProperties.shaderGroupHandleAlignment,
            m_rtPipelineProperties.shaderGroupBaseAlignment);

        if (m_clusterASSupported) {
            printf("[StratumV] Cluster AS supported (maxVerts=%u, maxTris=%u, clusterAlign=%u, blasAlign=%u)\n",
                m_clusterASProperties.maxVerticesPerCluster,
                m_clusterASProperties.maxTrianglesPerCluster,
                m_clusterASProperties.clusterByteAlignment,
                m_clusterASProperties.clusterBottomLevelByteAlignment);
        }

        if (m_rayQuerySupported) {
            printf("[StratumV] Ray query supported (inline RT in compute shaders)\n");
        }
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features13;
    createInfo.queueCreateInfoCount = (uint32_t)queueInfos.size();
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.enabledExtensionCount = (uint32_t)m_deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = m_deviceExtensions.data();
    createInfo.pEnabledFeatures = &features;

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create logical device\n");
        return false;
    }

    vkGetDeviceQueue(m_device, m_queueFamilies.graphics, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.compute, 0, &m_computeQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.present, 0, &m_presentQueue);

    return true;
}

bool VkCtx::createAllocator()
{
    VmaVulkanFunctions vkFuncs{};
    vkFuncs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vkFuncs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (m_memoryBudgetSupported)
        info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    info.physicalDevice = m_physicalDevice;
    info.device = m_device;
    info.instance = m_instance;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.pVulkanFunctions = &vkFuncs;

    if (vmaCreateAllocator(&info, &m_allocator) != VK_SUCCESS) {
        fprintf(stderr, "[StratumV] Failed to create VMA allocator\n");
        return false;
    }

    return true;
}

QueueFamilies VkCtx::findQueueFamilies(VkPhysicalDevice device)
{
    QueueFamilies families;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());

    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            families.graphics = i;
        if (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            families.compute = i;

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
        if (presentSupport) families.present = i;

        if (families.isComplete()) break;
    }

    return families;
}

bool VkCtx::checkDeviceExtensions(VkPhysicalDevice device)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    for (const char* required : m_deviceExtensions) {
        bool found = false;
        for (auto& ext : available) {
            if (strcmp(ext.extensionName, required) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

VkCtx::HeapBudget VkCtx::getDeviceLocalBudget() const
{
    HeapBudget result{0, 0};
    if (!m_allocator) return result;

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(m_allocator, budgets);

    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            result.usageBytes += budgets[i].usage;
            result.budgetBytes += budgets[i].budget;
        }
    }
    return result;
}

VkDeviceAddress VkCtx::getBufferDeviceAddress(VkBuffer buffer) const
{
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(m_device, &info);
}

uint32_t VkCtx::accelStructScratchAlignment() const
{
    return m_asProperties.minAccelerationStructureScratchOffsetAlignment;
}

uint32_t VkCtx::rtShaderGroupHandleSize() const
{
    return m_rtPipelineProperties.shaderGroupHandleSize;
}

uint32_t VkCtx::rtShaderGroupHandleAlignment() const
{
    return m_rtPipelineProperties.shaderGroupHandleAlignment;
}

uint32_t VkCtx::rtShaderGroupBaseAlignment() const
{
    return m_rtPipelineProperties.shaderGroupBaseAlignment;
}

uint32_t VkCtx::rtMaxRecursionDepth() const
{
    return m_rtPipelineProperties.maxRayRecursionDepth;
}

} // namespace sv
