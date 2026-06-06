// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "VkNtcTexture.h"
#include "VkContext.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <chrono>

#if NTC_AVAILABLE
#include <libntc/ntc.h>
#include <libntc/wrappers.h>
#include <libntc/shaders/Bindings.h>
#endif

namespace sv {

// ── NtcOutput ───────────────────────────────────────────────────

void NtcOutput::destroy(VkDevice device, VmaAllocator alloc)
{
    if (sampler)    vkDestroySampler(device, sampler, nullptr);
    if (view)       vkDestroyImageView(device, view, nullptr);
    if (image)      vmaDestroyImage(alloc, image, allocation);
    sampler    = VK_NULL_HANDLE;
    view       = VK_NULL_HANDLE;
    image      = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

// ── NtcTextureSet ───────────────────────────────────────────────

void NtcTextureSet::destroy(VkDevice device, VmaAllocator alloc)
{
    color.destroy(device, alloc);
    normal.destroy(device, alloc);
    displacement.destroy(device, alloc);
}

// ── NtcLoader ───────────────────────────────────────────────────

#if NTC_AVAILABLE

bool NtcLoader::init(VkCtx& ctx)
{
    ntc::ContextParameters params{};
    // interfaceVersion defaults to ntc::InterfaceVersion
    params.graphicsApi      = ntc::GraphicsAPI::Vulkan;
    params.vkInstance        = ctx.instance();
    params.vkPhysicalDevice  = ctx.physicalDevice();
    params.vkDevice          = ctx.device();
    params.cudaDevice        = ntc::DisableCudaDevice;

    ntc::IContext* context = nullptr;
    ntc::Status status = ntc::CreateContext(&context, params);
    // CudaUnavailable is non-fatal: context is created, CUDA just isn't available.
    // We only need Vulkan for IoL decompression, so treat it as success.
    if (status != ntc::Status::Ok && status != ntc::Status::CudaUnavailable) {
        fprintf(stderr, "[NTC] Failed to create context: %s\n", ntc::StatusToString(status));
        return false;
    }
    if (!context) {
        fprintf(stderr, "[NTC] Context pointer is null despite status=%s\n", ntc::StatusToString(status));
        return false;
    }

    m_ntcContext = context;
    if (status == ntc::Status::CudaUnavailable)
        printf("[NTC] Context initialized (Vulkan IoL mode, CUDA unavailable — OK)\n");
    else
        printf("[NTC] Context initialized (Vulkan IoL mode)\n");
    return true;
}

void NtcLoader::shutdown()
{
    if (m_ntcContext) {
        ntc::DestroyContext(static_cast<ntc::IContext*>(m_ntcContext));
        m_ntcContext = nullptr;
    }
}

// Create an RGBA8 output image that supports both storage writes (decompression)
// and sampled reads (rendering). Matches VkTex interface.
static bool createOutputImage(VkCtx& ctx, uint32_t w, uint32_t h, bool srgb, NtcOutput& out)
{
    out.width     = w;
    out.height    = h;
    out.mipLevels = (uint32_t)std::floor(std::log2((double)(std::max)(w, h))) + 1;

    VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.flags         = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = imageFormat;
    imgCI.extent        = { w, h, 1 };
    imgCI.mipLevels     = out.mipLevels;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx.allocator(), &imgCI, &allocCI,
            &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
        fprintf(stderr, "[NTC] Failed to create output image\n");
        return false;
    }

    VkImageViewCreateInfo viewCI{};
    viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image    = out.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = out.mipLevels;
    viewCI.subresourceRange.layerCount = 1;

    if (vkCreateImageView(ctx.device(), &viewCI, nullptr, &out.view) != VK_SUCCESS) {
        fprintf(stderr, "[NTC] Failed to create output image view\n");
        return false;
    }

    VkSamplerCreateInfo sampCI{};
    sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter    = VK_FILTER_LINEAR;
    sampCI.minFilter    = VK_FILTER_LINEAR;
    sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampCI.anisotropyEnable = VK_TRUE;
    sampCI.maxAnisotropy    = 8.0f;
    sampCI.maxLod           = (float)(out.mipLevels - 1);

    if (vkCreateSampler(ctx.device(), &sampCI, nullptr, &out.sampler) != VK_SUCCESS) {
        fprintf(stderr, "[NTC] Failed to create output sampler\n");
        return false;
    }

    return true;
}

// Generate mipmap chain via vkCmdBlitImage after mip 0 has been written
static void generateMipmaps(VkCtx& ctx, NtcOutput& out)
{
    auto cmd = ctx.beginSingleTimeCommands();

    int32_t mipW = (int32_t)out.width;
    int32_t mipH = (int32_t)out.height;

    for (uint32_t i = 1; i < out.mipLevels; i++) {
        // Transition previous mip to TRANSFER_SRC
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image               = out.image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1 };
        barrier.oldLayout     = (i == 1) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Transition current mip to TRANSFER_DST
        barrier.subresourceRange.baseMipLevel = i;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        int32_t nextW = (std::max)(1, mipW / 2);
        int32_t nextH = (std::max)(1, mipH / 2);

        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
        blit.srcOffsets[0]  = { 0, 0, 0 };
        blit.srcOffsets[1]  = { mipW, mipH, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        blit.dstOffsets[0]  = { 0, 0, 0 };
        blit.dstOffsets[1]  = { nextW, nextH, 1 };
        vkCmdBlitImage(cmd,
            out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // Transition previous mip back to SHADER_READ_ONLY
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        mipW = nextW;
        mipH = nextH;
    }

    // Transition last mip to SHADER_READ_ONLY
    VkImageMemoryBarrier lastBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    lastBarrier.image               = out.image;
    lastBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lastBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lastBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, out.mipLevels - 1, 1, 0, 1 };
    lastBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    lastBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    lastBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    lastBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &lastBarrier);

    ctx.endSingleTimeCommands(cmd);
}

bool NtcLoader::loadTextureSet(VkCtx& ctx, const std::string& ntcPath, NtcTextureSet& outSet)
{
    auto* context = static_cast<ntc::IContext*>(m_ntcContext);
    if (!context) {
        fprintf(stderr, "[NTC] Context not initialized\n");
        return false;
    }

    // ── Step 1: Open .ntc file and read metadata ────────────────

    ntc::FileStreamWrapper inputFile(context);
    ntc::Status status = context->OpenFile(ntcPath.c_str(), false, inputFile.ptr());
    if (status != ntc::Status::Ok) {
        fprintf(stderr, "[NTC] Failed to open: %s (%s)\n", ntcPath.c_str(), ntc::StatusToString(status));
        return false;
    }

    ntc::TextureSetMetadataWrapper metadata(context);
    status = context->CreateTextureSetMetadataFromStream(inputFile.Get(), metadata.ptr());
    if (status != ntc::Status::Ok) {
        fprintf(stderr, "[NTC] Failed to read metadata: %s (%s)\n", ntcPath.c_str(), ntc::StatusToString(status));
        return false;
    }

    ntc::TextureSetDesc const& desc = metadata->GetDesc();
    int numTextures = metadata->GetTextureCount();
    printf("[NTC] Loading %s: %d textures, %dx%d, %d channels\n",
        ntcPath.c_str(), numTextures, desc.width, desc.height, desc.channels);

    if (numTextures < 3) {
        fprintf(stderr, "[NTC] Expected 3 textures (color/normal/disp), got %d\n", numTextures);
        return false;
    }

    // ── Step 2: Create latent texture (input to MLP) ────────────

    ntc::LatentTextureDesc latDesc = metadata->GetLatentTextureDesc();

    VkImage latentImage = VK_NULL_HANDLE;
    VmaAllocation latentAlloc = VK_NULL_HANDLE;
    VkImageView latentView = VK_NULL_HANDLE;
    VkSampler latentSampler = VK_NULL_HANDLE;

    {
        VkImageCreateInfo imgCI{};
        imgCI.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgCI.imageType   = VK_IMAGE_TYPE_2D;
        imgCI.format      = VK_FORMAT_A4R4G4B4_UNORM_PACK16;
        imgCI.extent      = { (uint32_t)latDesc.width, (uint32_t)latDesc.height, 1 };
        imgCI.mipLevels   = (uint32_t)latDesc.mipLevels;
        imgCI.arrayLayers = (uint32_t)latDesc.arraySize;
        imgCI.samples     = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling      = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(ctx.allocator(), &imgCI, &allocCI,
                &latentImage, &latentAlloc, nullptr) != VK_SUCCESS) {
            fprintf(stderr, "[NTC] Failed to create latent texture\n");
            return false;
        }

        VkImageViewCreateInfo viewCI{};
        viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCI.image    = latentImage;
        viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewCI.format   = VK_FORMAT_A4R4G4B4_UNORM_PACK16;
        viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCI.subresourceRange.levelCount = (uint32_t)latDesc.mipLevels;
        viewCI.subresourceRange.layerCount = (uint32_t)latDesc.arraySize;

        vkCreateImageView(ctx.device(), &viewCI, nullptr, &latentView);

        VkSamplerCreateInfo sampCI{};
        sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.maxLod       = (float)latDesc.mipLevels;

        vkCreateSampler(ctx.device(), &sampCI, nullptr, &latentSampler);
    }

    // ── Step 3: Upload latent data per mip/layer ────────────────

    {
        auto cmd = ctx.beginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = latentImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = (uint32_t)latDesc.mipLevels;
        barrier.subresourceRange.layerCount = (uint32_t)latDesc.arraySize;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        ctx.endSingleTimeCommands(cmd);

        for (int mip = 0; mip < latDesc.mipLevels; mip++) {
            for (int layer = 0; layer < latDesc.arraySize; layer++) {
                ntc::LatentTextureFootprint footprint{};
                metadata->GetLatentTextureFootprint(mip, layer, footprint);

                size_t dataSize = footprint.buffer.rangeInStream.size;
                if (dataSize == 0) continue;

                std::vector<uint8_t> cpuData(dataSize);
                inputFile->Seek(footprint.buffer.rangeInStream.offset);
                inputFile->Read(cpuData.data(), dataSize);

                // GDeflate CPU decompression if needed
                std::vector<uint8_t> decompressed;
                const uint8_t* uploadData = cpuData.data();
                size_t uploadSize = dataSize;

                if (footprint.buffer.compressionType == ntc::CompressionType::GDeflate) {
                    size_t decompSize = (size_t)footprint.buffer.uncompressedSize;
                    decompressed.resize(decompSize);
                    status = context->DecompressBuffer(
                        ntc::CompressionType::GDeflate,
                        cpuData.data(), dataSize,
                        decompressed.data(), decompSize);
                    if (status != ntc::Status::Ok) {
                        fprintf(stderr, "[NTC] GDeflate decompress failed mip %d layer %d\n", mip, layer);
                        continue;
                    }
                    uploadData = decompressed.data();
                    uploadSize = decompSize;
                }

                VkBuffer stagingBuf;
                VmaAllocation stagingAlloc;
                {
                    VkBufferCreateInfo bufCI{};
                    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    bufCI.size  = uploadSize;
                    bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                    VmaAllocationCreateInfo aCI{};
                    aCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;
                    vmaCreateBuffer(ctx.allocator(), &bufCI, &aCI, &stagingBuf, &stagingAlloc, nullptr);
                    void* mapped;
                    vmaMapMemory(ctx.allocator(), stagingAlloc, &mapped);
                    memcpy(mapped, uploadData, uploadSize);
                    vmaUnmapMemory(ctx.allocator(), stagingAlloc);
                }

                uint32_t mipW = (std::max)(1, latDesc.width >> mip);
                uint32_t mipH = (std::max)(1, latDesc.height >> mip);

                auto copyCmd = ctx.beginSingleTimeCommands();
                VkBufferImageCopy region{};
                region.bufferRowLength   = (uint32_t)(footprint.rowPitch / 2); // 2 bytes per A4R4G4B4
                region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel       = (uint32_t)mip;
                region.imageSubresource.baseArrayLayer = (uint32_t)layer;
                region.imageSubresource.layerCount     = 1;
                region.imageExtent = { mipW, mipH, 1 };
                vkCmdCopyBufferToImage(copyCmd, stagingBuf, latentImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                ctx.endSingleTimeCommands(copyCmd);

                vmaDestroyBuffer(ctx.allocator(), stagingBuf, stagingAlloc);
            }
        }

        // Transition latent to SHADER_READ_ONLY
        cmd = ctx.beginSingleTimeCommands();
        VkImageMemoryBarrier readBarrier{};
        readBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        readBarrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        readBarrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        readBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readBarrier.image               = latentImage;
        readBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        readBarrier.subresourceRange.levelCount = (uint32_t)latDesc.mipLevels;
        readBarrier.subresourceRange.layerCount = (uint32_t)latDesc.arraySize;
        readBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        readBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &readBarrier);
        ctx.endSingleTimeCommands(cmd);
    }

    // ── Step 4: Upload MLP network weights ──────────────────────

    ntc::InferenceWeightType weightType = metadata->GetBestSupportedWeightType();

    void const* pWeightData = nullptr;
    size_t weightUploadSize = 0, weightConvertedSize = 0;
    metadata->GetInferenceWeights(weightType, &pWeightData, &weightUploadSize, &weightConvertedSize);

    VkBuffer weightBuffer = VK_NULL_HANDLE;
    VmaAllocation weightAlloc = VK_NULL_HANDLE;

    {
        size_t bufSize = (weightConvertedSize > 0) ? weightConvertedSize : weightUploadSize;

        VkBufferCreateInfo bufCI{};
        bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufCI.size  = bufSize;
        bufCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aCI{};
        aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateBuffer(ctx.allocator(), &bufCI, &aCI, &weightBuffer, &weightAlloc, nullptr);

        if (weightConvertedSize > 0) {
            // Upload raw weights, then run GPU conversion
            VkBuffer stagingBuf;
            VmaAllocation stagingAlloc;
            {
                VkBufferCreateInfo sCI{};
                sCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                sCI.size  = weightUploadSize;
                sCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VmaAllocationCreateInfo saCI{};
                saCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;
                vmaCreateBuffer(ctx.allocator(), &sCI, &saCI, &stagingBuf, &stagingAlloc, nullptr);
                void* mapped;
                vmaMapMemory(ctx.allocator(), stagingAlloc, &mapped);
                memcpy(mapped, pWeightData, weightUploadSize);
                vmaUnmapMemory(ctx.allocator(), stagingAlloc);
            }

            auto cmd = ctx.beginSingleTimeCommands();
            metadata->ConvertInferenceWeights(weightType, cmd, stagingBuf, 0, weightBuffer, 0);
            VkBufferMemoryBarrier bufBarrier{};
            bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bufBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bufBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufBarrier.buffer = weightBuffer;
            bufBarrier.size   = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &bufBarrier, 0, nullptr);
            ctx.endSingleTimeCommands(cmd);
            vmaDestroyBuffer(ctx.allocator(), stagingBuf, stagingAlloc);
        } else {
            // Direct upload
            VkBuffer stagingBuf;
            VmaAllocation stagingAlloc;
            {
                VkBufferCreateInfo sCI{};
                sCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                sCI.size  = weightUploadSize;
                sCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VmaAllocationCreateInfo saCI{};
                saCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;
                vmaCreateBuffer(ctx.allocator(), &sCI, &saCI, &stagingBuf, &stagingAlloc, nullptr);
                void* mapped;
                vmaMapMemory(ctx.allocator(), stagingAlloc, &mapped);
                memcpy(mapped, pWeightData, weightUploadSize);
                vmaUnmapMemory(ctx.allocator(), stagingAlloc);
            }
            auto cmd = ctx.beginSingleTimeCommands();
            VkBufferCopy region{};
            region.size = weightUploadSize;
            vkCmdCopyBuffer(cmd, stagingBuf, weightBuffer, 1, &region);
            ctx.endSingleTimeCommands(cmd);
            vmaDestroyBuffer(ctx.allocator(), stagingBuf, stagingAlloc);
        }
    }

    // ── Step 5: Create output RGBA8 textures ────────────────────

    // Output ordering: index 0 = color (sRGB), 1 = normal (linear), 2 = displacement (linear)
    NtcOutput* outputs[3] = { &outSet.color, &outSet.normal, &outSet.displacement };
    bool isSrgb[3]        = { true, false, false };
    bool outputsOk = true;

    for (int i = 0; i < 3 && i < numTextures; i++) {
        if (!createOutputImage(ctx, (uint32_t)desc.width, (uint32_t)desc.height, isSrgb[i], *outputs[i])) {
            fprintf(stderr, "[NTC] Failed to create output texture %d\n", i);
            outputsOk = false;
            break;
        }
    }

    if (!outputsOk) goto cleanup;

    // ── Step 6: Decompression compute dispatch (mip 0 only) ──

    {
        // Create storage views for compute writes (UNORM, not SRGB)
        VkImageView storageViews[3] = {};
        for (int i = 0; i < 3; i++) {
            VkImageViewCreateInfo svCI{};
            svCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            svCI.image    = outputs[i]->image;
            svCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
            svCI.format   = VK_FORMAT_R8G8B8A8_UNORM; // Storage writes use UNORM
            svCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            svCI.subresourceRange.levelCount = 1;
            svCI.subresourceRange.layerCount = 1;
            vkCreateImageView(ctx.device(), &svCI, nullptr, &storageViews[i]);
        }

        // Get decompression compute pass from LibNTC (mip 0, default output mapping)
        ntc::MakeDecompressionComputePassParameters decompParams{};
        decompParams.textureSetMetadata      = metadata.Get();
        decompParams.weightType              = weightType;
        decompParams.weightOffset            = 0;
        decompParams.mipLevel                = 0;
        decompParams.firstLatentMipInTexture = 0;
        decompParams.firstOutputDescriptorIndex = 0;
        // pOutputTextures = nullptr → use metadata-derived mapping

        ntc::ComputePassDesc computePass{};
        status = context->MakeDecompressionComputePass(decompParams, &computePass);
        if (status != ntc::Status::Ok) {
            fprintf(stderr, "[NTC] MakeDecompressionComputePass failed: %s\n", ntc::StatusToString(status));
            for (int i = 0; i < 3; i++) vkDestroyImageView(ctx.device(), storageViews[i], nullptr);
            goto cleanup;
        }

        // Create compute pipeline from LibNTC-provided SPIR-V
        VkShaderModuleCreateInfo smCI{};
        smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCI.codeSize = computePass.computeShaderSize;
        smCI.pCode    = (const uint32_t*)computePass.computeShader;
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        vkCreateShaderModule(ctx.device(), &smCI, nullptr, &shaderModule);

        // Descriptor set layout (from Bindings.h):
        // Set 0: b0=UBO, t1=latent sampled image, t2=weight SSBO, s3=latent sampler
        // Set 1: u0,u1,u2=output storage images
        VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout set1Layout = VK_NULL_HANDLE;
        {
            VkDescriptorSetLayoutBinding bindings0[] = {
                { NTC_BINDING_DECOMPRESSION_CONSTANT_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { NTC_BINDING_DECOMPRESSION_LATENT_TEXTURE,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,     1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { NTC_BINDING_DECOMPRESSION_WEIGHT_BUFFER,   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { NTC_BINDING_DECOMPRESSION_LATENT_SAMPLER,  VK_DESCRIPTOR_TYPE_SAMPLER,           1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            VkDescriptorSetLayoutCreateInfo layoutCI{};
            layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutCI.bindingCount = 4;
            layoutCI.pBindings    = bindings0;
            vkCreateDescriptorSetLayout(ctx.device(), &layoutCI, nullptr, &set0Layout);

            VkDescriptorSetLayoutBinding bindings1[3];
            for (int i = 0; i < 3; i++) {
                bindings1[i] = { (uint32_t)i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            }
            VkDescriptorSetLayoutCreateInfo layout1CI{};
            layout1CI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout1CI.bindingCount = 3;
            layout1CI.pBindings    = bindings1;
            vkCreateDescriptorSetLayout(ctx.device(), &layout1CI, nullptr, &set1Layout);
        }

        VkDescriptorSetLayout setLayouts[] = { set0Layout, set1Layout };
        VkPipelineLayoutCreateInfo plCI{};
        plCI.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCI.setLayoutCount = 2;
        plCI.pSetLayouts    = setLayouts;
        VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
        vkCreatePipelineLayout(ctx.device(), &plCI, nullptr, &pipeLayout);

        VkPipelineShaderStageCreateInfo stageCI{};
        stageCI.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stageCI.module = shaderModule;
        stageCI.pName  = "main";
        VkComputePipelineCreateInfo cpCI{};
        cpCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpCI.stage  = stageCI;
        cpCI.layout = pipeLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        vkCreateComputePipelines(ctx.device(), VK_NULL_HANDLE, 1, &cpCI, nullptr, &pipeline);

        // Descriptor pool + sets
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1 },
            { VK_DESCRIPTOR_TYPE_SAMPLER,         1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   3 },
        };
        VkDescriptorPoolCreateInfo dpCI{};
        dpCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpCI.maxSets       = 2;
        dpCI.poolSizeCount = 5;
        dpCI.pPoolSizes    = poolSizes;
        VkDescriptorPool descPool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(ctx.device(), &dpCI, nullptr, &descPool);

        VkDescriptorSet descSet0 = VK_NULL_HANDLE, descSet1 = VK_NULL_HANDLE;
        {
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool     = descPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts        = &set0Layout;
            vkAllocateDescriptorSets(ctx.device(), &allocInfo, &descSet0);
            allocInfo.pSetLayouts = &set1Layout;
            vkAllocateDescriptorSets(ctx.device(), &allocInfo, &descSet1);
        }

        // Upload constant buffer
        VkBuffer constBuf = VK_NULL_HANDLE;
        VmaAllocation constAlloc = VK_NULL_HANDLE;
        {
            VkBufferCreateInfo bufCI{};
            bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufCI.size  = computePass.constantBufferSize;
            bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aCI{};
            aCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx.allocator(), &bufCI, &aCI, &constBuf, &constAlloc, &allocInfo);
            memcpy(allocInfo.pMappedData, computePass.constantBufferData, computePass.constantBufferSize);
        }

        // Write set 0
        VkDescriptorBufferInfo constBufInfo{ constBuf, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo latentImgInfo{ VK_NULL_HANDLE, latentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo weightBufInfo{ weightBuffer, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo samplerInfo{ latentSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

        VkWriteDescriptorSet writes0[4]{};
        writes0[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet0, 0, 0, 1,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &constBufInfo, nullptr };
        writes0[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet0, 1, 0, 1,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &latentImgInfo, nullptr, nullptr };
        writes0[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet0, 2, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &weightBufInfo, nullptr };
        writes0[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet0, 3, 0, 1,
            VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr, nullptr };
        vkUpdateDescriptorSets(ctx.device(), 4, writes0, 0, nullptr);

        // Write set 1 (output storage images)
        VkDescriptorImageInfo outImgInfos[3];
        VkWriteDescriptorSet writes1[3];
        for (int i = 0; i < 3; i++) {
            outImgInfos[i] = { VK_NULL_HANDLE, storageViews[i], VK_IMAGE_LAYOUT_GENERAL };
            writes1[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet1,
                (uint32_t)i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outImgInfos[i], nullptr, nullptr };
        }
        vkUpdateDescriptorSets(ctx.device(), 3, writes1, 0, nullptr);

        // Dispatch decompression
        auto cmd = ctx.beginSingleTimeCommands();

        // Transition output images to GENERAL for compute writes
        VkImageMemoryBarrier outBarriers[3]{};
        for (int i = 0; i < 3; i++) {
            outBarriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            outBarriers[i].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            outBarriers[i].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            outBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            outBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            outBarriers[i].image               = outputs[i]->image;
            outBarriers[i].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            outBarriers[i].dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 3, outBarriers);

        VkDescriptorSet sets[] = { descSet0, descSet1 };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 2, sets, 0, nullptr);
        vkCmdDispatch(cmd, computePass.dispatchWidth, computePass.dispatchHeight, 1);

        // Transition outputs to SHADER_READ_ONLY for fragment sampling
        for (int i = 0; i < 3; i++) {
            outBarriers[i].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            outBarriers[i].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            outBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            outBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 3, outBarriers);

        ctx.endSingleTimeCommands(cmd);

        // Cleanup per-dispatch resources
        for (int i = 0; i < 3; i++) vkDestroyImageView(ctx.device(), storageViews[i], nullptr);
        vmaDestroyBuffer(ctx.allocator(), constBuf, constAlloc);
        vkDestroyDescriptorPool(ctx.device(), descPool, nullptr);
        vkDestroyPipeline(ctx.device(), pipeline, nullptr);
        vkDestroyPipelineLayout(ctx.device(), pipeLayout, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device(), set0Layout, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device(), set1Layout, nullptr);
        vkDestroyShaderModule(ctx.device(), shaderModule, nullptr);
    }

    // Generate mipmap chains from decompressed mip 0
    for (int i = 0; i < 3 && i < numTextures; i++) {
        generateMipmaps(ctx, *outputs[i]);
    }

    printf("[NTC] Decompressed %s → %dx%d RGBA8 (%u mips, 3 textures)\n",
        ntcPath.c_str(), desc.width, desc.height, outputs[0]->mipLevels);

cleanup:
    vkDestroySampler(ctx.device(), latentSampler, nullptr);
    vkDestroyImageView(ctx.device(), latentView, nullptr);
    vmaDestroyImage(ctx.allocator(), latentImage, latentAlloc);
    vmaDestroyBuffer(ctx.allocator(), weightBuffer, weightAlloc);

    return (outSet.color.image && outSet.normal.image && outSet.displacement.image);
}

#else // !NTC_AVAILABLE

bool NtcLoader::init(VkCtx&)
{
    printf("[NTC] RTXNTC not available (ENABLE_NTC=OFF)\n");
    return false;
}

void NtcLoader::shutdown() {}

bool NtcLoader::loadTextureSet(VkCtx&, const std::string&, NtcTextureSet&)
{
    return false;
}

#endif

} // namespace sv
