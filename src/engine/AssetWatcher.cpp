// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "AssetWatcher.h"
#include "EngineLog.h"

#include <filesystem>
#include <algorithm>
#include <cstdio>

namespace sv {

namespace {

// ASCII-lowercase copy (no locale dependency).
std::string lowerAscii(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') u = static_cast<unsigned char>(u + ('a' - 'A'));
        out.push_back(static_cast<char>(u));
    }
    return out;
}

// Case-insensitive "does `ext` appear in `wanted`" test. `wanted` entries
// are expected to be lowercase already; the input extension is normalized.
bool extMatches(const std::string& ext, const std::vector<std::string>& wanted)
{
    std::string extLow = lowerAscii(ext);
    return std::any_of(wanted.begin(), wanted.end(),
                       [&](const std::string& e) { return e == extLow; });
}

} // anonymous

void AssetWatcher::watch(const std::string& path, std::function<void()> callback)
{
    WatchEntry entry;
    entry.path = path;
    entry.callback = std::move(callback);
    entry.lastModified = getFileTimestamp(path);
    m_entries.push_back(std::move(entry));
}

bool AssetWatcher::checkAll(int throttleInterval)
{
    m_counter++;
    if (m_counter < throttleInterval) return false;
    m_counter = 0;
    return forceCheck();
}

bool AssetWatcher::forceCheck()
{
    bool anyChanged = false;
    for (auto& entry : m_entries) {
        int64_t ts = getFileTimestamp(entry.path);
        if (ts != entry.lastModified && ts != 0) {
            entry.lastModified = ts;
            anyChanged = true;
            if (entry.callback) {
                entry.callback();
            }
        }
    }
    return anyChanged;
}

size_t AssetWatcher::watchDirectory(const std::string& dir,
                                    const std::vector<std::string>& extensions,
                                    std::function<void()> callback)
{
    std::error_code ec;
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        if (extMatches(ext, extensions)) {
            watch(entry.path().string(), callback);
            count++;
        }
    }
    if (ec) {
        SV_LOG_WARN("AssetWatcher", "Could not scan directory: %s (%s)",
                    dir.c_str(), ec.message().c_str());
    } else if (count > 0) {
        SV_LOG_INFO("AssetWatcher", "Watching %zu files in %s", count, dir.c_str());
    }
    return count;
}

size_t AssetWatcher::watchDirectoryRecursive(const std::string& dir,
                                             const std::vector<std::string>& extensions,
                                             std::function<void()> callback)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    size_t count = 0;

    fs::recursive_directory_iterator it(
        dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        SV_LOG_WARN("AssetWatcher",
                    "Could not open recursive iterator for %s (%s)",
                    dir.c_str(), ec.message().c_str());
        return 0;
    }

    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            SV_LOG_WARN("AssetWatcher",
                        "Recursive walk error under %s: %s",
                        dir.c_str(), ec.message().c_str());
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(ec)) { ec.clear(); continue; }
        std::string ext = entry.path().extension().string();
        if (extMatches(ext, extensions)) {
            watch(entry.path().string(), callback);
            ++count;
        }
    }

    if (count > 0) {
        SV_LOG_INFO("AssetWatcher",
                    "Watching %zu file(s) recursively under %s",
                    count, dir.c_str());
    }
    return count;
}

void AssetWatcher::clear()
{
    m_entries.clear();
    m_counter = 0;
}

int64_t AssetWatcher::getFileTimestamp(const std::string& path)
{
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return ftime.time_since_epoch().count();
}

} // namespace sv
