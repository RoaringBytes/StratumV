// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetBrowser ─────────────────────────────────────────
// See AssetBrowser.h for overview.

#include "AssetBrowser.h"
#include "AssetManifest.h"
#include "AssetWatcher.h"
#include "EngineLog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace sv {

static constexpr const char* TAG = "AssetBrowser";

namespace fs = std::filesystem;

// ── Internal helpers ───────────────────────────────────────────────

namespace {

// ASCII lowercase — keeps kindFromFilename / isIgnoredFilename
// deterministic across locales.
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

// Replace backslashes with forward slashes in-place.
std::string normalizeSlashes(std::string path)
{
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

// Return the extension in lowercase (including the dot), or empty string.
std::string lowerExtensionOf(std::string_view filename)
{
    auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    return toLowerAscii(filename.substr(dot));
}

// True if `hay` ends with `needle` (ASCII case-insensitive).
bool endsWithICase(std::string_view hay, std::string_view needle)
{
    if (needle.size() > hay.size()) return false;
    std::string hayLow    = toLowerAscii(hay.substr(hay.size() - needle.size()));
    std::string needleLow = toLowerAscii(needle);
    return hayLow == needleLow;
}

// Case-insensitive substring search.
bool containsICase(std::string_view hay, std::string_view needle)
{
    if (needle.empty()) return true;
    std::string hayLow    = toLowerAscii(hay);
    std::string needleLow = toLowerAscii(needle);
    return hayLow.find(needleLow) != std::string::npos;
}

// Filename stem — strip the trailing extension from name.
// Handles compound ".scene.json" by stripping .json + .scene.
std::string filenameStem(std::string_view filename)
{
    std::string name{filename};
    // Strip .json first
    if (endsWithICase(name, ".json")) {
        name.resize(name.size() - 5);
    }
    // Strip trailing ".scene" if present (so level_01.scene.json → level_01)
    if (endsWithICase(name, ".scene")) {
        name.resize(name.size() - 6);
    } else {
        auto dot = name.find_last_of('.');
        if (dot != std::string::npos) name.resize(dot);
    }
    return name;
}

} // anonymous

// ── kindFromFilename ───────────────────────────────────────────────

AssetKind kindFromFilename(std::string_view filename)
{
    // Compound suffixes first (.scene.json, .meta.json, .thumb.png).
    if (endsWithICase(filename, ".scene.json")) return AssetKind::Scene;
    if (endsWithICase(filename, ".meta.json"))  return AssetKind::Other;
    if (endsWithICase(filename, ".thumb.png"))  return AssetKind::Other;

    std::string ext = lowerExtensionOf(filename);

    // Mesh
    if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj")
        return AssetKind::Mesh;

    // Texture
    if (ext == ".png"  || ext == ".jpg"  || ext == ".jpeg" ||
        ext == ".tga"  || ext == ".bmp"  || ext == ".dds"  ||
        ext == ".ktx"  || ext == ".ktx2" || ext == ".hdr"  ||
        ext == ".exr")
        return AssetKind::Texture;

    // Audio
    if (ext == ".ogg"  || ext == ".wav"  || ext == ".mp3"  ||
        ext == ".flac" || ext == ".opus")
        return AssetKind::Audio;

    // Shader
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" ||
        ext == ".geom" || ext == ".tesc" || ext == ".tese" ||
        ext == ".rgen" || ext == ".rchit"|| ext == ".rmiss"||
        ext == ".glsl" || ext == ".hlsl" || ext == ".spv")
        return AssetKind::Shader;

    // Material
    if (ext == ".mat" || ext == ".material")
        return AssetKind::Material;

    // Animation
    if (ext == ".anim" || ext == ".animation")
        return AssetKind::Animation;

    return AssetKind::Other;
}

// ── isIgnoredFilename ──────────────────────────────────────────────

bool isIgnoredFilename(std::string_view filename)
{
    if (filename.empty())                  return true;
    if (filename.front() == '.')           return true;  // dotfiles (.DS_Store etc.)
    if (endsWithICase(filename, ".meta.json")) return true;
    if (endsWithICase(filename, ".thumb.png")) return true;  // baked thumbnails
    if (endsWithICase(filename, ".tmp"))       return true;
    if (endsWithICase(filename, "~"))          return true;
    return false;
}

// ── ImportSettings JSON round-trip ────────────────────────────────

nlohmann::json ImportSettings::toJson() const
{
    nlohmann::json doc;
    doc["version"]         = 1;
    doc["scale"]           = scale;
    doc["upAxis"]          = (upAxis == UpAxis::Z) ? "Z" : "Y";
    doc["materialMapping"] = materialMapping;
    doc["preload"]         = preload;
    return doc;
}

ImportSettings ImportSettings::fromJson(const nlohmann::json& doc)
{
    ImportSettings out;
    if (doc.contains("scale") && doc["scale"].is_number()) {
        out.scale = doc["scale"].get<float>();
    }
    if (doc.contains("upAxis") && doc["upAxis"].is_string()) {
        std::string u = doc["upAxis"].get<std::string>();
        // ASCII-upper for robustness
        for (char& c : u) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc >= 'a' && uc <= 'z') c = static_cast<char>(uc - ('a' - 'A'));
        }
        out.upAxis = (u == "Z") ? UpAxis::Z : UpAxis::Y;
    }
    if (doc.contains("materialMapping") && doc["materialMapping"].is_string()) {
        out.materialMapping = doc["materialMapping"].get<std::string>();
    }
    if (doc.contains("preload") && doc["preload"].is_boolean()) {
        out.preload = doc["preload"].get<bool>();
    }
    return out;
}

// ── AssetBrowser ───────────────────────────────────────────────────

AssetBrowser::AssetBrowser()  = default;
AssetBrowser::~AssetBrowser() = default;

bool AssetBrowser::scan(const std::string& rootDir)
{
    m_entries.clear();
    // import cache deliberately kept — rescans should not lose user edits.
    m_rootDir.clear();

    std::error_code ec;
    fs::path root(rootDir);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        SV_LOG_WARN(TAG, "Scan root missing or not a directory: %s",
                    rootDir.c_str());
        return false;
    }

    // Store canonical-ish absolute root with forward slashes.
    fs::path absRoot = fs::absolute(root, ec);
    if (ec) absRoot = root;
    m_rootDir = normalizeSlashes(absRoot.string());

    fs::recursive_directory_iterator it(absRoot, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        SV_LOG_WARN(TAG, "Cannot open directory: %s (%s)",
                    rootDir.c_str(), ec.message().c_str());
        return false;
    }

    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            SV_LOG_WARN(TAG, "Scan iterator error: %s", ec.message().c_str());
            ec.clear();
            continue;
        }
        const auto& dirent = *it;
        if (!dirent.is_regular_file(ec)) continue;

        std::string filename = dirent.path().filename().string();
        if (isIgnoredFilename(filename)) continue;

        // Build relative + absolute with forward slashes.
        fs::path relP = fs::relative(dirent.path(), absRoot, ec);
        if (ec) {
            ec.clear();
            relP = dirent.path().lexically_relative(absRoot);
        }
        std::string relPath = normalizeSlashes(relP.generic_string());
        std::string absPath = normalizeSlashes(dirent.path().generic_string());

        uint64_t sizeB = 0;
        auto     sz    = dirent.file_size(ec);
        if (!ec) sizeB = static_cast<uint64_t>(sz);
        ec.clear();

        int64_t mtime = 0;
        auto    tt    = dirent.last_write_time(ec);
        if (!ec) mtime = tt.time_since_epoch().count();
        ec.clear();

        ingestFile(absPath, relPath, sizeB, mtime);
    }

    // Stable order: by relativePath so tests are deterministic on all OSes.
    std::sort(m_entries.begin(), m_entries.end(),
              [](const AssetBrowserEntry& a, const AssetBrowserEntry& b) {
                  return a.relativePath < b.relativePath;
              });

    // Auto-load "<file>.meta.json" siblings into the cache.
    // Cache-wins: entries already set in-memory are NOT overwritten so
    // uncommitted edits survive a rescan. Invalid JSON is logged and
    // skipped without aborting the scan.
    if (m_autoLoadMeta) {
        size_t loaded = 0;
        for (const auto& e : m_entries) {
            if (m_importCache.find(e.relativePath) != m_importCache.end()) {
                continue;  // cache-wins
            }
            std::string metaPath = e.absolutePath + ".meta.json";
            std::error_code metaEc;
            if (!fs::exists(metaPath, metaEc) ||
                !fs::is_regular_file(metaPath, metaEc)) {
                continue;
            }

            std::ifstream in(metaPath);
            if (!in.good()) continue;
            nlohmann::json doc;
            try {
                in >> doc;
            } catch (const nlohmann::json::exception& ex) {
                SV_LOG_WARN(TAG, "Failed to parse %s: %s",
                            metaPath.c_str(), ex.what());
                continue;
            }
            m_importCache[e.relativePath] = ImportSettings::fromJson(doc);
            ++loaded;
        }
        if (loaded > 0) {
            SV_LOG_INFO(TAG, "Auto-loaded %zu .meta.json sidecar(s)", loaded);
        }
    }

    SV_LOG_INFO(TAG, "Scanned %zu asset(s) under %s",
                m_entries.size(), m_rootDir.c_str());
    return true;
}

bool AssetBrowser::rescan()
{
    if (m_rootDir.empty()) return false;
    std::string root = m_rootDir;
    return scan(root);
}

void AssetBrowser::clear()
{
    m_entries.clear();
    m_importCache.clear();
    m_rootDir.clear();
}

void AssetBrowser::ingestFile(const std::string& absPath,
                              const std::string& relPath,
                              uint64_t           sizeBytes,
                              int64_t            lastModified)
{
    AssetBrowserEntry e;
    // Filename is the leaf of relPath.
    auto slash = relPath.find_last_of('/');
    e.name = (slash == std::string::npos) ? relPath : relPath.substr(slash + 1);
    e.relativePath = relPath;
    e.absolutePath = absPath;
    e.extension    = lowerExtensionOf(e.name);
    e.kind         = kindFromFilename(e.name);
    e.sizeBytes    = sizeBytes;
    e.lastModified = lastModified;
    m_entries.push_back(std::move(e));
}

std::vector<const AssetBrowserEntry*>
AssetBrowser::entriesOfKind(AssetKind k) const
{
    std::vector<const AssetBrowserEntry*> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        if (e.kind == k) out.push_back(&e);
    }
    return out;
}

std::vector<const AssetBrowserEntry*>
AssetBrowser::filter(std::string_view needle) const
{
    std::vector<const AssetBrowserEntry*> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        if (containsICase(e.name,         needle) ||
            containsICase(e.relativePath, needle)) {
            out.push_back(&e);
        }
    }
    return out;
}

const AssetBrowserEntry*
AssetBrowser::findByRelativePath(const std::string& relativePath) const
{
    for (const auto& e : m_entries) {
        if (e.relativePath == relativePath) return &e;
    }
    return nullptr;
}

ImportSettings
AssetBrowser::getImportSettings(const std::string& relativePath) const
{
    auto it = m_importCache.find(relativePath);
    if (it != m_importCache.end()) return it->second;
    return {};
}

void AssetBrowser::setImportSettings(const std::string& relativePath,
                                     ImportSettings     settings)
{
    m_importCache[relativePath] = std::move(settings);

    // Optional write-through to <file>.meta.json. Silently
    // skips writes for unknown paths (saveMetaFile logs the reason).
    if (m_autoSaveMeta) {
        (void) saveMetaFile(relativePath);
    }
}

bool AssetBrowser::hasImportSettings(const std::string& relativePath) const
{
    return m_importCache.find(relativePath) != m_importCache.end();
}

// ── sidecar persistence ─────────────────────────────────

std::string AssetBrowser::metaFilePathFor(const std::string& relativePath) const
{
    const AssetBrowserEntry* e = findByRelativePath(relativePath);
    if (!e) return {};
    return e->absolutePath + ".meta.json";
}

bool AssetBrowser::loadMetaFile(const std::string& relativePath)
{
    const AssetBrowserEntry* e = findByRelativePath(relativePath);
    if (!e) {
        SV_LOG_WARN(TAG, "loadMetaFile: unknown relativePath '%s'",
                    relativePath.c_str());
        return false;
    }
    std::string metaPath = e->absolutePath + ".meta.json";

    std::error_code ec;
    if (!fs::exists(metaPath, ec) || !fs::is_regular_file(metaPath, ec)) {
        return false;
    }

    std::ifstream in(metaPath);
    if (!in.good()) {
        SV_LOG_WARN(TAG, "loadMetaFile: cannot open %s", metaPath.c_str());
        return false;
    }

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& ex) {
        SV_LOG_WARN(TAG, "loadMetaFile: invalid JSON in %s: %s",
                    metaPath.c_str(), ex.what());
        return false;
    }

    m_importCache[relativePath] = ImportSettings::fromJson(doc);
    return true;
}

bool AssetBrowser::saveMetaFile(const std::string& relativePath) const
{
    const AssetBrowserEntry* e = findByRelativePath(relativePath);
    if (!e) {
        SV_LOG_WARN(TAG, "saveMetaFile: unknown relativePath '%s'",
                    relativePath.c_str());
        return false;
    }
    std::string metaPath = e->absolutePath + ".meta.json";

    // Use cache entry if present, otherwise a default-constructed
    // ImportSettings (so the user gets a stable JSON scaffold on disk).
    ImportSettings s;
    auto it = m_importCache.find(relativePath);
    if (it != m_importCache.end()) s = it->second;

    std::error_code ec;
    fs::path absParent = fs::path(e->absolutePath).parent_path();
    if (!absParent.empty()) {
        fs::create_directories(absParent, ec);  // best-effort; ignore ec
    }

    std::ofstream out(metaPath);
    if (!out.good()) {
        SV_LOG_WARN(TAG, "saveMetaFile: cannot write %s", metaPath.c_str());
        return false;
    }
    out << s.toJson().dump(2);
    out.flush();
    if (!out.good()) {
        SV_LOG_WARN(TAG, "saveMetaFile: write failed for %s", metaPath.c_str());
        return false;
    }
    return true;
}

void AssetBrowser::populateManifest(AssetManifest&     out,
                                    const std::string& manifestName) const
{
    nlohmann::json doc;
    doc["version"] = 1;
    doc["name"]    = manifestName;
    doc["assets"]  = nlohmann::json::array();

    // Name collisions: first occurrence keeps the bare stem. Later
    // occurrences disambiguate with "<parentDir>/<stem>". If that
    // still collides, the full relative path (minus extension) is
    // used. This matches the intuition "character.glb wins over
    // subfolder/character.glb" while still exposing the collision.
    std::unordered_set<std::string> used;

    for (const auto& e : m_entries) {
        std::string stem = filenameStem(e.name);
        std::string baseName = stem;

        if (used.count(baseName)) {
            // Fall back: parent dir + stem.
            std::string parentDir;
            auto        slash = e.relativePath.find_last_of('/');
            if (slash != std::string::npos) {
                std::string relDir = e.relativePath.substr(0, slash);
                auto        ps    = relDir.find_last_of('/');
                parentDir = (ps == std::string::npos) ? relDir : relDir.substr(ps + 1);
            }
            if (!parentDir.empty()) baseName = parentDir + "/" + stem;
        }
        if (used.count(baseName)) {
            // Last resort: full path minus extension.
            baseName = e.relativePath;
            auto dot = baseName.find_last_of('.');
            if (dot != std::string::npos) baseName.resize(dot);
        }
        used.insert(baseName);

        nlohmann::json entry;
        entry["name"]    = baseName;
        entry["path"]    = e.relativePath;
        entry["kind"]    = assetKindToString(e.kind);
        // Import settings override preload if set; otherwise default true.
        bool preload = true;
        auto it = m_importCache.find(e.relativePath);
        if (it != m_importCache.end()) preload = it->second.preload;
        entry["preload"] = preload;
        doc["assets"].push_back(std::move(entry));
    }

    out.loadFromJson(doc);
}

void AssetBrowser::attachWatcher(AssetWatcher* watcher)
{
    if (!watcher)         return;
    if (m_rootDir.empty()) return;

    // Recursive registration. Walks the full scan tree so
    // nested-directory changes (e.g. "assets/meshes/characters/foo.glb")
    // trigger a rescan. The callback re-registers the watch list on
    // every rescan() so newly-created files and subdirectories are
    // picked up without the caller having to call attachWatcher again.
    const std::vector<std::string> exts = {
        ".glb", ".gltf", ".fbx", ".obj",
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx", ".ktx2",
        ".ogg", ".wav", ".mp3", ".flac",
        ".vert", ".frag", ".comp", ".glsl", ".spv",
        ".json",  // catches .scene.json + manifest
        ".mat", ".anim",
    };

    watcher->watchDirectoryRecursive(
        m_rootDir, exts, [this]() { this->rescan(); });
}

} // namespace sv
