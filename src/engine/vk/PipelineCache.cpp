// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ============================================================
// PipelineCache.cpp — Layer 1: VkPipelineCache on-disk persistence
// ============================================================
// Load/validate/save the Vulkan pipeline cache blob so that
// subsequent runs skip the first-run shader compile cost.

#include "PipelineCache.h"

#include "../EngineLog.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace sv {

// ═══════════════════════════════════════════════════════════════════
// Pure helpers
// ═══════════════════════════════════════════════════════════════════

namespace {

// VkPipelineCacheHeaderVersionOne layout (Vulkan spec 11.4.2) — a
// fixed 32-byte prefix before the driver payload.
constexpr std::size_t kPipelineCacheHeaderSize        = 32;
constexpr uint32_t    kPipelineCacheHeaderVersionOne  = 1;
constexpr std::size_t kPipelineCacheUuidOffset        = 16;
constexpr std::size_t kPipelineCacheUuidSize          = 16;

// Little-endian uint32 read from a raw byte pointer. The Vulkan cache
// header is always LE regardless of host endianness.
uint32_t readU32LE(const uint8_t* p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

} // namespace

PipelineCacheValidation validatePipelineCacheBlob(
    const std::vector<uint8_t>& blob,
    uint32_t                    expectedVendorID,
    uint32_t                    expectedDeviceID,
    const uint8_t               expectedCacheUUID[16])
{
    if (blob.size() < kPipelineCacheHeaderSize)
        return PipelineCacheValidation::TooSmall;

    const uint32_t headerLength  = readU32LE(blob.data() +  0);
    const uint32_t headerVersion = readU32LE(blob.data() +  4);
    const uint32_t vendorID      = readU32LE(blob.data() +  8);
    const uint32_t deviceID      = readU32LE(blob.data() + 12);

    if (headerLength < kPipelineCacheHeaderSize)
        return PipelineCacheValidation::BadHeaderLength;
    if (headerVersion != kPipelineCacheHeaderVersionOne)
        return PipelineCacheValidation::UnsupportedVersion;
    if (vendorID != expectedVendorID)
        return PipelineCacheValidation::VendorMismatch;
    if (deviceID != expectedDeviceID)
        return PipelineCacheValidation::DeviceMismatch;
    if (std::memcmp(blob.data() + kPipelineCacheUuidOffset,
                    expectedCacheUUID,
                    kPipelineCacheUuidSize) != 0)
        return PipelineCacheValidation::UuidMismatch;

    return PipelineCacheValidation::Ok;
}

bool readPipelineCacheFile(const std::string&    path,
                           std::vector<uint8_t>& out)
{
    out.clear();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return false;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return false;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    const std::streampos endPos = f.tellg();
    if (endPos <= std::streampos(0)) return false;

    const auto size = static_cast<std::size_t>(endPos);
    out.resize(size);

    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(size));

    // good() and eof() both mean "read the full request"; ifstream
    // sets eof on exact-size reads.
    if (!f) {
        out.clear();
        return false;
    }
    return true;
}

bool writePipelineCacheFile(const std::string& path,
                            const void*        data,
                            std::size_t        size)
{
    if (size == 0 || data == nullptr) return false;

    const std::string tmp = path + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f.write(reinterpret_cast<const char*>(data),
                static_cast<std::streamsize>(size));
        if (!f.good()) return false;
    }

    // std::filesystem::rename on Windows fails if the destination
    // already exists. Fall back to remove-then-rename in that case.
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code rmec;
        std::filesystem::remove(path, rmec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec); // best-effort cleanup
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// PipelineCache — RAII wrapper
// ═══════════════════════════════════════════════════════════════════

bool PipelineCache::load(VkDevice           device,
                         VkPhysicalDevice   physicalDevice,
                         const std::string& path)
{
    m_loadedFromFile  = false;
    m_lastLoadedBytes = 0;
    m_lastValidation  = PipelineCacheValidation::Ok;

    // Query device identity so we can validate whatever we read.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    std::vector<uint8_t> blob;
    const bool fileExists = readPipelineCacheFile(path, blob);

    const void* initialData = nullptr;
    std::size_t initialSize = 0;

    if (fileExists) {
        m_lastValidation = validatePipelineCacheBlob(
            blob, props.vendorID, props.deviceID, props.pipelineCacheUUID);
        if (m_lastValidation == PipelineCacheValidation::Ok) {
            initialData       = blob.data();
            initialSize       = blob.size();
            m_lastLoadedBytes = blob.size();
            m_loadedFromFile  = true;
            SV_LOG_INFO("PipelineCache",
                "Loaded %zu bytes from %s",
                blob.size(), path.c_str());
        } else {
            SV_LOG_WARN("PipelineCache",
                "Rejecting cache %s (validation=%d) — starting empty",
                path.c_str(), (int)m_lastValidation);
        }
    } else {
        SV_LOG_INFO("PipelineCache",
            "No cache file at %s — starting empty", path.c_str());
    }

    VkPipelineCacheCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = initialSize;
    ci.pInitialData    = initialData;

    const VkResult vr = vkCreatePipelineCache(device, &ci, nullptr, &m_cache);
    if (vr != VK_SUCCESS) {
        SV_LOG_ERROR("PipelineCache",
            "vkCreatePipelineCache failed (VkResult=%d)", (int)vr);
        m_cache = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool PipelineCache::save(VkDevice device, const std::string& path) const
{
    if (m_cache == VK_NULL_HANDLE) return false;

    std::size_t size = 0;
    if (vkGetPipelineCacheData(device, m_cache, &size, nullptr) != VK_SUCCESS
        || size == 0)
    {
        SV_LOG_WARN("PipelineCache",
            "vkGetPipelineCacheData returned zero bytes — nothing to save");
        return false;
    }

    std::vector<uint8_t> blob(size);
    const VkResult vr = vkGetPipelineCacheData(device, m_cache, &size, blob.data());
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        SV_LOG_ERROR("PipelineCache",
            "vkGetPipelineCacheData failed (VkResult=%d)", (int)vr);
        return false;
    }
    // Vulkan may shrink size between the two calls — trim.
    blob.resize(size);

    const bool ok = writePipelineCacheFile(path, blob.data(), blob.size());
    if (ok) {
        SV_LOG_INFO("PipelineCache",
            "Saved %zu bytes to %s", blob.size(), path.c_str());
    } else {
        SV_LOG_WARN("PipelineCache",
            "Failed to save %zu bytes to %s", blob.size(), path.c_str());
    }
    return ok;
}

void PipelineCache::destroy(VkDevice device)
{
    if (m_cache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device, m_cache, nullptr);
        m_cache = VK_NULL_HANDLE;
    }
    m_loadedFromFile  = false;
    m_lastLoadedBytes = 0;
    m_lastValidation  = PipelineCacheValidation::Ok;
}

} // namespace sv
