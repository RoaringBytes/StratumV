// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── AssetBrowser ─────────────────────────────────────────
// DCC-agnostic asset browser that discovers files under a project
// assets directory, categorizes them by extension, and exposes them
// via a lookup/filter API. Consumed by the AdminPanel "Assets" tab
// (ImGui surface lives in src/engine/ui/AdminPanel.cpp so this
// translation unit has no ImGui dependency — tests can link it
// without pulling in ImGui symbols).
//
// Ingestion flow:
//   1. Engine calls AssetBrowser::scan(rootDir) at startup.
//   2. Files are categorized via kindFromFilename() (extension routing
//      + compound-suffix handling for .scene.json / .meta.json).
//   3. Games can request an AssetManifest snapshot via
//      populateManifest() — useful for tooling that wants to treat
//      discovered files as preload candidates without editing a JSON
//      manifest by hand.
//   4. Optional: attachWatcher() registers with an AssetWatcher so
//      filesystem changes trigger rescan().
//
// Design notes:
//   - Pure logic. No Vulkan, no ImGui, no game code.
//   - Paths are normalized to forward slashes.
//   - Recursive directory walk, skipping isIgnoredFilename().
//   - ImportSettings are cached in-memory per scan root.
//   - Optional `<file>.meta.json` sidecar persistence —
//     loadMetaFile/saveMetaFile plus auto-load-on-scan and
//     opt-in auto-save-on-edit. Sidecars use the Unity-style
//     "<filename>.meta.json" convention (e.g. "player.glb.meta.json")
//     so two assets with the same stem but different extensions don't
//     clobber each other.
//
// Layer 4 — depends on: std::filesystem, nlohmann::json, AssetManifest
// (for AssetKind + forward decl), AssetWatcher (forward decl).

#include "AssetManifest.h"   // AssetKind, AssetManifest (for populateManifest)

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sv {

class AssetWatcher;

// ── AssetBrowserEntry ──────────────────────────────────────────────
// One discovered file on disk. `relativePath` is the canonical key
// into the browser — it is unique within a scan root.
struct AssetBrowserEntry {
    std::string name;          // filename only, e.g. "player.glb"
    std::string relativePath;  // path relative to scan root, forward slashes
    std::string absolutePath;  // absolute path on disk, forward slashes
    std::string extension;     // lowercase, includes the dot (".glb"). Empty if none.
    AssetKind   kind = AssetKind::Other;
    uint64_t    sizeBytes    = 0;
    int64_t     lastModified = 0;  // filesystem last_write_time raw ticks
};

// ── ImportSettings ─────────────────────────────────────────────────
// Per-asset import hints. Stable v1 JSON schema:
//   {
//     "version": 1,
//     "scale": 1.0,
//     "upAxis": "Y",               // "Y" or "Z"
//     "materialMapping": "pbr",
//     "preload": true
//   }
struct ImportSettings {
    enum class UpAxis : uint8_t { Y = 0, Z = 1 };

    float       scale           = 1.0f;
    UpAxis      upAxis          = UpAxis::Y;
    std::string materialMapping;
    bool        preload         = true;

    nlohmann::json toJson() const;
    static ImportSettings fromJson(const nlohmann::json& doc);
};

// ── Filename → AssetKind routing ───────────────────────────────────
// Case-insensitive. Handles compound suffixes (.scene.json,
// .meta.json, .thumb.png). Unknown extensions map to AssetKind::Other.
// .meta.json and .thumb.png both return AssetKind::Other and should
// be paired with isIgnoredFilename() to skip the browser entirely.
AssetKind kindFromFilename(std::string_view filename);

// True if the browser should skip this filename. Covers
// sidecar metadata (.meta.json), baked thumbnails
// (.thumb.png), dotfiles, tmp/backup files. Case-insensitive.
bool isIgnoredFilename(std::string_view filename);

// ── AssetBrowser ───────────────────────────────────────────────────
class AssetBrowser {
public:
    AssetBrowser();
    ~AssetBrowser();

    AssetBrowser(const AssetBrowser&) = delete;
    AssetBrowser& operator=(const AssetBrowser&) = delete;

    // Scan a directory recursively. Replaces current entries.
    // Returns false if the directory is missing or unreadable.
    bool scan(const std::string& rootDir);

    // Re-scan the last successful scan root. No-op (returns false) if
    // scan() was never called.
    bool rescan();

    // Clear all entries, cached import settings, and scan root.
    void clear();

    // ── Accessors ──
    const std::vector<AssetBrowserEntry>& entries() const { return m_entries; }
    const std::string&                    rootDir() const { return m_rootDir; }
    size_t                                size()    const { return m_entries.size(); }
    bool                                  empty()   const { return m_entries.empty(); }

    // Entries filtered by kind (stable pointers into m_entries).
    std::vector<const AssetBrowserEntry*> entriesOfKind(AssetKind k) const;

    // Case-insensitive substring search over name + relativePath.
    // Empty needle returns pointers to every entry.
    std::vector<const AssetBrowserEntry*> filter(std::string_view needle) const;

    // Lookup by forward-slash relativePath. Returns nullptr if missing.
    const AssetBrowserEntry* findByRelativePath(const std::string& relativePath) const;

    // ── Import settings cache + sidecar persistence ──
    ImportSettings getImportSettings(const std::string& relativePath) const;
    void           setImportSettings(const std::string& relativePath,
                                     ImportSettings     settings);
    bool           hasImportSettings(const std::string& relativePath) const;

    // Sidecar path convention: "<absolutePath>.meta.json".
    // Returns the empty string if relativePath is unknown.
    std::string metaFilePathFor(const std::string& relativePath) const;

    // Load "<file>.meta.json" from disk into the cache for this asset.
    // Overwrites any existing cache entry on success. Returns false if
    // relativePath is unknown, the sidecar is missing, or the JSON is
    // invalid. Safe to call multiple times.
    bool loadMetaFile(const std::string& relativePath);

    // Save the current (or default) import settings for relativePath to
    // "<file>.meta.json". Returns false if relativePath is unknown or the
    // write fails. If there is no cache entry, a default ImportSettings
    // is written.
    bool saveMetaFile(const std::string& relativePath) const;

    // Auto-load every "<file>.meta.json" sibling during scan(). Entries
    // already present in the cache are NOT overwritten (cache-wins for
    // uncommitted edits). Default: ON.
    void setAutoLoadMeta(bool enabled) { m_autoLoadMeta = enabled; }
    bool autoLoadMeta() const { return m_autoLoadMeta; }

    // Auto-write "<file>.meta.json" to disk whenever setImportSettings
    // mutates the cache. Default: OFF — opt-in for tools that want
    // write-through persistence. When OFF, callers must invoke
    // saveMetaFile() explicitly.
    void setAutoSaveMeta(bool enabled) { m_autoSaveMeta = enabled; }
    bool autoSaveMeta() const { return m_autoSaveMeta; }

    // ── AssetManifest bridge ──
    // Rebuild an AssetManifest from the current scan results.
    // Manifest entry name = filename stem; when two files share a stem
    // the second and later get their parent directory appended to
    // disambiguate ("character" + "character_anim" etc.).
    void populateManifest(AssetManifest&     out,
                          const std::string& manifestName = "asset_browser") const;

    // ── Hot-reload ──
    // Register a rescan callback with the supplied watcher. Does not
    // take ownership. Safe to call multiple times — replaces any
    // previous attachment.
    void attachWatcher(AssetWatcher* watcher);

private:
    // Helper: push one discovered path into m_entries.
    void ingestFile(const std::string& absPath,
                    const std::string& relPath,
                    uint64_t           sizeBytes,
                    int64_t            lastModified);

    std::string                                        m_rootDir;
    std::vector<AssetBrowserEntry>                     m_entries;
    std::unordered_map<std::string, ImportSettings>    m_importCache;

    // Sidecar persistence toggles.
    bool                                               m_autoLoadMeta = true;
    bool                                               m_autoSaveMeta = false;
};

} // namespace sv
