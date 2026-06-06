// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── Shared test utilities ──────────────────────────────
// Consolidates the three TempDir/writeFile/writeFakePng helpers that
// previously lived as independent copies in test_AssetBrowser.cpp,
// test_AssetWatcher.cpp, and test_ThumbnailCache.cpp. Each test file
// now includes this header and reuses a single implementation.
//
// All helpers live in namespace `svtest` to avoid colliding with
// production `sv::` symbols. The counter is a TU-local static so
// every translation unit shares it — duplicate temp-dir collisions
// across files are impossible because Catch2 tests run sequentially
// in a single process.
//
// This is a header-only utility on purpose: these helpers are only
// used by the test binary and are small enough that header inlining
// is simpler than threading yet another .cpp through the CMake list.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace svtest {

// ── Shared temp-dir counter ────────────────────────────────────────
// One atomic counter per translation unit (anonymous static via
// inline variable) — the scheme guarantees unique paths inside a
// single test process.
inline std::atomic<uint64_t>& tempCounter()
{
    static std::atomic<uint64_t> counter{0};
    return counter;
}

// ── RAII temp directory ────────────────────────────────────────────
// Creates a unique subdirectory under the OS temp path on
// construction and recursively deletes it on destruction. The
// `label` argument is appended to the directory name so failures
// are easy to identify while debugging.
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const char* label)
    {
        namespace fs = std::filesystem;
        const uint64_t id = ++tempCounter();
        path = fs::temp_directory_path() / "stratumv_tests" /
               (std::string(label) + "_" + std::to_string(id));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    // Non-copyable, non-movable — the destructor must own the path.
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&)                 = delete;
    TempDir& operator=(TempDir&&)      = delete;

    std::string str() const { return path.generic_string(); }
};

// ── writeFile: small arbitrary blob ────────────────────────────────
// Creates parent directories as needed, then writes `body` to `p`
// in binary mode. The default body makes it usable as "writeStub".
// Tests use this for every fake asset in the AssetBrowser tree —
// the browser only reads filename + size + mtime, so the payload
// bytes are irrelevant.
inline void writeFile(const std::filesystem::path& p,
                      std::string_view body = "stub")
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

// ── writeFakePng: tiny file that looks PNG-shaped ──────────────────
// ThumbnailCache::isValid only checks that the sibling file exists
// and is a regular file — the actual pixel data is never inspected.
// We still write the 8-byte PNG signature plus a small payload so
// that hex dumps during debugging make it clear the file is a test
// stub rather than empty.
inline void writeFakePng(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.write(reinterpret_cast<const char*>(sig), 8);
    out << "fake-content";
}

} // namespace svtest
