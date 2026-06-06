// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── WorldPersistence tests ───────────────────────────────
// Lives in sv_core_tests so it runs on both the full Windows build
// and the Linux-or-Windows core-only carve-out. Tagged [persistence]
// with sub-tags for in-memory round-trip / file round-trip / error.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "EditTransaction.h"
#include "NetTransform.h"
#include "ReplicationRegistry.h"
#include "WorldPersistence.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

// Unique temp directory per test case — copied from the pattern in
// tests/test_util.h. Can't share test_util.h directly because
// sv_core_tests must not pull graphics-adjacent headers into its
// include path. Duplicating the few dozen lines keeps the core
// suite free of boundary violations.
struct CoreTempDir {
    std::filesystem::path path;

    CoreTempDir() {
        namespace fs = std::filesystem;
        static std::atomic<uint64_t> counter{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() / "stratumv_persistence_tests"
             / ("t_" + std::to_string(now) + "_" + std::to_string(n));
        fs::create_directories(path);
    }
    ~CoreTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    CoreTempDir(const CoreTempDir&) = delete;
    CoreTempDir& operator=(const CoreTempDir&) = delete;

    std::string filePath(const char* name) const {
        return (path / name).string();
    }
};

// Build a NetTransform-backed PersistedWorld with N entities.
sv::PersistedWorld makeTestWorld(size_t entityCount) {
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);

    sv::PersistedWorld w;
    w.nextEntityId = 200;
    w.nextClientId = 5;
    w.nextTxId     = 42;
    w.entities.reserve(entityCount);
    for (size_t i = 0; i < entityCount; ++i) {
        sv::PersistedEntity e;
        e.entityId      = static_cast<uint32_t>(10 + i);
        e.authority     = 0;   // Server
        e.ownerClientId = 0;
        e.typeNameHash  = meta->typeNameHash;
        e.label         = "Ent" + std::to_string(i);

        sv::NetTransform t;
        t.posX = 1.0f + static_cast<float>(i);
        t.posY = 50.0f;
        t.posZ = -2.0f - static_cast<float>(i);
        t.rotX = 0.0f;
        t.rotY = 0.0f;
        t.rotZ = 0.0f;
        t.rotW = 1.0f;

        sv::DirtyMask fullMask(meta->fields.size());
        fullMask.setAll();
        REQUIRE(sv::writeGenericSetFieldPayload(*meta, &t, fullMask, e.payload));
        w.entities.push_back(std::move(e));
    }
    return w;
}

} // namespace

// ── Status strings ────────────────────────────────────────────────

TEST_CASE("WorldPersistence: status string covers every enum value",
          "[persistence][status]") {
    using S = sv::WorldPersistenceStatus;
    REQUIRE(std::string("Ok")                 == sv::worldPersistenceStatusToString(S::Ok));
    REQUIRE(std::string("MissingFile")        == sv::worldPersistenceStatusToString(S::MissingFile));
    REQUIRE(std::string("IoError")            == sv::worldPersistenceStatusToString(S::IoError));
    REQUIRE(std::string("CorruptHeader")      == sv::worldPersistenceStatusToString(S::CorruptHeader));
    REQUIRE(std::string("UnsupportedVersion") == sv::worldPersistenceStatusToString(S::UnsupportedVersion));
    REQUIRE(std::string("PayloadDecodeFail")  == sv::worldPersistenceStatusToString(S::PayloadDecodeFail));
    REQUIRE(std::string("UnknownType")        == sv::worldPersistenceStatusToString(S::UnknownType));
}

// ── In-memory round-trip ──────────────────────────────────────────

TEST_CASE("WorldPersistence: empty world encode + decode round-trip",
          "[persistence][roundtrip]") {
    sv::PersistedWorld w;
    w.nextEntityId = 100;
    w.nextClientId = 1;
    w.nextTxId     = 1;

    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeWorldToBytes(w, bytes) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(bytes.size() == sv::kWorldFileHeaderSize);

    sv::PersistedWorld decoded;
    REQUIRE(sv::decodeWorldFromBytes(bytes.data(), bytes.size(), decoded)
            == sv::WorldPersistenceStatus::Ok);
    REQUIRE(decoded.entities.empty());
    REQUIRE(decoded.nextEntityId == 100);
    REQUIRE(decoded.nextClientId == 1);
    REQUIRE(decoded.nextTxId     == 1);
}

TEST_CASE("WorldPersistence: three-entity round-trip preserves every field",
          "[persistence][roundtrip]") {
    const sv::PersistedWorld src = makeTestWorld(3);

    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeWorldToBytes(src, bytes) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(bytes.size() > sv::kWorldFileHeaderSize);

    sv::PersistedWorld decoded;
    REQUIRE(sv::decodeWorldFromBytes(bytes.data(), bytes.size(), decoded)
            == sv::WorldPersistenceStatus::Ok);
    REQUIRE(decoded.entities.size() == 3);
    REQUIRE(decoded.nextEntityId == src.nextEntityId);
    REQUIRE(decoded.nextClientId == src.nextClientId);
    REQUIRE(decoded.nextTxId     == src.nextTxId);

    for (size_t i = 0; i < 3; ++i) {
        const auto& a = src.entities[i];
        const auto& b = decoded.entities[i];
        REQUIRE(a.entityId      == b.entityId);
        REQUIRE(a.authority     == b.authority);
        REQUIRE(a.ownerClientId == b.ownerClientId);
        REQUIRE(a.typeNameHash  == b.typeNameHash);
        REQUIRE(a.label         == b.label);
        REQUIRE(a.payload       == b.payload);
    }
}

TEST_CASE("WorldPersistence: decoded payloads re-inflate via readGenericSetFieldPayload",
          "[persistence][roundtrip]") {
    // The whole point of saving encodeSnapshot bytes is that the
    // next process can round-trip them back into live component
    // state. This test proves that loop.
    (void)sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    REQUIRE(meta != nullptr);

    const sv::PersistedWorld src = makeTestWorld(2);
    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeWorldToBytes(src, bytes) == sv::WorldPersistenceStatus::Ok);

    sv::PersistedWorld decoded;
    REQUIRE(sv::decodeWorldFromBytes(bytes.data(), bytes.size(), decoded)
            == sv::WorldPersistenceStatus::Ok);

    for (size_t i = 0; i < decoded.entities.size(); ++i) {
        sv::NetTransform t;
        sv::DirtyMask mask(meta->fields.size());
        REQUIRE(sv::readGenericSetFieldPayload(
            decoded.entities[i].typeNameHash,
            decoded.entities[i].payload.data(),
            decoded.entities[i].payload.size(),
            &t,
            mask));
        // The encoder used indices 0,1,2 → posX should be 1+i.
        REQUIRE(t.posX == Approx(1.0f + static_cast<float>(i)));
        REQUIRE(t.posZ == Approx(-2.0f - static_cast<float>(i)));
        REQUIRE(t.posY == Approx(50.0f));
        REQUIRE(t.rotW == Approx(1.0f));
    }
}

// ── File I/O round-trip ───────────────────────────────────────────

TEST_CASE("WorldPersistence: save + load file round-trip", "[persistence][file]") {
    CoreTempDir tmp;
    const std::string path = tmp.filePath("world.svbin");
    const sv::PersistedWorld src = makeTestWorld(2);

    REQUIRE(sv::saveWorldToFile(path, src) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(std::filesystem::exists(path));

    sv::PersistedWorld loaded;
    REQUIRE(sv::loadWorldFromFile(path, loaded) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(loaded.entities.size() == 2);
    REQUIRE(loaded.nextEntityId == src.nextEntityId);
    REQUIRE(loaded.nextClientId == src.nextClientId);
    REQUIRE(loaded.nextTxId     == src.nextTxId);
    REQUIRE(loaded.entities[0].label == "Ent0");
    REQUIRE(loaded.entities[1].label == "Ent1");
}

TEST_CASE("WorldPersistence: save overwrites existing file (atomic swap)",
          "[persistence][file]") {
    CoreTempDir tmp;
    const std::string path = tmp.filePath("world.svbin");

    // Write v1 with one entity
    const sv::PersistedWorld v1 = makeTestWorld(1);
    REQUIRE(sv::saveWorldToFile(path, v1) == sv::WorldPersistenceStatus::Ok);

    // Overwrite with v2 containing three entities
    const sv::PersistedWorld v2 = makeTestWorld(3);
    REQUIRE(sv::saveWorldToFile(path, v2) == sv::WorldPersistenceStatus::Ok);

    sv::PersistedWorld loaded;
    REQUIRE(sv::loadWorldFromFile(path, loaded) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(loaded.entities.size() == 3);
}

// ── Error paths ───────────────────────────────────────────────────

TEST_CASE("WorldPersistence: load returns MissingFile for non-existent path",
          "[persistence][file][error]") {
    CoreTempDir tmp;
    const std::string path = tmp.filePath("never_existed.svbin");
    sv::PersistedWorld loaded;
    REQUIRE(sv::loadWorldFromFile(path, loaded) == sv::WorldPersistenceStatus::MissingFile);
    REQUIRE(loaded.entities.empty());
}

TEST_CASE("WorldPersistence: corrupt magic header is rejected",
          "[persistence][file][error]") {
    CoreTempDir tmp;
    const std::string path = tmp.filePath("corrupt.svbin");

    // Write junk bytes with the right size but wrong magic.
    FILE* fp = nullptr;
#if defined(_WIN32)
    fopen_s(&fp, path.c_str(), "wb");
#else
    fp = std::fopen(path.c_str(), "wb");
#endif
    REQUIRE(fp != nullptr);
    const uint8_t junk[sv::kWorldFileHeaderSize] = {'X','X','X','X','X','X','X','X'};
    std::fwrite(junk, 1, sizeof(junk), fp);
    std::fclose(fp);

    sv::PersistedWorld loaded;
    REQUIRE(sv::loadWorldFromFile(path, loaded) == sv::WorldPersistenceStatus::CorruptHeader);
    REQUIRE(loaded.entities.empty());
}

TEST_CASE("WorldPersistence: unsupported version is rejected",
          "[persistence][file][error]") {
    CoreTempDir tmp;
    const std::string path = tmp.filePath("bad_version.svbin");

    // Build a well-formed header for an empty world, then poke the
    // version field to something other than 1.
    sv::PersistedWorld empty;
    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeWorldToBytes(empty, bytes) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(bytes.size() == sv::kWorldFileHeaderSize);
    // Version lives at offset 8 (after the 8-byte magic).
    bytes[8]  = 0xFF;
    bytes[9]  = 0xFF;
    bytes[10] = 0x00;
    bytes[11] = 0x00;

    FILE* fp = nullptr;
#if defined(_WIN32)
    fopen_s(&fp, path.c_str(), "wb");
#else
    fp = std::fopen(path.c_str(), "wb");
#endif
    REQUIRE(fp != nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);

    sv::PersistedWorld loaded;
    REQUIRE(sv::loadWorldFromFile(path, loaded) == sv::WorldPersistenceStatus::UnsupportedVersion);
}

TEST_CASE("WorldPersistence: decodeFromBytes rejects short buffer",
          "[persistence][roundtrip][error]") {
    sv::PersistedWorld loaded;
    uint8_t tiny[8] = {0};
    REQUIRE(sv::decodeWorldFromBytes(tiny, sizeof(tiny), loaded)
            == sv::WorldPersistenceStatus::CorruptHeader);
    REQUIRE(sv::decodeWorldFromBytes(nullptr, 100, loaded)
            == sv::WorldPersistenceStatus::CorruptHeader);
}

TEST_CASE("WorldPersistence: decodeFromBytes rejects unknown typeNameHash",
          "[persistence][roundtrip][error]") {
    // Build a world with a fake typeNameHash that is NOT in the
    // replication registry — the loader should refuse it.
    sv::PersistedWorld w;
    sv::PersistedEntity e;
    e.entityId      = 1;
    e.authority     = 0;
    e.ownerClientId = 0;
    e.typeNameHash  = 0xFEEDFACEu;     // guaranteed unregistered
    e.label         = "Ghost";
    // Empty payload is fine — the loader rejects before any decode.
    w.entities.push_back(std::move(e));

    std::vector<uint8_t> bytes;
    REQUIRE(sv::encodeWorldToBytes(w, bytes) == sv::WorldPersistenceStatus::Ok);

    sv::PersistedWorld loaded;
    REQUIRE(sv::decodeWorldFromBytes(bytes.data(), bytes.size(), loaded)
            == sv::WorldPersistenceStatus::UnknownType);
}

TEST_CASE("WorldPersistence: save creates parent directories on demand",
          "[persistence][file]") {
    CoreTempDir tmp;
    const std::string path = (tmp.path / "nested" / "deep" / "world.svbin").string();

    const sv::PersistedWorld src = makeTestWorld(1);
    REQUIRE(sv::saveWorldToFile(path, src) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(std::filesystem::exists(path));

    sv::PersistedWorld loaded;
    REQUIRE(sv::loadWorldFromFile(path, loaded) == sv::WorldPersistenceStatus::Ok);
    REQUIRE(loaded.entities.size() == 1);
    REQUIRE(loaded.entities[0].label == "Ent0");
}
