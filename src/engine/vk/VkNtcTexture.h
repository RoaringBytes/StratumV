// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>
#include <string>
#include <cstdint>
#include <vector>

namespace sv {

class VkCtx;

// Output from NTC decompression: a single BCn-format GPU texture with mipmaps.
// Exposes same view()/sampler() interface as VkTex for drop-in descriptor binding.
struct NtcOutput {
    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VkSampler     sampler    = VK_NULL_HANDLE;
    uint32_t      width      = 0;
    uint32_t      height     = 0;
    uint32_t      mipLevels  = 1;

    void destroy(VkDevice device, VmaAllocator alloc);
};

// Loads one .ntc texture set (containing color + normal + displacement for one terrain layer)
// and decompresses to individual BCn textures on the GPU.
//
// NTC decompression pipeline:
//   1. Load .ntc metadata (MLP weights, latent texture dims, output texture specs)
//   2. Upload latent texture (A4R4G4B4 array) to GPU
//   3. Upload network weights to storage buffer
//   4. LibNTC provides SPIR-V compute shader + dispatch params per mip level
//   5. Dispatch decompression → outputs BCn-format storage images
//   6. Create sampling views + anisotropic sampler
struct NtcTextureSet {
    NtcOutput color;
    NtcOutput normal;
    NtcOutput displacement;

    void destroy(VkDevice device, VmaAllocator alloc);
};

// NTC context and decompression driver.
// Call init() once at startup, then loadTextureSet() per .ntc file.
class NtcLoader {
public:
    bool init(VkCtx& ctx);
    void shutdown();

    // Load and decompress a .ntc file containing 3 textures (color, normal, displacement).
    // Returns true on success, populating outSet with GPU-resident BCn textures.
    bool loadTextureSet(VkCtx& ctx, const std::string& ntcPath, NtcTextureSet& outSet);

private:
    void* m_ntcContext = nullptr;  // ntc::IContext*
};

} // namespace sv
