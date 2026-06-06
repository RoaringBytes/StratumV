// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "Types.h"
#include "vk/VkPipeline.h"
#include "vk/VkMesh.h"
// NOTE: Terrain-specific pipeline builders (buildTerrainPipeline,
// buildTerrainTessPipeline, buildShadowPipeline) live in the consuming game's
// src/game/ — they depend on TerrainVertex from Terrain.h which is
// a game-specific domain type. Not part of the game-agnostic engine layer.

namespace sv
{

    VkPipeline buildSkyPipeline(VkDevice device,
                                VkShaderModule vert, VkShaderModule frag,
                                VkPipelineLayout layout, VkFormat colorFormat,
                                VkPipelineCache cache)
    {
        return VkPipeBuilder()
            .setShaders(vert, frag)
            // No vertex input — fullscreen triangle via gl_VertexIndex
            .setCullMode(VK_CULL_MODE_NONE)
            .setDepthTest(false, false)
            .setColorFormat(colorFormat)
            .addColorFormat(VK_FORMAT_R16G16_SFLOAT) // motion vectors
            .setDepthFormat(VK_FORMAT_D32_SFLOAT)
            .setLayout(layout)
            .build(device, cache);
    }

    VkPipeline buildScenePipeline(VkDevice device,
                                  VkShaderModule vert, VkShaderModule frag,
                                  VkPipelineLayout layout, VkFormat colorFormat,
                                  VkPipelineCache cache)
    {
        VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(MeshVertex, uv)},
            {3, 0, VK_FORMAT_R32G32B32A32_UINT,   offsetof(MeshVertex, joints)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, weights)},
        };

        return VkPipeBuilder()
            .setShaders(vert, frag)
            .setVertexBinding(sizeof(MeshVertex), attrs, 5)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setCullMode(VK_CULL_MODE_BACK_BIT)
            .setDepthTest(true, true)
            .setColorFormat(colorFormat)
            .addColorFormat(VK_FORMAT_R16G16_SFLOAT) // motion vectors
            .setDepthFormat(VK_FORMAT_D32_SFLOAT)
            .setLayout(layout)
            .build(device, cache);
    }

    VkPipeline buildSkinnedMeshPipeline(VkDevice device,
                                       VkShaderModule vert, VkShaderModule frag,
                                       VkPipelineLayout layout, VkFormat colorFormat,
                                       VkPipelineCache cache)
    {
        // Same vertex layout as buildScenePipeline (MeshVertex, 64B)
        VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(MeshVertex, uv)},
            {3, 0, VK_FORMAT_R32G32B32A32_UINT,   offsetof(MeshVertex, joints)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, weights)},
        };

        return VkPipeBuilder()
            .setShaders(vert, frag)
            .setVertexBinding(sizeof(MeshVertex), attrs, 5)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setCullMode(VK_CULL_MODE_BACK_BIT)
            .setDepthTest(true, true)
            .setColorFormat(colorFormat)
            .addColorFormat(VK_FORMAT_R16G16_SFLOAT) // motion vectors
            .setDepthFormat(VK_FORMAT_D32_SFLOAT)
            .setLayout(layout)
            .build(device, cache);
    }

    VkPipeline buildSkinnedMeshPipelineBlend(VkDevice device,
                                           VkShaderModule vert, VkShaderModule frag,
                                           VkPipelineLayout layout, VkFormat colorFormat,
                                           VkPipelineCache cache)
    {
        VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(MeshVertex, uv)},
            {3, 0, VK_FORMAT_R32G32B32A32_UINT,   offsetof(MeshVertex, joints)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, weights)},
        };

        return VkPipeBuilder()
            .setShaders(vert, frag)
            .setVertexBinding(sizeof(MeshVertex), attrs, 5)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setCullMode(VK_CULL_MODE_NONE)          // hair/eyelash: double-sided
            .setDepthTest(true, false)                // test yes, write no
            .setBlendEnabled(true)
            .setColorFormat(colorFormat)
            .addColorFormat(VK_FORMAT_R16G16_SFLOAT)  // motion vectors
            .setDepthFormat(VK_FORMAT_D32_SFLOAT)
            .setLayout(layout)
            .build(device, cache);
    }

    VkPipeline buildSkinnedShadowPipeline(VkDevice device,
                                          VkShaderModule vert, VkShaderModule frag,
                                          VkPipelineLayout layout,
                                          VkPipelineCache cache)
    {
        VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(MeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(MeshVertex, uv)},
            {3, 0, VK_FORMAT_R32G32B32A32_UINT,   offsetof(MeshVertex, joints)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, weights)},
        };

        return VkPipeBuilder()
            .setShaders(vert, frag)
            .setVertexBinding(sizeof(MeshVertex), attrs, 5)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setCullMode(VK_CULL_MODE_BACK_BIT)
            .setDepthTest(true, true)
            .setDepthOnly(true)
            .setDepthBias(1.5f, 3.0f)
            .setLayout(layout)
            .build(device, cache);
    }

} // namespace sv
