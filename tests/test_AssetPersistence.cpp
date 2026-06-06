// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetPersistence + asset sync wire tests ────────────
// Lives in sv_core_tests so it runs on both the full Windows build
// and the Linux-or-Windows core-only carve-out. Tagged [assetsync]
// with sub-tags for [sha256], [hex], [chunkcount], [announce],
// [chunk], [ack], [receiver], [store], [disk].

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AssetPersistence.h"
#include "AssetUploadClient.h"
#include "Sha256.h"
#include "net/ReplicationProtocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Unique temp directory per test case. Duplicates the CoreTempDir
// pattern from tests/test_WorldPersistence.cpp because the core
// test binary must not pull graphics-adjacent headers from
// tests/test_util.h. The small duplication keeps the boundary
// probe (test_StratumVCore.cpp) working on both build flavors.
struct CoreTempDir {
    std::filesystem::path path;

    CoreTempDir() {
        namespace fs = std::filesystem;
        static std::atomic<uint64_t> counter{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() / "stratumv_asset_tests"
             / ("t_" + std::to_string(now) + "_" + std::to_string(n));
        fs::create_directories(path);
    }
    ~CoreTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    CoreTempDir(const CoreTempDir&) = delete;
    CoreTempDir& operator=(const CoreTempDir&) = delete;

    std::string str() const { return path.string(); }
};

// Helper: deterministic filler bytes for a given byte count.
std::vector<uint8_t> makeFiller(size_t n, uint8_t seed) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>((seed + i * 31) & 0xFF);
    }
    return out;
}

} // namespace

// ── SHA-256 test vectors ────────────────────────────────────────────
// FIPS 180-4 appendix B.1 (one-block and two-block test vectors).

TEST_CASE("SHA-256: empty input matches known digest",
          "[assetsync][sha256]") {
    const auto d = sv::sha256(nullptr, 0);
    const std::string hex = sv::digestToHex(d);
    REQUIRE(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("SHA-256: 'abc' matches FIPS 180-4 appendix B.1",
          "[assetsync][sha256]") {
    const char* msg = "abc";
    const auto d = sv::sha256(reinterpret_cast<const uint8_t*>(msg), 3);
    const std::string hex = sv::digestToHex(d);
    REQUIRE(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("SHA-256: two-block test vector matches FIPS 180-4 appendix B.2",
          "[assetsync][sha256]") {
    const char* msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const size_t len = std::strlen(msg);
    REQUIRE(len == 56);
    const auto d = sv::sha256(reinterpret_cast<const uint8_t*>(msg), len);
    REQUIRE(sv::digestToHex(d) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256: incremental hasher matches one-shot result",
          "[assetsync][sha256]") {
    const std::vector<uint8_t> data = makeFiller(200 * 1024, 0xA5);

    const auto oneShot = sv::sha256(data.data(), data.size());

    sv::Sha256Hasher h;
    // Feed in varied-size chunks to exercise both the tail-buffer
    // and full-block paths.
    size_t pos = 0;
    const size_t chunkSizes[] = {1, 63, 64, 65, 1000, 32768};
    for (size_t cs : chunkSizes) {
        const size_t take = std::min(cs, data.size() - pos);
        h.update(data.data() + pos, take);
        pos += take;
    }
    h.update(data.data() + pos, data.size() - pos);
    const auto incremental = h.finalize();
    REQUIRE(incremental == oneShot);

    // finalize() is idempotent — second call returns the same bytes.
    REQUIRE(h.finalize() == oneShot);
}

// ── Hex round-trip ──────────────────────────────────────────────────

TEST_CASE("digestToHex / digestFromHex round-trip",
          "[assetsync][hex]") {
    sv::AssetHash src{};
    for (size_t i = 0; i < 32; ++i) src[i] = static_cast<uint8_t>(i * 7 + 3);
    const std::string hex = sv::digestToHex(src);
    REQUIRE(hex.size() == 64);
    sv::AssetHash back{};
    REQUIRE(sv::digestFromHex(hex, back));
    REQUIRE(back == src);
}

TEST_CASE("digestFromHex: rejects short / long / non-hex input",
          "[assetsync][hex][error]") {
    sv::AssetHash out{};
    REQUIRE_FALSE(sv::digestFromHex("short", out));
    REQUIRE_FALSE(sv::digestFromHex(std::string(63, '0'), out));
    REQUIRE_FALSE(sv::digestFromHex(std::string(65, '0'), out));
    // Valid length but invalid characters.
    std::string bad(64, '0');
    bad[5] = 'x';
    REQUIRE_FALSE(sv::digestFromHex(bad, out));
    // Case-insensitive accept.
    std::string mixed = "ABCDEF0123456789abcdef0123456789abcdef0123456789ABCDEF0123456789";
    REQUIRE(mixed.size() == 64);
    REQUIRE(sv::digestFromHex(mixed, out));
}

// ── Chunk arithmetic ────────────────────────────────────────────────

TEST_CASE("assetChunkCount handles zero / empty / tail sizes",
          "[assetsync][chunkcount]") {
    // chunkSize 0 is a caller error — returns 0.
    REQUIRE(sv::assetChunkCount(1024, 0) == 0);
    // Empty asset rounds up to one zero-byte chunk.
    REQUIRE(sv::assetChunkCount(0, 64) == 1);
    // Exact multiple.
    REQUIRE(sv::assetChunkCount(256, 64) == 4);
    // Tail remainder.
    REQUIRE(sv::assetChunkCount(257, 64) == 5);
    REQUIRE(sv::assetChunkCount(63, 64) == 1);
    REQUIRE(sv::assetChunkCount(65, 64) == 2);
    // Typical 150 KiB asset at the 64 KiB default chunk size.
    REQUIRE(sv::assetChunkCount(150 * 1024,
                                sv::net::kAssetChunkSize) == 3);
}

// ── Announce encode / parse ─────────────────────────────────────────

TEST_CASE("encodeAssetAnnounce + parseAssetAnnounce round-trip",
          "[assetsync][announce][roundtrip]") {
    sv::net::AssetAnnounceMessage src;
    for (size_t i = 0; i < 32; ++i) src.hash[i] = static_cast<uint8_t>(i + 1);
    src.byteSize  = 150 * 1024;
    src.assetKind = 2;
    src.name      = "textures/stone.png";

    std::vector<uint8_t> bytes;
    REQUIRE(sv::net::encodeAssetAnnounce(src, bytes));
    REQUIRE(bytes.size() == sv::net::kAssetAnnounceHeader + src.name.size());
    REQUIRE(bytes[0] == sv::net::kFrameAssetAnnounce);

    sv::net::AssetAnnounceMessage dst;
    REQUIRE(sv::net::parseAssetAnnounce(bytes.data(), bytes.size(), dst));
    REQUIRE(dst.hash      == src.hash);
    REQUIRE(dst.byteSize  == src.byteSize);
    REQUIRE(dst.assetKind == src.assetKind);
    REQUIRE(dst.name      == src.name);
}

TEST_CASE("parseAssetAnnounce rejects short / wrong msgType",
          "[assetsync][announce][error]") {
    sv::net::AssetAnnounceMessage out;
    // Empty buffer.
    REQUIRE_FALSE(sv::net::parseAssetAnnounce(nullptr, 0, out));
    // Short header.
    std::vector<uint8_t> shortBuf(sv::net::kAssetAnnounceHeader - 1, 0);
    shortBuf[0] = sv::net::kFrameAssetAnnounce;
    REQUIRE_FALSE(sv::net::parseAssetAnnounce(shortBuf.data(),
                                              shortBuf.size(), out));
    // Wrong msgType byte.
    std::vector<uint8_t> wrongType(sv::net::kAssetAnnounceHeader, 0);
    wrongType[0] = sv::net::kFrameSnapshot;
    REQUIRE_FALSE(sv::net::parseAssetAnnounce(wrongType.data(),
                                              wrongType.size(), out));
    // Declared name length overruns buffer.
    std::vector<uint8_t> overrun(sv::net::kAssetAnnounceHeader, 0);
    overrun[0] = sv::net::kFrameAssetAnnounce;
    // nameLen = 100 at offset 38, but we haven't appended the bytes.
    overrun[38] = 100;
    overrun[39] = 0;
    REQUIRE_FALSE(sv::net::parseAssetAnnounce(overrun.data(),
                                              overrun.size(), out));
}

// ── Chunk encode / parse ────────────────────────────────────────────

TEST_CASE("encodeAssetChunk + parseAssetChunk round-trip",
          "[assetsync][chunk][roundtrip]") {
    std::vector<uint8_t> payload = makeFiller(1024, 0x11);
    sv::net::AssetChunkMessage src;
    for (size_t i = 0; i < 32; ++i) src.hash[i] = static_cast<uint8_t>(i * 3 + 5);
    src.chunkIndex = 1;
    src.chunkCount = 4;
    src.chunkLen   = static_cast<uint32_t>(payload.size());
    src.chunk      = payload.data();

    std::vector<uint8_t> bytes;
    REQUIRE(sv::net::encodeAssetChunk(src, bytes));
    REQUIRE(bytes.size() == sv::net::kAssetChunkHeaderSize + payload.size());
    REQUIRE(bytes[0] == sv::net::kFrameAssetChunk);

    sv::net::AssetChunkMessage dst;
    REQUIRE(sv::net::parseAssetChunk(bytes.data(), bytes.size(), dst));
    REQUIRE(dst.hash       == src.hash);
    REQUIRE(dst.chunkIndex == 1);
    REQUIRE(dst.chunkCount == 4);
    REQUIRE(dst.chunkLen   == payload.size());
    REQUIRE(dst.chunk      != nullptr);
    REQUIRE(std::memcmp(dst.chunk, payload.data(), payload.size()) == 0);
}

TEST_CASE("parseAssetChunk rejects malformed inputs",
          "[assetsync][chunk][error]") {
    sv::net::AssetChunkMessage out;
    REQUIRE_FALSE(sv::net::parseAssetChunk(nullptr, 0, out));

    // Wrong msgType byte.
    std::vector<uint8_t> badType(sv::net::kAssetChunkHeaderSize, 0);
    badType[0] = sv::net::kFrameAssetAnnounce;
    REQUIRE_FALSE(sv::net::parseAssetChunk(badType.data(), badType.size(), out));

    // Declared chunkLen > available bytes.
    std::vector<uint8_t> overrun(sv::net::kAssetChunkHeaderSize, 0);
    overrun[0]  = sv::net::kFrameAssetChunk;
    overrun[33] = 0;     // chunkIndex = 0
    overrun[37] = 1;     // chunkCount = 1
    overrun[41] = 200;   // chunkLen = 200, no payload bytes follow
    REQUIRE_FALSE(sv::net::parseAssetChunk(overrun.data(),
                                           overrun.size(), out));

    // chunkIndex >= chunkCount.
    std::vector<uint8_t> outOfRange(sv::net::kAssetChunkHeaderSize, 0);
    outOfRange[0]  = sv::net::kFrameAssetChunk;
    outOfRange[33] = 5;  // chunkIndex = 5
    outOfRange[37] = 3;  // chunkCount = 3
    outOfRange[41] = 0;  // chunkLen = 0
    REQUIRE_FALSE(sv::net::parseAssetChunk(outOfRange.data(),
                                           outOfRange.size(), out));
}

// ── Ack encode / parse ──────────────────────────────────────────────

TEST_CASE("encodeAssetAck + parseAssetAck round-trip both statuses",
          "[assetsync][ack][roundtrip]") {
    for (auto st : {sv::net::AssetAckStatus::NeedChunks,
                    sv::net::AssetAckStatus::HaveIt}) {
        sv::net::AssetAckMessage src;
        for (size_t i = 0; i < 32; ++i) src.hash[i] = static_cast<uint8_t>(i);
        src.status = st;

        std::vector<uint8_t> bytes;
        REQUIRE(sv::net::encodeAssetAck(src, bytes));
        REQUIRE(bytes.size() == sv::net::kAssetAckSize);
        REQUIRE(bytes[0] == sv::net::kFrameAssetAck);

        sv::net::AssetAckMessage dst;
        REQUIRE(sv::net::parseAssetAck(bytes.data(), bytes.size(), dst));
        REQUIRE(dst.hash   == src.hash);
        REQUIRE(dst.status == st);
    }
}

TEST_CASE("parseAssetAck: unknown status downgrades to NeedChunks",
          "[assetsync][ack][error]") {
    std::vector<uint8_t> bytes(sv::net::kAssetAckSize, 0);
    bytes[0]  = sv::net::kFrameAssetAck;
    bytes[33] = 99;  // unknown status byte
    sv::net::AssetAckMessage out;
    REQUIRE(sv::net::parseAssetAck(bytes.data(), bytes.size(), out));
    REQUIRE(out.status == sv::net::AssetAckStatus::NeedChunks);
}

// ── Upload client chunk slicer ──────────────────────────────────────

TEST_CASE("buildAssetChunks slices a 150 KiB asset into 3 chunks",
          "[assetsync][upload]") {
    const size_t assetSize = 150 * 1024;
    std::vector<uint8_t> bytes = makeFiller(assetSize, 0x33);

    sv::AssetUploadRequest req;
    req.hash      = sv::sha256(bytes.data(), bytes.size());
    req.byteSize  = static_cast<uint32_t>(bytes.size());
    req.assetKind = 2;
    req.name      = "demo/150k.bin";
    req.bytes     = bytes.data();
    req.chunkSize = sv::net::kAssetChunkSize;

    std::vector<uint8_t> ann;
    REQUIRE(sv::buildAssetAnnounce(req, ann));
    REQUIRE(ann.size() == sv::net::kAssetAnnounceHeader + req.name.size());

    std::vector<std::vector<uint8_t>> chunks;
    REQUIRE(sv::buildAssetChunks(req, chunks));
    REQUIRE(chunks.size() == 3);
    // First two full 64 KiB chunks + one 22 KiB tail.
    REQUIRE(chunks[0].size() ==
            sv::net::kAssetChunkHeaderSize + sv::net::kAssetChunkSize);
    REQUIRE(chunks[1].size() ==
            sv::net::kAssetChunkHeaderSize + sv::net::kAssetChunkSize);
    REQUIRE(chunks[2].size() ==
            sv::net::kAssetChunkHeaderSize + (assetSize - 2 * sv::net::kAssetChunkSize));
}

// ── AssetReceiver assembly ──────────────────────────────────────────

TEST_CASE("AssetReceiver assembles Announce + Chunks + verifies hash",
          "[assetsync][receiver]") {
    const size_t assetSize = 4 * 1024 + 7;
    std::vector<uint8_t> bytes = makeFiller(assetSize, 0x77);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    sv::AssetUploadRequest req;
    req.hash      = hash;
    req.byteSize  = static_cast<uint32_t>(bytes.size());
    req.assetKind = 1;
    req.name      = "demo/small.bin";
    req.bytes     = bytes.data();
    req.chunkSize = 1024;

    std::vector<std::vector<uint8_t>> chunkBytes;
    REQUIRE(sv::buildAssetChunks(req, chunkBytes));
    REQUIRE(chunkBytes.size() == sv::assetChunkCount(req.byteSize, req.chunkSize));

    sv::AssetReceiver rx;
    rx.beginFromAnnounce(req.hash, req.byteSize, req.assetKind, req.name,
                         static_cast<uint32_t>(chunkBytes.size()), req.chunkSize);
    REQUIRE_FALSE(rx.complete);
    for (const auto& cb : chunkBytes) {
        sv::net::AssetChunkMessage parsed;
        REQUIRE(sv::net::parseAssetChunk(cb.data(), cb.size(), parsed));
        REQUIRE(rx.depositChunk(parsed.chunkIndex, parsed.chunk, parsed.chunkLen));
    }
    REQUIRE(rx.complete);
    REQUIRE(rx.verifyHash());
    REQUIRE(rx.assembled.size() == req.byteSize);
    REQUIRE(std::memcmp(rx.assembled.data(), bytes.data(), bytes.size()) == 0);
}

TEST_CASE("AssetReceiver rejects double-deposit of same chunk",
          "[assetsync][receiver][error]") {
    std::vector<uint8_t> bytes = makeFiller(128, 0x00);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    sv::AssetReceiver rx;
    rx.beginFromAnnounce(hash, 128, 0, "rej", 2, 64);
    REQUIRE(rx.depositChunk(0, bytes.data(), 64));
    // Second deposit of chunk 0 must fail.
    REQUIRE_FALSE(rx.depositChunk(0, bytes.data(), 64));
    // Valid second chunk still works.
    REQUIRE(rx.depositChunk(1, bytes.data() + 64, 64));
    REQUIRE(rx.complete);
    REQUIRE(rx.verifyHash());
}

TEST_CASE("AssetReceiver rejects out-of-range index + wrong length",
          "[assetsync][receiver][error]") {
    std::vector<uint8_t> bytes = makeFiller(128, 0x00);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    sv::AssetReceiver rx;
    rx.beginFromAnnounce(hash, 128, 0, "rej", 2, 64);
    // Out of range.
    REQUIRE_FALSE(rx.depositChunk(5, bytes.data(), 64));
    // Wrong length for chunk 0 (expected 64).
    REQUIRE_FALSE(rx.depositChunk(0, bytes.data(), 10));
    // After failures receiver is still consistent.
    REQUIRE_FALSE(rx.complete);
}

TEST_CASE("AssetReceiver verifyHash fails on tampered bytes",
          "[assetsync][receiver][error]") {
    std::vector<uint8_t> bytes = makeFiller(256, 0xAB);
    // Compute hash, then mutate bytes so the deposit produces a
    // different assembled buffer than the announce says.
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());
    bytes[0] ^= 0xFF;

    sv::AssetReceiver rx;
    rx.beginFromAnnounce(hash, 256, 0, "rej", 4, 64);
    for (uint32_t i = 0; i < 4; ++i) {
        REQUIRE(rx.depositChunk(i, bytes.data() + i * 64, 64));
    }
    REQUIRE(rx.complete);
    REQUIRE_FALSE(rx.verifyHash());
}

// ── AssetPersistence in-memory ──────────────────────────────────────

TEST_CASE("AssetPersistence: save + find + load (in-memory only)",
          "[assetsync][store]") {
    sv::AssetPersistence store;
    std::vector<uint8_t> bytes = makeFiller(2048, 0xCD);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    REQUIRE_FALSE(store.contains(hash));
    REQUIRE(store.find(hash) == nullptr);
    REQUIRE(store.save(hash, 2, "tex.png",
                       bytes.data(), bytes.size()) ==
            sv::AssetPersistenceStatus::Ok);
    REQUIRE(store.contains(hash));
    REQUIRE(store.size() == 1);

    const sv::AssetRecord* rec = store.find(hash);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->byteSize == 2048);
    REQUIRE(rec->assetKind == 2);
    REQUIRE(rec->name == "tex.png");
    REQUIRE(rec->bytes.size() == bytes.size());

    sv::AssetRecord loaded;
    REQUIRE(store.load(hash, loaded) == sv::AssetPersistenceStatus::Ok);
    REQUIRE(loaded.hash == hash);
    REQUIRE(loaded.byteSize == 2048);
    REQUIRE(std::memcmp(loaded.bytes.data(), bytes.data(), bytes.size()) == 0);
}

TEST_CASE("AssetPersistence: save is a no-op on dedup hit",
          "[assetsync][store][dedup]") {
    sv::AssetPersistence store;
    std::vector<uint8_t> bytes = makeFiller(512, 0x20);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    REQUIRE(store.save(hash, 2, "a.bin", bytes.data(), bytes.size()) ==
            sv::AssetPersistenceStatus::Ok);
    REQUIRE(store.size() == 1);
    // Second save with the same (hash, bytes) is the dedup hot path —
    // still returns Ok but doesn't grow the map.
    REQUIRE(store.save(hash, 2, "a.bin", bytes.data(), bytes.size()) ==
            sv::AssetPersistenceStatus::Ok);
    REQUIRE(store.size() == 1);
}

TEST_CASE("AssetPersistence: hash mismatch rejected",
          "[assetsync][store][error]") {
    sv::AssetPersistence store;
    std::vector<uint8_t> bytes = makeFiller(512, 0x20);
    sv::AssetHash bogus{};   // all-zero hash
    REQUIRE(store.save(bogus, 2, "a.bin", bytes.data(), bytes.size()) ==
            sv::AssetPersistenceStatus::HashMismatch);
    REQUIRE(store.empty());
}

TEST_CASE("AssetPersistence: load on missing hash returns MissingFile",
          "[assetsync][store][error]") {
    sv::AssetPersistence store;
    sv::AssetHash hash{};
    sv::AssetRecord out;
    REQUIRE(store.load(hash, out) == sv::AssetPersistenceStatus::MissingFile);
}

TEST_CASE("AssetPersistence: setRootDir + disk round-trip",
          "[assetsync][store][disk]") {
    CoreTempDir dir;
    const std::string root = dir.str();

    std::vector<uint8_t> bytes = makeFiller(3072, 0x40);
    const sv::AssetHash hash = sv::sha256(bytes.data(), bytes.size());

    {
        sv::AssetPersistence store;
        REQUIRE(store.setRootDir(root) == sv::AssetPersistenceStatus::Ok);
        REQUIRE(store.save(hash, 2, "textures/t.png",
                           bytes.data(), bytes.size()) ==
                sv::AssetPersistenceStatus::Ok);
    }

    // On-disk path lives under root/<2hex>/<62hex>.bin + .meta.json
    const std::string binPath  = sv::assetFilePath(root, hash);
    const std::string metaPath = sv::assetMetaPath(root, hash);
    REQUIRE(std::filesystem::exists(binPath));
    REQUIRE(std::filesystem::exists(metaPath));

    // New store pointed at the same root should rehydrate the
    // in-memory cache from disk.
    sv::AssetPersistence reopened;
    REQUIRE(reopened.setRootDir(root) == sv::AssetPersistenceStatus::Ok);
    REQUIRE(reopened.size() == 1);
    const sv::AssetRecord* rec = reopened.find(hash);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->byteSize == 3072);
    REQUIRE(rec->name == "textures/t.png");
    REQUIRE(rec->assetKind == 2);
    REQUIRE(std::memcmp(rec->bytes.data(), bytes.data(), bytes.size()) == 0);
}

TEST_CASE("assetFilePath / assetMetaPath composition",
          "[assetsync][store][paths]") {
    sv::AssetHash hash{};
    for (size_t i = 0; i < 32; ++i) hash[i] = static_cast<uint8_t>(i);
    const std::string hex = sv::digestToHex(hash);
    REQUIRE(hex == "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    const std::string bin = sv::assetFilePath("C:/data", hash);
    const std::string meta = sv::assetMetaPath("C:/data", hash);
    REQUIRE(bin  == "C:/data/00/0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f.bin");
    REQUIRE(meta == "C:/data/00/0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f.meta.json");

    // Empty root -> empty string.
    REQUIRE(sv::assetFilePath("", hash).empty());
    REQUIRE(sv::assetMetaPath("", hash).empty());
}

// ── Status diagnostics ─────────────────────────────────────────────

TEST_CASE("assetPersistenceStatusToString covers every enum value",
          "[assetsync][store][status]") {
    using S = sv::AssetPersistenceStatus;
    REQUIRE(std::string("Ok")            == sv::assetPersistenceStatusToString(S::Ok));
    REQUIRE(std::string("BadArg")        == sv::assetPersistenceStatusToString(S::BadArg));
    REQUIRE(std::string("HashMismatch")  == sv::assetPersistenceStatusToString(S::HashMismatch));
    REQUIRE(std::string("MissingFile")   == sv::assetPersistenceStatusToString(S::MissingFile));
    REQUIRE(std::string("IoError")       == sv::assetPersistenceStatusToString(S::IoError));
    REQUIRE(std::string("CorruptHeader") == sv::assetPersistenceStatusToString(S::CorruptHeader));
    REQUIRE(std::string("SizeExceeded")  == sv::assetPersistenceStatusToString(S::SizeExceeded));
}
