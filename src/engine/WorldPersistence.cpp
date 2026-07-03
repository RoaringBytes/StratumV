// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── WorldPersistence implementation ────────────────────────────────
// Binary file format described in WorldPersistence.h. Pure logic
// built on stdio — no Vulkan, no ReplicationRegistry dependency
// beyond the registry lookup during load. Lives in the core subset.

#include "WorldPersistence.h"
#include "CrtCompat.h"

#include "EngineLog.h"
#include "ReplicationRegistry.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace sv {

namespace {

// Little-endian primitive writers mirroring EditTransaction.cpp. The
// two TUs intentionally keep independent copies so neither has to
// depend on the other's private header surface.
inline void writeU8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}
inline void writeU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v         & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8)  & 0xFF));
}
inline void writeU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v         & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8)  & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
inline void writeU64LE(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

inline uint16_t readU16LE(const uint8_t* p) {
    return  static_cast<uint16_t>(p[0])
         | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t readU32LE(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) <<  8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t readU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

// ── Byte buffer cursor helper ─────────────────────────────────────
// Tracks a read offset over the input buffer so every reader shares
// the same "short-buffer" refuse path.
struct ReadCursor {
    const uint8_t* data;
    size_t         size;
    size_t         pos  = 0;

    bool read(void* dst, size_t bytes) {
        if (pos + bytes > size) return false;
        std::memcpy(dst, data + pos, bytes);
        pos += bytes;
        return true;
    }
    bool readU8LE(uint8_t& out) {
        if (pos + 1 > size) return false;
        out = data[pos++];
        return true;
    }
    bool readU16LE(uint16_t& out) {
        if (pos + 2 > size) return false;
        out = sv::readU16LE(data + pos);
        pos += 2;
        return true;
    }
    bool readU32LE(uint32_t& out) {
        if (pos + 4 > size) return false;
        out = sv::readU32LE(data + pos);
        pos += 4;
        return true;
    }
    bool readU64LE(uint64_t& out) {
        if (pos + 8 > size) return false;
        out = sv::readU64LE(data + pos);
        pos += 8;
        return true;
    }
};

} // namespace

// ── Status diagnostics ────────────────────────────────────────────

const char* worldPersistenceStatusToString(WorldPersistenceStatus s) {
    switch (s) {
        case WorldPersistenceStatus::Ok:                 return "Ok";
        case WorldPersistenceStatus::MissingFile:        return "MissingFile";
        case WorldPersistenceStatus::IoError:            return "IoError";
        case WorldPersistenceStatus::CorruptHeader:      return "CorruptHeader";
        case WorldPersistenceStatus::UnsupportedVersion: return "UnsupportedVersion";
        case WorldPersistenceStatus::PayloadDecodeFail:  return "PayloadDecodeFail";
        case WorldPersistenceStatus::UnknownType:        return "UnknownType";
    }
    return "Unknown";
}

// ── encode / decode ───────────────────────────────────────────────

WorldPersistenceStatus encodeWorldToBytes(const PersistedWorld& world,
                                          std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(kWorldFileHeaderSize + world.entities.size() * 64);

    // Header (32 bytes)
    for (size_t i = 0; i < kWorldFileMagicLen; ++i) {
        out.push_back(static_cast<uint8_t>(kWorldFileMagic[i]));
    }
    writeU32LE(out, kWorldFileVersion);
    writeU32LE(out, static_cast<uint32_t>(world.entities.size()));
    writeU32LE(out, world.nextEntityId);
    writeU32LE(out, world.nextClientId);
    writeU64LE(out, world.nextTxId);
    // kWorldFileHeaderSize == 32 (8 magic + 4 version + 4 count +
    //   4 nextEntityId + 4 nextClientId + 8 nextTxId). No padding.

    // Per-entity records
    for (const PersistedEntity& ent : world.entities) {
        writeU32LE(out, ent.entityId);
        writeU8(out, ent.authority);
        writeU32LE(out, ent.ownerClientId);
        writeU32LE(out, ent.typeNameHash);
        if (ent.label.size() > 0xFFFFu) {
            SV_LOG_WARN("WorldPersistence",
                "label for entity %u exceeds u16 length (%zu bytes)",
                static_cast<unsigned>(ent.entityId),
                ent.label.size());
            return WorldPersistenceStatus::IoError;
        }
        writeU16LE(out, static_cast<uint16_t>(ent.label.size()));
        if (!ent.label.empty()) {
            out.insert(out.end(),
                       ent.label.begin(),
                       ent.label.end());
        }
        if (ent.payload.size() > 0xFFFFFFFFu) {
            SV_LOG_WARN("WorldPersistence",
                "payload for entity %u exceeds u32 length (%zu bytes)",
                static_cast<unsigned>(ent.entityId),
                ent.payload.size());
            return WorldPersistenceStatus::IoError;
        }
        writeU32LE(out, static_cast<uint32_t>(ent.payload.size()));
        if (!ent.payload.empty()) {
            out.insert(out.end(),
                       ent.payload.begin(),
                       ent.payload.end());
        }
    }
    return WorldPersistenceStatus::Ok;
}

WorldPersistenceStatus decodeWorldFromBytes(const uint8_t*  data,
                                            size_t          size,
                                            PersistedWorld& outWorld) {
    outWorld = PersistedWorld{};
    if (!data || size < kWorldFileHeaderSize) {
        return WorldPersistenceStatus::CorruptHeader;
    }

    // Magic check
    for (size_t i = 0; i < kWorldFileMagicLen; ++i) {
        if (data[i] != static_cast<uint8_t>(kWorldFileMagic[i])) {
            return WorldPersistenceStatus::CorruptHeader;
        }
    }

    ReadCursor c{data, size, kWorldFileMagicLen};

    uint32_t version     = 0;
    uint32_t entityCount = 0;
    if (!c.readU32LE(version))     return WorldPersistenceStatus::CorruptHeader;
    if (version != kWorldFileVersion) {
        return WorldPersistenceStatus::UnsupportedVersion;
    }
    if (!c.readU32LE(entityCount))             return WorldPersistenceStatus::CorruptHeader;
    if (!c.readU32LE(outWorld.nextEntityId))   return WorldPersistenceStatus::CorruptHeader;
    if (!c.readU32LE(outWorld.nextClientId))   return WorldPersistenceStatus::CorruptHeader;
    if (!c.readU64LE(outWorld.nextTxId))       return WorldPersistenceStatus::CorruptHeader;
    // cursor is now at offset 32 — start of the first entity record.

    outWorld.entities.reserve(entityCount);
    for (uint32_t i = 0; i < entityCount; ++i) {
        PersistedEntity ent;
        if (!c.readU32LE(ent.entityId))      return WorldPersistenceStatus::CorruptHeader;
        if (!c.readU8LE (ent.authority))     return WorldPersistenceStatus::CorruptHeader;
        if (!c.readU32LE(ent.ownerClientId)) return WorldPersistenceStatus::CorruptHeader;
        if (!c.readU32LE(ent.typeNameHash))  return WorldPersistenceStatus::CorruptHeader;
        uint16_t labelLen = 0;
        if (!c.readU16LE(labelLen))          return WorldPersistenceStatus::CorruptHeader;
        ent.label.resize(labelLen);
        if (labelLen > 0) {
            if (!c.read(ent.label.data(), labelLen)) {
                return WorldPersistenceStatus::CorruptHeader;
            }
        }
        uint32_t payloadLen = 0;
        if (!c.readU32LE(payloadLen))        return WorldPersistenceStatus::CorruptHeader;
        ent.payload.resize(payloadLen);
        if (payloadLen > 0) {
            if (!c.read(ent.payload.data(), payloadLen)) {
                return WorldPersistenceStatus::CorruptHeader;
            }
        }

        // Probe the registry to fail fast on any type the local
        // replication table doesn't know about. The server's load
        // path can only rehydrate components it can decode; an
        // unknown typeNameHash is almost always a schema drift or
        // a world.svbin from a different engine version.
        const ReplicationMeta* meta =
            ReplicationRegistry::get().findByHash(ent.typeNameHash);
        if (!meta) {
            SV_LOG_WARN("WorldPersistence",
                "entity %u has unknown typeNameHash 0x%08x — aborting load",
                static_cast<unsigned>(ent.entityId),
                static_cast<unsigned>(ent.typeNameHash));
            return WorldPersistenceStatus::UnknownType;
        }

        outWorld.entities.push_back(std::move(ent));
    }
    return WorldPersistenceStatus::Ok;
}

// ── File I/O ──────────────────────────────────────────────────────

WorldPersistenceStatus saveWorldToFile(const std::string&    filePath,
                                       const PersistedWorld& world) {
    // Build in memory first so a failed encode doesn't touch disk.
    std::vector<uint8_t> bytes;
    auto enc = encodeWorldToBytes(world, bytes);
    if (enc != WorldPersistenceStatus::Ok) return enc;

    // Atomic write via a temp file + rename. Two processes racing
    // on the same directory would be a configuration bug, but the
    // temp filename is still process-unique so accidental co-tenancy
    // doesn't clobber.
    namespace fs = std::filesystem;
    fs::path target = sv::U8Path(filePath);
    fs::path dir    = target.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            SV_LOG_WARN("WorldPersistence",
                "create_directories failed for %s: %s",
                dir.string().c_str(), ec.message().c_str());
            return WorldPersistenceStatus::IoError;
        }
    }

    fs::path temp = target;
    temp += fs::path(".tmp-" + std::to_string(
        static_cast<unsigned long long>(
            std::hash<std::string>{}(filePath))));

    {
        FILE* fp = sv::FOpen(temp.string().c_str(), "wb");
        if (!fp) {
            SV_LOG_WARN("WorldPersistence",
                "fopen('%s', wb) failed: %s",
                temp.string().c_str(), sv::StrError(errno).c_str());
            return WorldPersistenceStatus::IoError;
        }
        const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), fp);
        std::fclose(fp);
        if (written != bytes.size()) {
            SV_LOG_WARN("WorldPersistence",
                "fwrite short: expected %zu got %zu",
                bytes.size(), written);
            std::error_code ec;
            fs::remove(temp, ec);
            return WorldPersistenceStatus::IoError;
        }
    }

    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        // On Windows rename fails if the target exists. Fall back
        // to remove-then-rename.
        fs::remove(target, ec);
        ec.clear();
        fs::rename(temp, target, ec);
        if (ec) {
            SV_LOG_WARN("WorldPersistence",
                "rename %s -> %s failed: %s",
                temp.string().c_str(),
                target.string().c_str(),
                ec.message().c_str());
            std::error_code rm;
            fs::remove(temp, rm);
            return WorldPersistenceStatus::IoError;
        }
    }

    SV_LOG_INFO("WorldPersistence",
        "wrote %s (%zu bytes, %zu entities)",
        target.string().c_str(),
        bytes.size(),
        world.entities.size());
    return WorldPersistenceStatus::Ok;
}

WorldPersistenceStatus loadWorldFromFile(const std::string& filePath,
                                         PersistedWorld&    outWorld) {
    outWorld = PersistedWorld{};
    namespace fs = std::filesystem;
    fs::path target = sv::U8Path(filePath);

    std::error_code ec;
    if (!fs::exists(target, ec)) {
        return WorldPersistenceStatus::MissingFile;
    }
    if (!fs::is_regular_file(target, ec)) {
        return WorldPersistenceStatus::CorruptHeader;
    }

    const uintmax_t fileSize = fs::file_size(target, ec);
    if (ec || fileSize == 0 || fileSize < kWorldFileHeaderSize) {
        return WorldPersistenceStatus::CorruptHeader;
    }

    FILE* fp = sv::FOpen(target.string().c_str(), "rb");
    if (!fp) {
        SV_LOG_WARN("WorldPersistence",
            "fopen('%s', rb) failed: %s",
            target.string().c_str(), sv::StrError(errno).c_str());
        return WorldPersistenceStatus::IoError;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    const size_t n = std::fread(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    if (n != bytes.size()) {
        SV_LOG_WARN("WorldPersistence",
            "fread short: expected %zu got %zu",
            bytes.size(), n);
        return WorldPersistenceStatus::IoError;
    }

    auto status = decodeWorldFromBytes(bytes.data(), bytes.size(), outWorld);
    if (status == WorldPersistenceStatus::Ok) {
        SV_LOG_INFO("WorldPersistence",
            "loaded %s (%zu bytes, %zu entities, nextEntityId=%u nextClientId=%u)",
            target.string().c_str(),
            bytes.size(),
            outWorld.entities.size(),
            static_cast<unsigned>(outWorld.nextEntityId),
            static_cast<unsigned>(outWorld.nextClientId));
    }
    return status;
}

} // namespace sv
