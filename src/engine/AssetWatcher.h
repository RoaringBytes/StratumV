// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace sv {

class AssetWatcher {
public:
    // Register a file to watch. Callback fires when the file changes.
    void watch(const std::string& path, std::function<void()> callback);

    // Watch all files matching extensions in a directory (non-recursive).
    // extensions: e.g. {".vert", ".frag", ".comp", ".glsl"}
    // Extension matching is ASCII case-insensitive (".GLB" matches ".glb").
    // Returns number of files registered.
    size_t watchDirectory(const std::string& dir,
                          const std::vector<std::string>& extensions,
                          std::function<void()> callback);

    // Recursive variant: walks `dir` and all subdirectories,
    // registering every file whose extension matches one in `extensions`.
    // Extension matching is ASCII case-insensitive. Returns number of
    // files registered. New files added to the tree after this call are
    // NOT picked up until watchDirectoryRecursive() is called again —
    // AssetBrowser::attachWatcher() re-registers on every rescan() to
    // handle that. Permission-denied subdirectories are skipped.
    size_t watchDirectoryRecursive(const std::string& dir,
                                   const std::vector<std::string>& extensions,
                                   std::function<void()> callback);

    // Check all watched files for changes. Throttled to every N calls.
    // Returns true if any file changed.
    bool checkAll(int throttleInterval = 60);

    // Force check all files regardless of throttle
    bool forceCheck();

    // Remove all watches
    void clear();

private:
    struct WatchEntry {
        std::string             path;
        std::function<void()>   callback;
        int64_t                 lastModified = 0;
    };

    std::vector<WatchEntry> m_entries;
    int                     m_counter = 0;

    static int64_t getFileTimestamp(const std::string& path);
};

} // namespace sv
