// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <volk.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// ── VkPipelineCache on-disk persistence ──────────────────────────────
// Pure helpers plus a thin RAII wrapper around VkPipelineCache.
//
// On boot we attempt to read a cache blob from disk, validate that its
// 32-byte header matches the current GPU (vendorID + deviceID +
// pipelineCacheUUID), and feed it as initial data to
// vkCreatePipelineCache. At clean shutdown we pull the latest blob out
// of the driver via vkGetPipelineCacheData and write it back atomically
// (tmp file + rename). Subsequent runs skip the first-run shader
// compilation cost the driver would otherwise repeat.
//
// Vulkan itself silently discards a mismatched cache passed as initial
// data, but we validate up front so we can log *why* a reject happened,
// avoid paying the copy cost, and surface "loaded / empty" state to
// callers (useful for lab HUDs and the CI smoke path).
//
// On-disk layout is the raw Vulkan blob — no wrapper header, no
// compression. One blob per file is fine for v1.

namespace sv {

// ═══════════════════════════════════════════════════════════════════
// Pure helpers — no VkDevice required, fully unit-testable.
// ═══════════════════════════════════════════════════════════════════

// Reason codes returned by validatePipelineCacheBlob so callers and
// tests can tell "the file is fine" apart from "the file is stale".
enum class PipelineCacheValidation {
    Ok                  = 0,
    TooSmall,           // buffer smaller than the 32-byte header
    BadHeaderLength,    // header reports < 32 bytes
    UnsupportedVersion, // header version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
    VendorMismatch,     // vendorID disagrees with current GPU
    DeviceMismatch,     // deviceID disagrees with current GPU
    UuidMismatch,       // pipelineCacheUUID disagrees with current GPU
};

// Validate the raw 32-byte Vulkan pipeline cache header prefix inside
// `blob` against the current GPU's identity. Returns Ok iff every
// field agrees; returns the first mismatch reason otherwise.
PipelineCacheValidation validatePipelineCacheBlob(
    const std::vector<uint8_t>& blob,
    uint32_t                    expectedVendorID,
    uint32_t                    expectedDeviceID,
    const uint8_t               expectedCacheUUID[16]);

// Read the entire file at `path` into `out`. Returns false if the file
// does not exist, is empty, or cannot be read; `out` is always cleared
// before read.
bool readPipelineCacheFile(const std::string&    path,
                           std::vector<uint8_t>& out);

// Write `size` bytes to `path` atomically: the payload is written to
// "<path>.tmp" first, then renamed over `path`, so a crash mid-write
// can never leave a half-written cache on disk. Returns true on
// success. Rejects zero-length writes.
bool writePipelineCacheFile(const std::string& path,
                            const void*        data,
                            std::size_t        size);

// ═══════════════════════════════════════════════════════════════════
// RAII wrapper — owns a VkPipelineCache handle plus the on-disk blob.
// ═══════════════════════════════════════════════════════════════════

class PipelineCache {
public:
    // Read the blob at `path` (if present + valid + GPU-matching) and
    // create the VkPipelineCache handle. If the blob is missing or
    // rejected, creates an empty cache and logs the reason. Returns
    // true iff a VkPipelineCache handle was created (empty or loaded).
    bool load(VkDevice           device,
              VkPhysicalDevice   physicalDevice,
              const std::string& path);

    // Pull the current cache data out of the driver and write it to
    // `path`. Call right before destroy() at clean shutdown. Returns
    // false if the handle was never created or the file write failed.
    bool save(VkDevice device, const std::string& path) const;

    // Destroy the VkPipelineCache handle. Safe to call more than once.
    void destroy(VkDevice device);

    // Raw handle for pipeline creation calls. VK_NULL_HANDLE until
    // load() succeeds.
    VkPipelineCache handle() const { return m_cache; }

    // Diagnostic accessors — populated by the most recent load() call.
    bool        loadedFromFile()  const { return m_loadedFromFile; }
    std::size_t lastLoadedBytes() const { return m_lastLoadedBytes; }
    PipelineCacheValidation lastValidation() const { return m_lastValidation; }

private:
    VkPipelineCache         m_cache           = VK_NULL_HANDLE;
    bool                    m_loadedFromFile  = false;
    std::size_t             m_lastLoadedBytes = 0;
    PipelineCacheValidation m_lastValidation  = PipelineCacheValidation::Ok;
};

} // namespace sv
