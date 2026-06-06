// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── AssetUploadClient implementation ───────────────────

#include "AssetUploadClient.h"

#include "EngineLog.h"
#include "Sha256.h"

#include <cstring>

namespace sv {

// ── Chunk arithmetic ──────────────────────────────────────────────

uint32_t assetChunkCount(uint32_t byteSize, uint32_t chunkSize) {
    if (chunkSize == 0) return 0;
    if (byteSize == 0)  return 1;    // one zero-byte chunk keeps Announce→Chunk→done symmetric
    return (byteSize + chunkSize - 1) / chunkSize;
}

// ── Announce builder ──────────────────────────────────────────────

bool buildAssetAnnounce(const AssetUploadRequest& req,
                        std::vector<uint8_t>&     out) {
    out.clear();
    if (req.chunkSize == 0) return false;
    if (req.byteSize > sv::net::kAssetByteLimit) return false;
    if (req.byteSize > 0 && !req.bytes) return false;

    sv::net::AssetAnnounceMessage msg;
    msg.hash      = req.hash;
    msg.byteSize  = req.byteSize;
    msg.assetKind = req.assetKind;
    msg.name      = req.name;
    return sv::net::encodeAssetAnnounce(msg, out);
}

// ── Chunk slicer ──────────────────────────────────────────────────

bool buildAssetChunks(const AssetUploadRequest&          req,
                      std::vector<std::vector<uint8_t>>& outChunks) {
    outChunks.clear();
    if (req.chunkSize == 0) return false;
    if (req.byteSize > sv::net::kAssetByteLimit) return false;
    if (req.byteSize > 0 && !req.bytes) return false;

    const uint32_t chunkCount = assetChunkCount(req.byteSize, req.chunkSize);
    if (chunkCount == 0) return false;
    outChunks.reserve(chunkCount);

    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint64_t offset   = static_cast<uint64_t>(i) * req.chunkSize;
        const uint64_t remaining = req.byteSize - offset;
        const uint32_t thisLen  = static_cast<uint32_t>(
            remaining < req.chunkSize ? remaining : req.chunkSize);

        sv::net::AssetChunkMessage msg;
        msg.hash       = req.hash;
        msg.chunkIndex = i;
        msg.chunkCount = chunkCount;
        msg.chunkLen   = thisLen;
        msg.chunk      = (thisLen > 0) ? (req.bytes + offset) : nullptr;

        std::vector<uint8_t> bytes;
        if (!sv::net::encodeAssetChunk(msg, bytes)) {
            outChunks.clear();
            return false;
        }
        outChunks.push_back(std::move(bytes));
    }
    return true;
}

// ── AssetReceiver ─────────────────────────────────────────────────

void AssetReceiver::beginFromAnnounce(const AssetHash&   h,
                                      uint32_t           size,
                                      uint8_t            kind,
                                      const std::string& n,
                                      uint32_t           cc,
                                      uint32_t           cs) {
    hash          = h;
    byteSize      = size;
    assetKind     = kind;
    name          = n;
    chunkCount    = cc;
    chunkSize     = cs;
    receivedCount = 0;
    complete      = false;
    assembled.clear();
    assembled.resize(byteSize, 0);
    chunkReceived.assign(chunkCount, false);
}

bool AssetReceiver::depositChunk(uint32_t       chunkIndex,
                                 const uint8_t* bytes,
                                 size_t         len) {
    if (chunkCount == 0)            return false;
    if (chunkIndex >= chunkCount)   return false;
    if (complete)                   return false;
    if (chunkReceived[chunkIndex])  return false;   // double deposit
    if (len > 0 && !bytes)          return false;

    const uint64_t offset    = static_cast<uint64_t>(chunkIndex) * chunkSize;
    const uint64_t remaining = byteSize - offset;
    // Expected length: full chunkSize for all but the last chunk,
    // tail-length for the last chunk (or 0 for a zero-byte asset).
    const uint32_t expected =
        (chunkIndex + 1 == chunkCount)
            ? static_cast<uint32_t>(remaining)
            : chunkSize;
    if (len != expected) return false;

    if (len > 0) {
        std::memcpy(assembled.data() + offset, bytes, len);
    }
    chunkReceived[chunkIndex] = true;
    ++receivedCount;
    if (receivedCount == chunkCount) {
        complete = true;
    }
    return true;
}

bool AssetReceiver::verifyHash() const {
    if (!complete) return false;
    if (assembled.size() != byteSize) return false;
    const AssetHash actual = sha256(assembled.data(), assembled.size());
    return actual == hash;
}

void AssetReceiver::clear() {
    hash          = AssetHash{};
    byteSize      = 0;
    assetKind     = 0;
    name.clear();
    chunkCount    = 0;
    chunkSize     = 0;
    receivedCount = 0;
    complete      = false;
    assembled.clear();
    chunkReceived.clear();
}

} // namespace sv
