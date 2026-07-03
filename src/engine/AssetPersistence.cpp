// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetPersistence implementation ────────────────────
// In-memory content-addressable store with optional on-disk
// write-through. Lives in the core subset — no Vulkan, no ImGui,
// no third-party JSON dependency beyond a hand-rolled metadata
// serializer so we don't inherit nlohmann::json into the core
// subset. The metadata file is tiny (kind/name/size), so hand-
// rolling is cheaper than pulling in a full JSON parser just
// for round-tripping those three fields.

#include "AssetPersistence.h"

#include "CrtCompat.h"
#include "EngineLog.h"
#include "net/ReplicationProtocol.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace sv {

namespace {

namespace fs = std::filesystem;

// ── Metadata file format ──────────────────────────────────────────
// Single-line key=value lines so we don't need a JSON parser in the
// core subset. Format:
//
//   kind=<decimal u8>
//   size=<decimal u32>
//   name=<UTF-8, no newlines>
//
// Any line starting with '#' is ignored. Unknown keys are ignored.
// `name` is read as everything after the first '=' on its line, so
// a filename with '=' characters survives the round-trip.
struct MetaFields {
    uint8_t     kind = 0;
    uint32_t    size = 0;
    std::string name;
    bool        haveKind = false;
    bool        haveSize = false;
    bool        haveName = false;
};

bool parseMetaFile(const fs::path& path, MetaFields& out) {
    out = MetaFields{};
    std::ifstream f(path);
    if (!f.good()) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key   = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "kind") {
            try {
                unsigned long v = std::stoul(value);
                out.kind = static_cast<uint8_t>(v > 255 ? 0 : v);
                out.haveKind = true;
            } catch (...) {
                return false;
            }
        } else if (key == "size") {
            try {
                unsigned long v = std::stoul(value);
                out.size = static_cast<uint32_t>(v);
                out.haveSize = true;
            } catch (...) {
                return false;
            }
        } else if (key == "name") {
            out.name = value;
            out.haveName = true;
        }
    }
    return out.haveKind && out.haveSize && out.haveName;
}

bool writeMetaFile(const fs::path& path, const AssetRecord& rec) {
    // Build the full contents in memory first, then write atomically
    // via a temp file + rename. Keeps the common-case failure (disk
    // full) from leaving a half-written meta on disk next to a fully
    // written .bin.
    std::ostringstream oss;
    oss << "# StratumV asset metadata\n";
    oss << "kind=" << static_cast<unsigned>(rec.assetKind) << "\n";
    oss << "size=" << rec.byteSize << "\n";
    oss << "name=" << rec.name << "\n";
    const std::string body = oss.str();

    fs::path temp = path;
    temp += ".tmp";
    {
        FILE* fp = sv::FOpen(temp.string().c_str(), "wb");
        if (!fp) return false;
        const size_t n = std::fwrite(body.data(), 1, body.size(), fp);
        std::fclose(fp);
        if (n != body.size()) {
            std::error_code ec;
            fs::remove(temp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp, path, ec);
        if (ec) {
            std::error_code rm;
            fs::remove(temp, rm);
            return false;
        }
    }
    return true;
}

bool writeBinaryFile(const fs::path& path,
                     const uint8_t*  data,
                     size_t          size) {
    fs::path temp = path;
    temp += ".tmp";
    {
        FILE* fp = sv::FOpen(temp.string().c_str(), "wb");
        if (!fp) return false;
        size_t written = 0;
        if (size > 0) {
            written = std::fwrite(data, 1, size, fp);
        }
        std::fclose(fp);
        if (written != size) {
            std::error_code ec;
            fs::remove(temp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp, path, ec);
        if (ec) {
            std::error_code rm;
            fs::remove(temp, rm);
            return false;
        }
    }
    return true;
}

bool readBinaryFile(const fs::path&       path,
                    std::vector<uint8_t>& out) {
    std::error_code ec;
    const uintmax_t sz = fs::file_size(path, ec);
    if (ec) return false;
    FILE* fp = sv::FOpen(path.string().c_str(), "rb");
    if (!fp) return false;
    out.resize(static_cast<size_t>(sz));
    const size_t n = (sz > 0) ? std::fread(out.data(), 1, out.size(), fp) : 0;
    std::fclose(fp);
    return n == out.size();
}

} // namespace

const char* assetPersistenceStatusToString(AssetPersistenceStatus s) {
    switch (s) {
        case AssetPersistenceStatus::Ok:            return "Ok";
        case AssetPersistenceStatus::BadArg:        return "BadArg";
        case AssetPersistenceStatus::HashMismatch:  return "HashMismatch";
        case AssetPersistenceStatus::MissingFile:   return "MissingFile";
        case AssetPersistenceStatus::IoError:       return "IoError";
        case AssetPersistenceStatus::CorruptHeader: return "CorruptHeader";
        case AssetPersistenceStatus::SizeExceeded:  return "SizeExceeded";
    }
    return "Unknown";
}

AssetHash hashAssetBytes(const uint8_t* data, size_t size) {
    return sha256(data, size);
}

std::string assetFilePath(const std::string& rootDir,
                          const AssetHash&   hash) {
    if (rootDir.empty()) return {};
    const std::string hex = digestToHex(hash);
    // <root>/<2 hex>/<62 hex>.bin
    std::string out;
    out.reserve(rootDir.size() + 1 + 2 + 1 + 62 + 4);
    out.append(rootDir);
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    out.append(hex, 0, 2);
    out.push_back('/');
    out.append(hex, 2, 62);
    out.append(".bin");
    return out;
}

std::string assetMetaPath(const std::string& rootDir,
                          const AssetHash&   hash) {
    std::string p = assetFilePath(rootDir, hash);
    if (p.empty()) return {};
    // Swap .bin for .meta.json sibling.
    p.resize(p.size() - 4);
    p.append(".meta.json");
    return p;
}

AssetPersistenceStatus AssetPersistence::setRootDir(const std::string& rootDir) {
    m_rootDir = rootDir;
    if (m_rootDir.empty()) {
        // Keep the in-memory cache; caller may re-enable write-through later.
        return AssetPersistenceStatus::Ok;
    }
    std::error_code ec;
    fs::create_directories(sv::U8Path(m_rootDir), ec);
    if (ec) {
        SV_LOG_WARN("AssetPersistence",
            "create_directories('%s') failed: %s",
            m_rootDir.c_str(), ec.message().c_str());
        m_rootDir.clear();
        return AssetPersistenceStatus::IoError;
    }
    scanRootDir();
    return AssetPersistenceStatus::Ok;
}

bool AssetPersistence::contains(const AssetHash& hash) const {
    const std::string key = digestToHex(hash);
    return m_records.find(key) != m_records.end();
}

const AssetRecord* AssetPersistence::find(const AssetHash& hash) const {
    const std::string key = digestToHex(hash);
    auto it = m_records.find(key);
    return (it == m_records.end()) ? nullptr : &it->second;
}

void AssetPersistence::clear() {
    m_records.clear();
}

AssetPersistenceStatus AssetPersistence::save(const AssetHash&   hash,
                                              uint8_t            assetKind,
                                              const std::string& name,
                                              const uint8_t*     bytes,
                                              size_t             byteSize) {
    if (byteSize > 0 && !bytes) return AssetPersistenceStatus::BadArg;
    if (byteSize > sv::net::kAssetByteLimit) {
        return AssetPersistenceStatus::SizeExceeded;
    }

    // Verify the declared hash matches the bytes BEFORE we touch
    // memory or disk. Prevents a bad client from pinning arbitrary
    // bytes under a chosen hash.
    const AssetHash actual = sha256(bytes, byteSize);
    if (actual != hash) {
        return AssetPersistenceStatus::HashMismatch;
    }

    const std::string key = digestToHex(hash);

    // Already cached? The earliest wins — if the exact same hash
    // is stored again (identical bytes by definition) we log and
    // return Ok without rewriting. This is the dedup hot path.
    auto it = m_records.find(key);
    if (it != m_records.end()) {
        SV_LOG_INFO("AssetPersistence",
            "save: dedup hit for %s (name=%s)",
            key.c_str(), name.c_str());
        return AssetPersistenceStatus::Ok;
    }

    AssetRecord rec;
    rec.hash      = hash;
    rec.byteSize  = static_cast<uint32_t>(byteSize);
    rec.assetKind = assetKind;
    rec.name      = name;
    rec.bytes.assign(bytes, bytes + byteSize);

    if (!m_rootDir.empty()) {
        if (!writeToDisk(rec)) {
            SV_LOG_WARN("AssetPersistence",
                "writeToDisk failed for %s", key.c_str());
            return AssetPersistenceStatus::IoError;
        }
    }

    m_records.emplace(key, std::move(rec));
    return AssetPersistenceStatus::Ok;
}

AssetPersistenceStatus AssetPersistence::load(const AssetHash& hash,
                                              AssetRecord&     out) const {
    out = AssetRecord{};
    const std::string key = digestToHex(hash);
    auto it = m_records.find(key);
    if (it == m_records.end()) {
        return AssetPersistenceStatus::MissingFile;
    }
    out = it->second;   // copy; caller may mutate freely
    return AssetPersistenceStatus::Ok;
}

bool AssetPersistence::writeToDisk(const AssetRecord& rec) const {
    if (m_rootDir.empty()) return false;
    const std::string binPath  = assetFilePath(m_rootDir, rec.hash);
    const std::string metaPath = assetMetaPath(m_rootDir, rec.hash);
    fs::path bp = sv::U8Path(binPath);
    fs::path mp = sv::U8Path(metaPath);

    // Make sure the 2-hex shard directory exists.
    std::error_code ec;
    fs::create_directories(bp.parent_path(), ec);
    if (ec) {
        SV_LOG_WARN("AssetPersistence",
            "create_directories('%s') failed: %s",
            bp.parent_path().string().c_str(),
            ec.message().c_str());
        return false;
    }

    if (!writeBinaryFile(bp,
                         rec.bytes.data(),
                         rec.bytes.size())) {
        SV_LOG_WARN("AssetPersistence",
            "writeBinaryFile('%s') failed", bp.string().c_str());
        return false;
    }
    if (!writeMetaFile(mp, rec)) {
        SV_LOG_WARN("AssetPersistence",
            "writeMetaFile('%s') failed", mp.string().c_str());
        return false;
    }
    return true;
}

void AssetPersistence::scanRootDir() {
    if (m_rootDir.empty()) return;
    namespace fs = std::filesystem;
    fs::path root = sv::U8Path(m_rootDir);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;

    size_t loaded = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator{};
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string filename = it->path().filename().string();
        if (filename.size() < 4) continue;
        if (filename.substr(filename.size() - 4) != ".bin") continue;

        // Reconstruct the hash from the two-path-component shard layout:
        // .../<2hex>/<62hex>.bin
        const std::string stem = filename.substr(0, filename.size() - 4);
        if (stem.size() != 62) continue;
        const std::string shard = it->path().parent_path().filename().string();
        if (shard.size() != 2) continue;
        const std::string hex = shard + stem;
        AssetHash hash{};
        if (!digestFromHex(hex, hash)) continue;

        // Load bytes + metadata.
        std::vector<uint8_t> bytes;
        if (!readBinaryFile(it->path(), bytes)) continue;

        fs::path mp = it->path();
        std::string metaName = mp.filename().string();
        metaName = metaName.substr(0, metaName.size() - 4) + ".meta.json";
        mp = mp.parent_path() / metaName;
        MetaFields mf;
        if (!parseMetaFile(mp, mf)) {
            SV_LOG_WARN("AssetPersistence",
                "skipping %s: sibling meta file missing or invalid",
                it->path().string().c_str());
            continue;
        }

        // Cross-check: the bytes we read back must hash to the
        // keyed hash, AND the stored byteSize must match the
        // actual byte count. A mismatch means the file was
        // tampered with or truncated; drop it rather than cache
        // corrupt bytes.
        const AssetHash actual = sha256(bytes.data(), bytes.size());
        if (actual != hash) {
            SV_LOG_WARN("AssetPersistence",
                "skipping %s: hash mismatch on reload",
                it->path().string().c_str());
            continue;
        }
        if (mf.size != bytes.size()) {
            SV_LOG_WARN("AssetPersistence",
                "skipping %s: meta size %u != actual %zu",
                it->path().string().c_str(),
                static_cast<unsigned>(mf.size),
                bytes.size());
            continue;
        }

        AssetRecord rec;
        rec.hash      = hash;
        rec.byteSize  = mf.size;
        rec.assetKind = mf.kind;
        rec.name      = std::move(mf.name);
        rec.bytes     = std::move(bytes);
        m_records.emplace(hex, std::move(rec));
        ++loaded;
    }

    if (loaded > 0) {
        SV_LOG_INFO("AssetPersistence",
            "Scanned %s: loaded %zu assets into CAS",
            m_rootDir.c_str(), loaded);
    }
}

} // namespace sv
