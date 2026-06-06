// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetWatcher recursion tests ───────────────────────
// Covers the new recursive directory walk, its case-insensitive
// extension matching, and change detection via checkAll() /
// forceCheck(). Uses the same TempDir pattern as test_AssetBrowser.cpp
// so nothing leaks into the shared fixture tree.

#include "AssetWatcher.h"
#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

using sv::AssetWatcher;
using svtest::TempDir;
using svtest::writeFile;

// ── watchDirectoryRecursive: basic file discovery ──────────────────

TEST_CASE("AssetWatcher: watchDirectoryRecursive registers nested files",
          "[asset-watcher][recursive]") {
    TempDir tmp("watcher_discover");

    // Tree:
    //   root/a.glb
    //   root/sub/b.glb
    //   root/sub/deeper/c.glb
    //   root/sub/deeper/note.txt     (excluded by extension)
    //   root/other/d.png             (excluded by extension filter)
    writeFile(tmp.path / "a.glb", "x");
    writeFile(tmp.path / "sub" / "b.glb", "y");
    writeFile(tmp.path / "sub" / "deeper" / "c.glb", "z");
    writeFile(tmp.path / "sub" / "deeper" / "note.txt", "text");
    writeFile(tmp.path / "other" / "d.png", "pixels");

    AssetWatcher w;
    int fires = 0;
    size_t count = w.watchDirectoryRecursive(
        tmp.str(), {".glb"}, [&fires]() { ++fires; });

    // Three .glb files across three directory levels.
    REQUIRE(count == 3);
}

TEST_CASE("AssetWatcher: watchDirectoryRecursive matches extensions case-insensitively",
          "[asset-watcher][recursive]") {
    TempDir tmp("watcher_case");
    writeFile(tmp.path / "LOWER.glb", "a");
    writeFile(tmp.path / "UPPER.GLB", "b");
    writeFile(tmp.path / "nested" / "Mixed.GlB", "c");

    AssetWatcher w;
    size_t count = w.watchDirectoryRecursive(
        tmp.str(), {".glb"}, []() {});

    REQUIRE(count == 3);
}

TEST_CASE("AssetWatcher: watchDirectoryRecursive returns 0 for missing dir",
          "[asset-watcher][recursive]") {
    AssetWatcher w;
    size_t count = w.watchDirectoryRecursive(
        "C:/definitely_not_a_real_directory_stratumv_test",
        {".glb"}, []() {});
    REQUIRE(count == 0);
}

TEST_CASE("AssetWatcher: watchDirectoryRecursive reports 0 in empty dir",
          "[asset-watcher][recursive]") {
    TempDir tmp("watcher_empty");
    AssetWatcher w;
    size_t count = w.watchDirectoryRecursive(
        tmp.str(), {".glb"}, []() {});
    REQUIRE(count == 0);
}

// ── watchDirectoryRecursive: change detection ──────────────────────

TEST_CASE("AssetWatcher: forceCheck fires callback on nested file change",
          "[asset-watcher][recursive]") {
    TempDir tmp("watcher_nested_change");

    std::filesystem::path nested = tmp.path / "sub" / "deeper" / "asset.glb";
    writeFile(nested, "initial");

    AssetWatcher w;
    int fires = 0;
    size_t count = w.watchDirectoryRecursive(
        tmp.str(), {".glb"}, [&fires]() { ++fires; });
    REQUIRE(count == 1);

    // Baseline: forceCheck right after watch should not fire anything.
    REQUIRE_FALSE(w.forceCheck());
    REQUIRE(fires == 0);

    // filesystem last_write_time has limited granularity on some
    // platforms — bump it explicitly so the change is unambiguous.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::error_code ec;
        auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(nested, now, ec);
    }

    REQUIRE(w.forceCheck());
    REQUIRE(fires == 1);
}

// ── watchDirectory (non-recursive): case-insensitive regression ────

TEST_CASE("AssetWatcher: non-recursive watchDirectory now matches case-insensitively",
          "[asset-watcher]") {
    TempDir tmp("watcher_noncase");
    writeFile(tmp.path / "a.glb", "1");
    writeFile(tmp.path / "B.GLB", "2");
    // Nested file should NOT be picked up by the non-recursive variant.
    writeFile(tmp.path / "sub" / "c.glb", "3");

    AssetWatcher w;
    size_t count = w.watchDirectory(
        tmp.str(), {".glb"}, []() {});
    // Two top-level files (case-insensitive), nested one ignored.
    REQUIRE(count == 2);
}
