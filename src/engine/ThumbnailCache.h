// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── ThumbnailCache ─────────────────────────────────────────────────
// Disk-backed sibling-file cache for asset thumbnails. Each cached
// thumbnail lives on disk as "<absolutePath>.thumb.png" next to the
// source asset (matching the `<filename>.meta.json` sidecar
// convention). The in-memory index records the source mtime at bake
// time, so callers can detect when a thumbnail has become stale
// because the underlying asset was edited.
//
// Design notes:
//   - Pure logic. No Vulkan, no ImGui, no image encoding. The GPU
//     bake lives in consumer code (lab/skinned_test). This module
//     only manages cache state + sibling path composition +
//     filesystem eviction.
//   - Cache key = relativePath (matches AssetBrowserEntry::relativePath).
//     The Entry stores the source mtime + absolute paths + byte size.
//   - Invalidation: isValid() rejects entries whose stored mtime no
//     longer matches the live source mtime; invalidateStale() walks
//     the cache against an AssetBrowser snapshot and drops every
//     entry that either has advanced mtime or has been deleted from
//     the browser.
//   - On-disk eviction is opt-out via the `deleteFile` flag on
//     evict(). invalidateStale()/clear() always delete sibling files
//     so stale thumbnails never linger.
//   - LRU + byte-budget eviction lives on top of the invalidation
//     primitives. `setBudgetBytes(N)` caps total
//     `Entry::byteSize` and drops least-recently-used entries until
//     the sum fits. `touch(relPath)` bumps an entry to the front of
//     the LRU list so consumers can mark "this thumbnail was just
//     shown in the UI". Default budget is zero, meaning "unbounded".
//
// Layer 4 — depends on: std::filesystem, std::unordered_map, std::list.
// Uses AssetBrowserEntry only as a forward-declared type for the
// invalidateStale() overload, so AssetBrowser.h is not pulled into
// every translation unit that touches the cache.

#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sv {

struct AssetBrowserEntry;  // forward decl — see AssetBrowser.h

// ── Sibling path composition ───────────────────────────────────────
// Returns "<absolutePath>.thumb.png". Does NOT touch the filesystem.
// Empty input returns empty string.
std::string thumbnailPathFor(std::string_view absolutePath);

// True if the filename ends with ".thumb.png" (ASCII case-insensitive).
// Used by AssetBrowser::isIgnoredFilename so baked thumbnails never
// show up in the browser entry list.
bool isThumbnailSibling(std::string_view filename);

// ── ThumbnailCache ─────────────────────────────────────────────────
class ThumbnailCache {
public:
    struct Entry {
        std::string relativePath;          // key (forward slashes)
        std::string absolutePath;          // absolute source path
        std::string thumbnailPath;         // absolute sibling .thumb.png path
        int64_t     sourceLastModified = 0;// mtime at time of bake
        uint64_t    byteSize           = 0;// PNG bytes written (0 if unknown)
    };

    ThumbnailCache() = default;
    ~ThumbnailCache() = default;

    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // ── Registration ──
    // Record a freshly-baked thumbnail. Stores the source mtime so
    // later isValid() calls can detect source mutations. Overwrites
    // any existing entry for relativePath. Caller is responsible for
    // having written the PNG to disk BEFORE calling markBaked().
    void markBaked(const std::string& relativePath,
                   const std::string& absolutePath,
                   int64_t             sourceLastModified,
                   uint64_t            byteSize = 0);

    // ── Lookup ──
    // True if all three hold:
    //   1. An entry exists for relativePath
    //   2. The entry's stored mtime equals currentMtime
    //   3. The sibling .thumb.png file exists on disk
    bool isValid(const std::string& relativePath, int64_t currentMtime) const;

    // Lookup without mtime / filesystem checks. Returns nullptr if
    // the entry is missing.
    const Entry* find(const std::string& relativePath) const;

    // True iff an entry exists for relativePath (does not check mtime
    // or disk presence).
    bool contains(const std::string& relativePath) const;

    // ── Eviction ──
    // Drop the entry from memory. If `deleteFile` is true, also
    // deletes the sibling .thumb.png on disk (best-effort; errors
    // are logged but still return true if the in-memory entry was
    // removed). Returns false if no entry existed for relativePath.
    bool evict(const std::string& relativePath, bool deleteFile = true);

    // Walk current asset browser entries and drop cache entries whose:
    //   - relativePath is no longer present in `current`
    //   - stored mtime != current entry's lastModified
    // Always deletes sibling files for dropped entries.
    // Returns number of cache entries removed.
    size_t invalidateStale(const std::vector<AssetBrowserEntry>& current);

    // Same policy as above, but driven by an explicit relPath→mtime
    // map. Useful for tests or tools that don't own an AssetBrowser.
    size_t invalidateStale(const std::unordered_map<std::string, int64_t>& currentMtimes);

    // Drop every entry and delete every sibling .thumb.png on disk.
    // Leaves the cache empty.
    void clear();

    // ── LRU + byte-budget eviction ──
    // Sets the maximum total byteSize the cache will tolerate. 0 =
    // unbounded (default). Setting a non-zero budget immediately
    // evicts least-recently-used entries until `currentBytes() <=
    // budget`. Sibling PNGs on disk are deleted for every evicted
    // entry.
    void     setBudgetBytes(uint64_t budget);
    uint64_t budgetBytes()  const { return m_budgetBytes; }

    // Current sum of all stored Entry::byteSize values. Walks the
    // internal map, so O(N) in cache size. Cheap for typical
    // thumbnail cache sizes (<1K entries).
    uint64_t currentBytes() const;

    // Explicitly evict LRU entries until `currentBytes() <=
    // targetBytes`. Returns the number of entries dropped. Useful
    // for callers that want to shrink the cache below the static
    // budget (e.g. on low-memory pressure).
    size_t   evictLRU(uint64_t targetBytes);

    // Bump an entry to the most-recently-used end of the LRU list.
    // No-op if the entry does not exist. Use after a UI tile is
    // shown, a tooltip pops the thumbnail, etc.
    void     touch(const std::string& relativePath);

    // ── Accessors ──
    const std::unordered_map<std::string, Entry>& entries() const { return m_entries; }
    size_t size()  const { return m_entries.size(); }
    bool   empty() const { return m_entries.empty(); }

    // Read-only access to the LRU list for tests/diagnostics.
    // Front = most recently used. Back = eviction target.
    const std::list<std::string>& lruOrder() const { return m_lru; }

private:
    // Internal: drop a single entry by key, updating both the map
    // and the LRU list. Optionally deletes the sibling PNG from
    // disk. Returns true if an entry was removed.
    bool eraseEntry(const std::string& relativePath, bool deleteFile);

    // Internal: move an existing entry's LRU node to the front.
    // Precondition: the entry exists in m_entries.
    void moveToFront(const std::string& relativePath);

    std::unordered_map<std::string, Entry> m_entries;

    // ── LRU shadow structures ──
    // m_lru is an ordered list of relativePaths, front=MRU, back=LRU.
    // m_lruIters maps relativePath → its iterator in m_lru so that
    // touch() / markBaked() can relocate nodes in O(1).
    std::list<std::string>                                       m_lru;
    std::unordered_map<std::string, std::list<std::string>::iterator> m_lruIters;

    uint64_t m_budgetBytes = 0; // 0 = unbounded
};

} // namespace sv
