// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── ThumbnailCache ─────────────────────────────────────────────────
// See ThumbnailCache.h for overview.

#include "ThumbnailCache.h"
#include "AssetBrowser.h"   // AssetBrowserEntry for invalidateStale overload
#include "EngineLog.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace sv {

static constexpr const char* TAG = "ThumbnailCache";

namespace fs = std::filesystem;

// ── Internal helpers ───────────────────────────────────────────────

namespace {

// ASCII-lowercase copy (no locale dependency). Mirrors the helper in
// AssetBrowser.cpp — kept private here so there is no inter-module
// dependency on that helper's linkage.
std::string toLowerAscii(std::string_view s)
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

// True if `hay` ends with `needle` (ASCII case-insensitive).
bool endsWithICase(std::string_view hay, std::string_view needle)
{
    if (needle.size() > hay.size()) return false;
    std::string hayLow    = toLowerAscii(hay.substr(hay.size() - needle.size()));
    std::string needleLow = toLowerAscii(needle);
    return hayLow == needleLow;
}

} // anonymous

// ── Free-function sibling path helpers ─────────────────────────────

std::string thumbnailPathFor(std::string_view absolutePath)
{
    if (absolutePath.empty()) return {};
    std::string out(absolutePath);
    out += ".thumb.png";
    return out;
}

bool isThumbnailSibling(std::string_view filename)
{
    return endsWithICase(filename, ".thumb.png");
}

// ── ThumbnailCache ─────────────────────────────────────────────────

void ThumbnailCache::markBaked(const std::string& relativePath,
                               const std::string& absolutePath,
                               int64_t             sourceLastModified,
                               uint64_t            byteSize)
{
    Entry e;
    e.relativePath       = relativePath;
    e.absolutePath       = absolutePath;
    e.thumbnailPath      = thumbnailPathFor(absolutePath);
    e.sourceLastModified = sourceLastModified;
    e.byteSize           = byteSize;
    m_entries[relativePath] = std::move(e);

    // Update LRU shadow. If the entry was re-baked
    // (overwrite), drop the old LRU node before pushing a new one
    // so there is at most one copy in the list.
    auto itLru = m_lruIters.find(relativePath);
    if (itLru != m_lruIters.end()) {
        m_lru.erase(itLru->second);
        m_lruIters.erase(itLru);
    }
    m_lru.push_front(relativePath);
    m_lruIters[relativePath] = m_lru.begin();

    // Enforce byte budget if one was set.
    if (m_budgetBytes > 0) {
        evictLRU(m_budgetBytes);
    }
}

bool ThumbnailCache::isValid(const std::string& relativePath,
                             int64_t             currentMtime) const
{
    auto it = m_entries.find(relativePath);
    if (it == m_entries.end()) return false;
    if (it->second.sourceLastModified != currentMtime) return false;

    std::error_code ec;
    if (!fs::exists(it->second.thumbnailPath, ec))      return false;
    if (!fs::is_regular_file(it->second.thumbnailPath, ec)) return false;
    return true;
}

const ThumbnailCache::Entry*
ThumbnailCache::find(const std::string& relativePath) const
{
    auto it = m_entries.find(relativePath);
    return (it == m_entries.end()) ? nullptr : &it->second;
}

bool ThumbnailCache::contains(const std::string& relativePath) const
{
    return m_entries.find(relativePath) != m_entries.end();
}

bool ThumbnailCache::evict(const std::string& relativePath, bool deleteFile)
{
    return eraseEntry(relativePath, deleteFile);
}

// Internal: single source of truth for removing an entry. Updates
// the map, deletes the sibling file if asked, and removes the
// LRU shadow node. Returns true if an entry was present.
bool ThumbnailCache::eraseEntry(const std::string& relativePath, bool deleteFile)
{
    auto it = m_entries.find(relativePath);
    if (it == m_entries.end()) return false;

    if (deleteFile) {
        std::error_code ec;
        const std::string& p = it->second.thumbnailPath;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            fs::remove(p, ec);
            if (ec) {
                SV_LOG_WARN(TAG,
                            "evict: failed to remove %s (%s)",
                            p.c_str(), ec.message().c_str());
                // Fall through — still remove the in-memory entry.
            }
        }
    }

    m_entries.erase(it);

    // Drop the LRU shadow node.
    auto itLru = m_lruIters.find(relativePath);
    if (itLru != m_lruIters.end()) {
        m_lru.erase(itLru->second);
        m_lruIters.erase(itLru);
    }

    return true;
}

void ThumbnailCache::moveToFront(const std::string& relativePath)
{
    auto itLru = m_lruIters.find(relativePath);
    if (itLru == m_lruIters.end()) return;
    if (itLru->second == m_lru.begin()) return; // already MRU
    m_lru.splice(m_lru.begin(), m_lru, itLru->second);
    // splice(begin, list, iter) does not invalidate the iterator,
    // so m_lruIters stays valid. No re-insert needed.
}

size_t ThumbnailCache::invalidateStale(
    const std::vector<AssetBrowserEntry>& current)
{
    // Build a mtime lookup keyed by relativePath.
    std::unordered_map<std::string, int64_t> mtimes;
    mtimes.reserve(current.size());
    for (const auto& e : current) {
        mtimes[e.relativePath] = e.lastModified;
    }
    return invalidateStale(mtimes);
}

size_t ThumbnailCache::invalidateStale(
    const std::unordered_map<std::string, int64_t>& currentMtimes)
{
    // Collect keys to drop first — we can't mutate the cache while
    // iterating it.
    std::vector<std::string> toDrop;
    toDrop.reserve(m_entries.size());

    for (const auto& kv : m_entries) {
        auto it = currentMtimes.find(kv.first);
        if (it == currentMtimes.end()) {
            // Source asset is no longer in the browser.
            toDrop.push_back(kv.first);
            continue;
        }
        if (it->second != kv.second.sourceLastModified) {
            // Source mtime has advanced (or regressed — either way,
            // the baked thumbnail is stale).
            toDrop.push_back(kv.first);
        }
    }

    for (const auto& key : toDrop) {
        evict(key, /*deleteFile=*/true);
    }

    if (!toDrop.empty()) {
        SV_LOG_INFO(TAG, "invalidateStale: dropped %zu entries",
                    toDrop.size());
    }
    return toDrop.size();
}

void ThumbnailCache::clear()
{
    // Delete every sibling file before wiping the in-memory map.
    std::error_code ec;
    for (const auto& kv : m_entries) {
        const std::string& p = kv.second.thumbnailPath;
        if (!p.empty() && fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            fs::remove(p, ec);
            if (ec) {
                SV_LOG_WARN(TAG,
                            "clear: failed to remove %s (%s)",
                            p.c_str(), ec.message().c_str());
                ec.clear();
            }
        }
    }
    m_entries.clear();

    // Wipe LRU shadow too.
    m_lru.clear();
    m_lruIters.clear();
}

// ── LRU + byte-budget eviction ─────────────────────────────────────

uint64_t ThumbnailCache::currentBytes() const
{
    uint64_t total = 0;
    for (const auto& kv : m_entries) {
        total += kv.second.byteSize;
    }
    return total;
}

void ThumbnailCache::setBudgetBytes(uint64_t budget)
{
    m_budgetBytes = budget;
    // Zero = unbounded; nothing to enforce.
    if (budget == 0) return;
    evictLRU(budget);
}

size_t ThumbnailCache::evictLRU(uint64_t targetBytes)
{
    uint64_t total = currentBytes();
    if (total <= targetBytes) return 0;

    size_t dropped = 0;
    // Walk the LRU list from the back (least recently used) and
    // drop entries until we fit under the target. eraseEntry()
    // mutates m_lru from under us, so keep re-fetching back().
    while (total > targetBytes && !m_lru.empty()) {
        const std::string key = m_lru.back();
        auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            // Defensive: LRU list has a stale entry. Drop the
            // orphaned node and keep looking.
            m_lru.pop_back();
            m_lruIters.erase(key);
            continue;
        }
        const uint64_t entryBytes = it->second.byteSize;
        eraseEntry(key, /*deleteFile=*/true);
        if (entryBytes >= total) {
            total = 0;
        } else {
            total -= entryBytes;
        }
        ++dropped;
    }

    if (dropped > 0) {
        SV_LOG_INFO(TAG,
                    "evictLRU: dropped %zu entries to fit under %llu bytes",
                    dropped, (unsigned long long)targetBytes);
    }
    return dropped;
}

void ThumbnailCache::touch(const std::string& relativePath)
{
    if (m_entries.find(relativePath) == m_entries.end()) return;
    moveToFront(relativePath);
}

} // namespace sv
