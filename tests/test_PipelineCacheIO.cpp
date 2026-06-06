// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── VkPipelineCache persistence — pure helper tests ───────
// Exercises the no-Vulkan-required functions in PipelineCache.h:
//
//   * validatePipelineCacheBlob — 32-byte header parser + compare
//   * readPipelineCacheFile     — file → byte vector
//   * writePipelineCacheFile    — byte vector → atomic file write
//
// The RAII class sv::PipelineCache itself needs a real VkDevice +
// VkPhysicalDevice to exercise, so it is validated at runtime by the
// skinned-test lab harness rather than here. Everything below runs
// without any Vulkan state.

#include <catch2/catch_test_macros.hpp>

#include "engine/vk/PipelineCache.h"
#include "test_util.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using sv::PipelineCacheValidation;
using sv::readPipelineCacheFile;
using sv::validatePipelineCacheBlob;
using sv::writePipelineCacheFile;

namespace {

// Build a 32-byte VkPipelineCacheHeaderVersionOne blob with the given
// vendorID, deviceID, and UUID — mirrors the on-disk layout we expect
// the Vulkan driver to produce. The payload beyond the header is
// zero-filled; validatePipelineCacheBlob only inspects bytes 0..31.
std::vector<uint8_t> makeValidHeaderBlob(
    uint32_t vendorID, uint32_t deviceID,
    const uint8_t (&uuid)[16],
    std::size_t totalSize = 64)
{
    REQUIRE(totalSize >= 32);
    std::vector<uint8_t> blob(totalSize, 0);

    // Little-endian u32 writes.
    auto writeU32 = [&](std::size_t off, uint32_t v) {
        blob[off + 0] = (uint8_t)( v        & 0xFF);
        blob[off + 1] = (uint8_t)((v >>  8) & 0xFF);
        blob[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        blob[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    };

    writeU32(0,  32);          // headerLength = 32
    writeU32(4,  1);           // headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE
    writeU32(8,  vendorID);
    writeU32(12, deviceID);
    std::memcpy(blob.data() + 16, uuid, 16);

    // Fill the "driver payload" with a repeating marker so the file
    // I/O round-trip tests can verify byte-for-byte equality.
    for (std::size_t i = 32; i < blob.size(); ++i)
        blob[i] = static_cast<uint8_t>(0xA0 + (i - 32));
    return blob;
}

constexpr uint8_t kTestUuid[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
};

} // namespace

// ════════════════════════════════════════════════════════════════════
// validatePipelineCacheBlob — header parsing + mismatch detection
// ════════════════════════════════════════════════════════════════════

TEST_CASE("validatePipelineCacheBlob: matching header accepts the blob",
          "[PipelineCache]")
{
    auto blob = makeValidHeaderBlob(0x10DE, 0x2484, kTestUuid);
    REQUIRE(validatePipelineCacheBlob(blob, 0x10DE, 0x2484, kTestUuid)
            == PipelineCacheValidation::Ok);
}

TEST_CASE("validatePipelineCacheBlob: empty / truncated blob rejected",
          "[PipelineCache]")
{
    std::vector<uint8_t> empty;
    REQUIRE(validatePipelineCacheBlob(empty, 0, 0, kTestUuid)
            == PipelineCacheValidation::TooSmall);

    std::vector<uint8_t> halfHeader(16, 0);
    REQUIRE(validatePipelineCacheBlob(halfHeader, 0, 0, kTestUuid)
            == PipelineCacheValidation::TooSmall);
}

TEST_CASE("validatePipelineCacheBlob: zero headerLength rejected",
          "[PipelineCache]")
{
    auto blob = makeValidHeaderBlob(0x10DE, 0x2484, kTestUuid);
    // Zero-out headerLength — simulates a truncated or corrupt blob.
    blob[0] = blob[1] = blob[2] = blob[3] = 0;
    REQUIRE(validatePipelineCacheBlob(blob, 0x10DE, 0x2484, kTestUuid)
            == PipelineCacheValidation::BadHeaderLength);
}

TEST_CASE("validatePipelineCacheBlob: unsupported header version rejected",
          "[PipelineCache]")
{
    auto blob = makeValidHeaderBlob(0x10DE, 0x2484, kTestUuid);
    blob[4] = 0x02; // headerVersion = 2
    REQUIRE(validatePipelineCacheBlob(blob, 0x10DE, 0x2484, kTestUuid)
            == PipelineCacheValidation::UnsupportedVersion);
}

TEST_CASE("validatePipelineCacheBlob: vendorID mismatch rejected",
          "[PipelineCache]")
{
    auto blob = makeValidHeaderBlob(0x10DE, 0x2484, kTestUuid);
    // Expect AMD (0x1002) but the blob was produced on NVIDIA (0x10DE).
    REQUIRE(validatePipelineCacheBlob(blob, 0x1002, 0x2484, kTestUuid)
            == PipelineCacheValidation::VendorMismatch);
}

TEST_CASE("validatePipelineCacheBlob: deviceID mismatch rejected",
          "[PipelineCache]")
{
    auto blob = makeValidHeaderBlob(0x10DE, 0x2484, kTestUuid);
    REQUIRE(validatePipelineCacheBlob(blob, 0x10DE, 0x2489, kTestUuid)
            == PipelineCacheValidation::DeviceMismatch);
}

TEST_CASE("validatePipelineCacheBlob: UUID mismatch rejected",
          "[PipelineCache]")
{
    auto blob = makeValidHeaderBlob(0x10DE, 0x2484, kTestUuid);
    uint8_t other[16] = {
        0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
        0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0,
    };
    REQUIRE(validatePipelineCacheBlob(blob, 0x10DE, 0x2484, other)
            == PipelineCacheValidation::UuidMismatch);
}

// ════════════════════════════════════════════════════════════════════
// File I/O — readPipelineCacheFile / writePipelineCacheFile
// ════════════════════════════════════════════════════════════════════

TEST_CASE("readPipelineCacheFile: missing file returns false and clears out",
          "[PipelineCache]")
{
    svtest::TempDir dir("pipecache_missing");
    const auto p = (dir.path / "nope.bin").generic_string();

    std::vector<uint8_t> prefilled{1, 2, 3, 4};
    REQUIRE_FALSE(readPipelineCacheFile(p, prefilled));
    REQUIRE(prefilled.empty());
}

TEST_CASE("writePipelineCacheFile: rejects zero-length writes",
          "[PipelineCache]")
{
    svtest::TempDir dir("pipecache_empty_write");
    const auto p = (dir.path / "cache.bin").generic_string();

    const uint8_t payload[1] = { 0xAA };
    REQUIRE_FALSE(writePipelineCacheFile(p, payload, 0));
    REQUIRE_FALSE(std::filesystem::exists(p));

    // nullptr source with non-zero size should also be rejected.
    REQUIRE_FALSE(writePipelineCacheFile(p, nullptr, 16));
    REQUIRE_FALSE(std::filesystem::exists(p));
}

TEST_CASE("writePipelineCacheFile + readPipelineCacheFile round-trip",
          "[PipelineCache]")
{
    svtest::TempDir dir("pipecache_round_trip");
    const auto p = (dir.path / "cache.bin").generic_string();

    auto original = makeValidHeaderBlob(0x1002, 0x7480, kTestUuid, 128);
    REQUIRE(writePipelineCacheFile(p, original.data(), original.size()));
    REQUIRE(std::filesystem::exists(p));
    REQUIRE(std::filesystem::file_size(p) == original.size());

    std::vector<uint8_t> loaded;
    REQUIRE(readPipelineCacheFile(p, loaded));
    REQUIRE(loaded.size() == original.size());
    REQUIRE(std::memcmp(loaded.data(), original.data(), original.size()) == 0);

    // Follow-up validation agrees on the GPU identity — this is the
    // exact end-to-end happy path used at engine startup.
    REQUIRE(validatePipelineCacheBlob(loaded, 0x1002, 0x7480, kTestUuid)
            == PipelineCacheValidation::Ok);
}

TEST_CASE("writePipelineCacheFile: overwriting existing file works",
          "[PipelineCache]")
{
    svtest::TempDir dir("pipecache_overwrite");
    const auto p = (dir.path / "cache.bin").generic_string();

    // First write — creates the file.
    std::vector<uint8_t> first(64, 0xAA);
    REQUIRE(writePipelineCacheFile(p, first.data(), first.size()));

    // Second write with a different payload should replace in place,
    // verifying the remove-then-rename fallback on Windows.
    std::vector<uint8_t> second(96, 0xBB);
    REQUIRE(writePipelineCacheFile(p, second.data(), second.size()));

    std::vector<uint8_t> loaded;
    REQUIRE(readPipelineCacheFile(p, loaded));
    REQUIRE(loaded.size() == second.size());
    for (uint8_t b : loaded) REQUIRE(b == 0xBB);

    // And no ".tmp" sidecar should be left behind on success.
    REQUIRE_FALSE(std::filesystem::exists(p + ".tmp"));
}

TEST_CASE("writePipelineCacheFile: atomic replacement leaves no tmp sidecar",
          "[PipelineCache]")
{
    svtest::TempDir dir("pipecache_atomic");
    const auto p = (dir.path / "cache.bin").generic_string();

    std::vector<uint8_t> blob(40, 0x55);
    REQUIRE(writePipelineCacheFile(p, blob.data(), blob.size()));

    REQUIRE(std::filesystem::exists(p));
    REQUIRE_FALSE(std::filesystem::exists(p + ".tmp"));
}

TEST_CASE("readPipelineCacheFile: directory path is rejected, not read",
          "[PipelineCache]")
{
    svtest::TempDir dir("pipecache_dir_as_file");
    // Point at the directory itself, not a file inside it.
    const auto p = dir.path.generic_string();

    std::vector<uint8_t> out;
    REQUIRE_FALSE(readPipelineCacheFile(p, out));
    REQUIRE(out.empty());
}
