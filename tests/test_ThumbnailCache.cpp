// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── ThumbnailCache unit tests ──────────────────────────
// Pure-logic tests for sv::ThumbnailCache + thumbnailPathFor +
// isThumbnailSibling. Exercises:
//   - Sibling path composition ("<absPath>.thumb.png")
//   - isThumbnailSibling case-insensitive matching
//   - Default state (empty cache)
//   - markBaked + find / contains round-trip
//   - isValid: mtime match, mtime mismatch, missing sibling file
//   - evict with deleteFile ON (removes file) and OFF (keeps file)
//   - invalidateStale by relPath removal (asset deleted)
//   - invalidateStale by mtime advance (asset edited)
//   - clear wipes memory + sibling files
//   - AssetBrowserEntry-based invalidateStale overload
//
// Disk writes go through a TempDir RAII helper — nothing leaks into
// the shared fixture tree.

#include "ThumbnailCache.h"
#include "AssetBrowser.h"  // for AssetBrowserEntry overload
#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using sv::AssetBrowserEntry;
using sv::ThumbnailCache;
using sv::thumbnailPathFor;
using sv::isThumbnailSibling;
using svtest::TempDir;
using svtest::writeFakePng;

namespace {

// Helper: build an AssetBrowserEntry by hand for the overload test.
AssetBrowserEntry makeEntry(const std::string& rel,
                            const std::string& abs,
                            int64_t mtime)
{
    AssetBrowserEntry e;
    e.name = std::filesystem::path(rel).filename().string();
    e.relativePath = rel;
    e.absolutePath = abs;
    e.extension    = ".glb";
    e.kind         = sv::AssetKind::Mesh;
    e.sizeBytes    = 42;
    e.lastModified = mtime;
    return e;
}

} // anonymous

// ── thumbnailPathFor ───────────────────────────────────────────────

TEST_CASE("thumbnailPathFor appends .thumb.png to absolute path",
          "[thumbnail-cache][path]") {
    REQUIRE(thumbnailPathFor("C:/assets/player.glb") ==
            "C:/assets/player.glb.thumb.png");
    REQUIRE(thumbnailPathFor("/home/dev/tree.fbx") ==
            "/home/dev/tree.fbx.thumb.png");
    REQUIRE(thumbnailPathFor("just_a_name.png") ==
            "just_a_name.png.thumb.png");
    // Empty input → empty output
    REQUIRE(thumbnailPathFor("").empty());
}

// ── isThumbnailSibling ─────────────────────────────────────────────

TEST_CASE("isThumbnailSibling matches .thumb.png case-insensitively",
          "[thumbnail-cache][path]") {
    REQUIRE(isThumbnailSibling("foo.glb.thumb.png"));
    REQUIRE(isThumbnailSibling("FOO.GLB.THUMB.PNG"));
    REQUIRE(isThumbnailSibling("nested/player.thumb.png"));
    // Non-matches
    REQUIRE_FALSE(isThumbnailSibling("foo.thumbnail.png"));
    REQUIRE_FALSE(isThumbnailSibling("foo.png"));
    REQUIRE_FALSE(isThumbnailSibling(""));
    // AssetBrowser::isIgnoredFilename must agree with us.
    REQUIRE(sv::isIgnoredFilename("player.glb.thumb.png"));
    REQUIRE_FALSE(sv::isIgnoredFilename("player.glb"));
}

// ── Default state ──────────────────────────────────────────────────

TEST_CASE("ThumbnailCache: default-constructed cache is empty",
          "[thumbnail-cache]") {
    ThumbnailCache c;
    REQUIRE(c.empty());
    REQUIRE(c.size() == 0);
    REQUIRE_FALSE(c.contains("anything"));
    REQUIRE(c.find("anything") == nullptr);
    REQUIRE_FALSE(c.evict("anything"));
    // isValid on empty cache always false (no entry, regardless of mtime)
    REQUIRE_FALSE(c.isValid("anything", 12345));
}

// ── markBaked + find ──────────────────────────────────────────────

TEST_CASE("ThumbnailCache: markBaked stores entry retrievable via find",
          "[thumbnail-cache][mark]") {
    TempDir tmp("thumb_markBaked");
    auto abs = tmp.path / "mesh.glb";
    writeFakePng(tmp.path / "mesh.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("mesh.glb", abs.generic_string(), 111, 1024);

    REQUIRE(c.size() == 1);
    REQUIRE(c.contains("mesh.glb"));
    const auto* e = c.find("mesh.glb");
    REQUIRE(e != nullptr);
    REQUIRE(e->relativePath == "mesh.glb");
    REQUIRE(e->absolutePath == abs.generic_string());
    REQUIRE(e->thumbnailPath == abs.generic_string() + ".thumb.png");
    REQUIRE(e->sourceLastModified == 111);
    REQUIRE(e->byteSize == 1024);
}

// ── isValid: mtime + disk checks ──────────────────────────────────

TEST_CASE("ThumbnailCache: isValid true when mtime matches and file exists",
          "[thumbnail-cache][valid]") {
    TempDir tmp("thumb_valid_match");
    auto abs = tmp.path / "rig.glb";
    writeFakePng(tmp.path / "rig.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("rig.glb", abs.generic_string(), 999);

    REQUIRE(c.isValid("rig.glb", 999));
    // Wrong mtime → invalid
    REQUIRE_FALSE(c.isValid("rig.glb", 1000));
    // Missing entry → invalid
    REQUIRE_FALSE(c.isValid("other.glb", 999));
}

TEST_CASE("ThumbnailCache: isValid false when sibling file is missing",
          "[thumbnail-cache][valid]") {
    TempDir tmp("thumb_valid_missing_file");
    auto abs = tmp.path / "no_sibling.glb";
    // Deliberately do NOT create the .thumb.png file.

    ThumbnailCache c;
    c.markBaked("no_sibling.glb", abs.generic_string(), 222);

    REQUIRE(c.contains("no_sibling.glb"));
    // Entry in cache but file missing → invalid
    REQUIRE_FALSE(c.isValid("no_sibling.glb", 222));
}

// ── evict ──────────────────────────────────────────────────────────

TEST_CASE("ThumbnailCache: evict with deleteFile=true removes file",
          "[thumbnail-cache][evict]") {
    TempDir tmp("thumb_evict_delete");
    auto abs = tmp.path / "gone.glb";
    auto thumb = tmp.path / "gone.glb.thumb.png";
    writeFakePng(thumb);
    REQUIRE(std::filesystem::exists(thumb));

    ThumbnailCache c;
    c.markBaked("gone.glb", abs.generic_string(), 333);
    REQUIRE(c.evict("gone.glb", /*deleteFile=*/true));

    REQUIRE(c.empty());
    REQUIRE_FALSE(std::filesystem::exists(thumb));
    // Second evict returns false — nothing left to drop.
    REQUIRE_FALSE(c.evict("gone.glb"));
}

TEST_CASE("ThumbnailCache: evict with deleteFile=false leaves file on disk",
          "[thumbnail-cache][evict]") {
    TempDir tmp("thumb_evict_keep");
    auto abs = tmp.path / "keep.glb";
    auto thumb = tmp.path / "keep.glb.thumb.png";
    writeFakePng(thumb);

    ThumbnailCache c;
    c.markBaked("keep.glb", abs.generic_string(), 444);
    REQUIRE(c.evict("keep.glb", /*deleteFile=*/false));

    REQUIRE(c.empty());
    // File should STILL exist since we asked evict not to delete it.
    REQUIRE(std::filesystem::exists(thumb));
}

// ── invalidateStale: mtime advance + removal ──────────────────────

TEST_CASE("ThumbnailCache: invalidateStale drops entries whose mtime advanced",
          "[thumbnail-cache][invalidate]") {
    TempDir tmp("thumb_invalidate_mtime");
    auto abs1 = tmp.path / "alpha.glb";
    auto abs2 = tmp.path / "beta.glb";
    writeFakePng(tmp.path / "alpha.glb.thumb.png");
    writeFakePng(tmp.path / "beta.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("alpha.glb", abs1.generic_string(), 100);
    c.markBaked("beta.glb",  abs2.generic_string(), 200);
    REQUIRE(c.size() == 2);

    // beta's mtime advances — it must be dropped. alpha remains.
    std::unordered_map<std::string, int64_t> current{
        {"alpha.glb", 100},
        {"beta.glb",  999},  // changed
    };
    size_t dropped = c.invalidateStale(current);
    REQUIRE(dropped == 1);
    REQUIRE(c.contains("alpha.glb"));
    REQUIRE_FALSE(c.contains("beta.glb"));
    // beta's sibling file must have been deleted on disk.
    REQUIRE_FALSE(std::filesystem::exists(tmp.path / "beta.glb.thumb.png"));
    // alpha's sibling file is untouched.
    REQUIRE(std::filesystem::exists(tmp.path / "alpha.glb.thumb.png"));
}

TEST_CASE("ThumbnailCache: invalidateStale drops entries removed from asset list",
          "[thumbnail-cache][invalidate]") {
    TempDir tmp("thumb_invalidate_removed");
    auto absA = tmp.path / "a.glb";
    auto absB = tmp.path / "b.glb";
    writeFakePng(tmp.path / "a.glb.thumb.png");
    writeFakePng(tmp.path / "b.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("a.glb", absA.generic_string(), 1);
    c.markBaked("b.glb", absB.generic_string(), 1);
    REQUIRE(c.size() == 2);

    // Only "a.glb" is present in the current list — "b.glb" is gone.
    std::vector<AssetBrowserEntry> current{
        makeEntry("a.glb", absA.generic_string(), 1),
    };
    size_t dropped = c.invalidateStale(current);
    REQUIRE(dropped == 1);
    REQUIRE(c.contains("a.glb"));
    REQUIRE_FALSE(c.contains("b.glb"));
    REQUIRE_FALSE(std::filesystem::exists(tmp.path / "b.glb.thumb.png"));
}

TEST_CASE("ThumbnailCache: invalidateStale returns 0 when nothing changed",
          "[thumbnail-cache][invalidate]") {
    TempDir tmp("thumb_invalidate_noop");
    auto abs = tmp.path / "stable.glb";
    writeFakePng(tmp.path / "stable.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("stable.glb", abs.generic_string(), 555);

    std::vector<AssetBrowserEntry> current{
        makeEntry("stable.glb", abs.generic_string(), 555),
    };
    REQUIRE(c.invalidateStale(current) == 0);
    REQUIRE(c.contains("stable.glb"));
    REQUIRE(std::filesystem::exists(tmp.path / "stable.glb.thumb.png"));
}

// ── clear ─────────────────────────────────────────────────────────

TEST_CASE("ThumbnailCache: clear wipes memory and deletes sibling files",
          "[thumbnail-cache][clear]") {
    TempDir tmp("thumb_clear");
    auto abs1 = tmp.path / "x.glb";
    auto abs2 = tmp.path / "y.glb";
    auto thumb1 = tmp.path / "x.glb.thumb.png";
    auto thumb2 = tmp.path / "y.glb.thumb.png";
    writeFakePng(thumb1);
    writeFakePng(thumb2);

    ThumbnailCache c;
    c.markBaked("x.glb", abs1.generic_string(), 10);
    c.markBaked("y.glb", abs2.generic_string(), 20);
    REQUIRE(c.size() == 2);

    c.clear();

    REQUIRE(c.empty());
    REQUIRE(c.lruOrder().empty());
    REQUIRE_FALSE(std::filesystem::exists(thumb1));
    REQUIRE_FALSE(std::filesystem::exists(thumb2));
}

// ── LRU + byte-budget eviction ─────────────────────────

TEST_CASE("ThumbnailCache: markBaked populates LRU order (MRU first)",
          "[thumbnail-cache][lru]") {
    TempDir tmp("thumb_lru_order");
    auto absA = tmp.path / "a.glb";
    auto absB = tmp.path / "b.glb";
    auto absC = tmp.path / "c.glb";
    writeFakePng(tmp.path / "a.glb.thumb.png");
    writeFakePng(tmp.path / "b.glb.thumb.png");
    writeFakePng(tmp.path / "c.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("a.glb", absA.generic_string(), 1, 100);
    c.markBaked("b.glb", absB.generic_string(), 1, 100);
    c.markBaked("c.glb", absC.generic_string(), 1, 100);

    // Most recent at front, oldest at back.
    REQUIRE(c.lruOrder().size() == 3);
    REQUIRE(c.lruOrder().front() == "c.glb");
    REQUIRE(c.lruOrder().back()  == "a.glb");

    // touch() moves an entry to the front.
    c.touch("a.glb");
    REQUIRE(c.lruOrder().front() == "a.glb");
    REQUIRE(c.lruOrder().back()  == "b.glb");

    // touch() on a missing entry is a no-op.
    c.touch("not-present");
    REQUIRE(c.lruOrder().front() == "a.glb");

    // Re-marking an existing key moves it to the front (and keeps
    // the list length constant).
    c.markBaked("b.glb", absB.generic_string(), 2, 200);
    REQUIRE(c.lruOrder().size() == 3);
    REQUIRE(c.lruOrder().front() == "b.glb");
}

TEST_CASE("ThumbnailCache: currentBytes sums Entry::byteSize",
          "[thumbnail-cache][lru]") {
    TempDir tmp("thumb_currentBytes");
    writeFakePng(tmp.path / "one.glb.thumb.png");
    writeFakePng(tmp.path / "two.glb.thumb.png");

    ThumbnailCache c;
    REQUIRE(c.currentBytes() == 0);
    REQUIRE(c.budgetBytes()  == 0); // default: unbounded

    c.markBaked("one.glb", (tmp.path / "one.glb").generic_string(), 1, 1024);
    REQUIRE(c.currentBytes() == 1024);

    c.markBaked("two.glb", (tmp.path / "two.glb").generic_string(), 1, 4096);
    REQUIRE(c.currentBytes() == 5120);

    // Eviction keeps the accounting accurate.
    REQUIRE(c.evict("one.glb", /*deleteFile=*/true));
    REQUIRE(c.currentBytes() == 4096);
}

TEST_CASE("ThumbnailCache: setBudgetBytes evicts LRU entries until under budget",
          "[thumbnail-cache][lru]") {
    TempDir tmp("thumb_budget");
    auto absA = tmp.path / "a.glb";
    auto absB = tmp.path / "b.glb";
    auto absC = tmp.path / "c.glb";
    auto thumbA = tmp.path / "a.glb.thumb.png";
    auto thumbB = tmp.path / "b.glb.thumb.png";
    auto thumbC = tmp.path / "c.glb.thumb.png";
    writeFakePng(thumbA);
    writeFakePng(thumbB);
    writeFakePng(thumbC);

    ThumbnailCache c;
    c.markBaked("a.glb", absA.generic_string(), 1, 1000);
    c.markBaked("b.glb", absB.generic_string(), 1, 1000);
    c.markBaked("c.glb", absC.generic_string(), 1, 1000);
    REQUIRE(c.currentBytes() == 3000);

    // Set a budget of 1500 — expect the two oldest (a, b) to be dropped.
    c.setBudgetBytes(1500);
    REQUIRE(c.budgetBytes() == 1500);
    REQUIRE(c.currentBytes() <= 1500);
    REQUIRE(c.contains("c.glb"));
    REQUIRE_FALSE(c.contains("a.glb"));
    REQUIRE_FALSE(c.contains("b.glb"));

    // Evicted sibling files must be deleted on disk.
    REQUIRE_FALSE(std::filesystem::exists(thumbA));
    REQUIRE_FALSE(std::filesystem::exists(thumbB));
    REQUIRE(std::filesystem::exists(thumbC));
}

TEST_CASE("ThumbnailCache: budget respects touch() order",
          "[thumbnail-cache][lru]") {
    TempDir tmp("thumb_budget_touch");
    writeFakePng(tmp.path / "old.glb.thumb.png");
    writeFakePng(tmp.path / "mid.glb.thumb.png");
    writeFakePng(tmp.path / "new.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("old.glb", (tmp.path / "old.glb").generic_string(), 1, 500);
    c.markBaked("mid.glb", (tmp.path / "mid.glb").generic_string(), 1, 500);
    c.markBaked("new.glb", (tmp.path / "new.glb").generic_string(), 1, 500);

    // Touch the nominally-oldest entry so the LRU back becomes "mid.glb"
    // instead of "old.glb".
    c.touch("old.glb");

    // Budget for 1 entry — two must be dropped. "mid.glb" should go
    // first because it became the LRU after the touch; "new.glb"
    // goes next to fit under 500 bytes.
    c.setBudgetBytes(500);

    REQUIRE(c.size() == 1);
    REQUIRE(c.contains("old.glb"));
    REQUIRE_FALSE(c.contains("mid.glb"));
    REQUIRE_FALSE(c.contains("new.glb"));
}

TEST_CASE("ThumbnailCache: evictLRU shrinks below explicit target",
          "[thumbnail-cache][lru]") {
    TempDir tmp("thumb_evictLRU");
    writeFakePng(tmp.path / "p.glb.thumb.png");
    writeFakePng(tmp.path / "q.glb.thumb.png");
    writeFakePng(tmp.path / "r.glb.thumb.png");
    writeFakePng(tmp.path / "s.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("p.glb", (tmp.path / "p.glb").generic_string(), 1, 250);
    c.markBaked("q.glb", (tmp.path / "q.glb").generic_string(), 1, 250);
    c.markBaked("r.glb", (tmp.path / "r.glb").generic_string(), 1, 250);
    c.markBaked("s.glb", (tmp.path / "s.glb").generic_string(), 1, 250);

    // No budget yet — eviction only happens on explicit call.
    REQUIRE(c.currentBytes() == 1000);
    REQUIRE(c.budgetBytes()  == 0);

    // evictLRU returns 0 when already under the target.
    REQUIRE(c.evictLRU(2000) == 0);
    REQUIRE(c.size() == 4);

    // Evict down to 500 bytes — expect exactly two drops (p and q,
    // the two oldest). 4 * 250 = 1000; after dropping 2 we're at
    // 500 which equals the target, so the loop stops.
    size_t dropped = c.evictLRU(500);
    REQUIRE(dropped == 2);
    REQUIRE(c.currentBytes() == 500);
    REQUIRE_FALSE(c.contains("p.glb"));
    REQUIRE_FALSE(c.contains("q.glb"));
    REQUIRE(c.contains("r.glb"));
    REQUIRE(c.contains("s.glb"));
}

TEST_CASE("ThumbnailCache: setBudgetBytes(0) is unbounded and never evicts",
          "[thumbnail-cache][lru]") {
    TempDir tmp("thumb_unbounded");
    writeFakePng(tmp.path / "big.glb.thumb.png");

    ThumbnailCache c;
    c.markBaked("big.glb", (tmp.path / "big.glb").generic_string(),
                1, /*byteSize=*/1'000'000);

    // Explicit zero — no eviction, cache stays intact.
    c.setBudgetBytes(0);
    REQUIRE(c.budgetBytes()  == 0);
    REQUIRE(c.currentBytes() == 1'000'000);
    REQUIRE(c.contains("big.glb"));
}
