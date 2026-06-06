// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── AssetUploadClient ────────────────────────────────────
// Client-side upload helper + receiver assembler for collaborative
// asset sync. Pure logic — no MsQuic dependency, no Vulkan — lives
// in the core subset so both the server (for broadcasting to other
// clients) and every client engine can share the same assembly
// state machine.
//
// The split between "upload client" (sender state) and
// "receive assembler" (receiver state) is done as two lightweight
// structs in this single TU because the life cycles are identical
// and both sides need the same chunk splitter + the same hash
// verification.
//
// ── Sender path ────────────────────────────────────────────────────
//
//   AssetUploadRequest req{};
//   req.byteSize   = ...;
//   req.assetKind  = ...;
//   req.name       = "textures/stone.png";
//   req.bytes      = fileBytes.data();
//   req.chunkSize  = kAssetChunkSize;  // 64 KiB default
//   req.hash       = hashAssetBytes(req.bytes, req.byteSize);
//
//   std::vector<uint8_t> announceWire;
//   buildAssetAnnounce(req, announceWire);
//   connection.sendReliableMessage(announceWire.data(), announceWire.size());
//
//   // Wait for AssetAck from server; if status == NeedChunks:
//   std::vector<std::vector<uint8_t>> chunks;
//   buildAssetChunks(req, chunks);
//   for (auto& c : chunks) {
//       connection.sendReliableMessage(c.data(), c.size());
//   }
//
// ── Receiver path ──────────────────────────────────────────────────
//
//   AssetReceiver rx;
//   // On AssetAnnounce arrival:
//   rx.beginFromAnnounce(announce.hash, announce.byteSize,
//                        announce.assetKind, announce.name,
//                        chunkSize);
//   // On each AssetChunk arrival:
//   rx.depositChunk(chunk.chunkIndex, chunk.chunk, chunk.chunkLen);
//   if (rx.complete && rx.verifyHash()) {
//       // hand rx.assembled over to the local AssetPersistence cache
//   }
//
// Either side can use either helper — the server uses AssetReceiver
// to assemble uploads before persisting, and uses buildAssetChunks
// to slice cached bytes when broadcasting to other clients.

#include "AssetPersistence.h"
#include "net/ReplicationProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sv {

// ── AssetUploadRequest ────────────────────────────────────────────
// All the information needed to turn a contiguous byte buffer into
// wire messages. `bytes` must outlive the enclosing
// buildAssetChunks call; the chunk builder copies into the output
// vectors.
struct AssetUploadRequest {
    AssetHash      hash{};
    uint32_t       byteSize  = 0;
    uint8_t        assetKind = 0;
    std::string    name;
    const uint8_t* bytes     = nullptr;
    uint32_t       chunkSize = sv::net::kAssetChunkSize;
};

// Compute the chunk count for the given size/chunkSize pair.
// Returns 1 for an empty asset (a single zero-byte chunk keeps the
// wire flow symmetric — Announce → one Chunk(len=0) → done).
// Returns 0 if chunkSize is 0 (caller error).
uint32_t assetChunkCount(uint32_t byteSize, uint32_t chunkSize);

// Build an AssetAnnounceMessage wire buffer from the request.
// Returns false if req.name is too long, or if the request is
// otherwise malformed.
bool buildAssetAnnounce(const AssetUploadRequest& req,
                        std::vector<uint8_t>&     out);

// Slice the request's bytes into chunk messages. `outChunks` is
// cleared and then populated with one ready-to-send reliable
// message per chunk. Returns false on malformed input (chunkSize=0,
// byteSize>limit, null bytes with nonzero size).
bool buildAssetChunks(const AssetUploadRequest&          req,
                      std::vector<std::vector<uint8_t>>& outChunks);

// ── AssetReceiver ─────────────────────────────────────────────────
// State machine that assembles incoming chunks into a contiguous
// byte buffer, tracks per-chunk receipt, and verifies the final
// hash against the Announce-declared hash.
//
// Lifecycle: `beginFromAnnounce` → one or more `depositChunk` →
// `complete` flips true when all chunks have arrived →
// `verifyHash` returns whether the bytes match. `clear` resets.
// A caller that wants to re-use the receiver for the next upload
// calls `beginFromAnnounce` again — it resets internal state.
struct AssetReceiver {
    AssetHash   hash{};
    uint32_t    byteSize      = 0;
    uint8_t     assetKind     = 0;
    std::string name;

    uint32_t    chunkCount    = 0;
    uint32_t    chunkSize     = 0;
    uint32_t    receivedCount = 0;

    std::vector<uint8_t> assembled;       // byteSize-sized buffer
    std::vector<bool>    chunkReceived;   // one bool per chunk

    bool        complete = false;

    // Start tracking a new upload.
    void beginFromAnnounce(const AssetHash&    hash,
                           uint32_t            byteSize,
                           uint8_t             assetKind,
                           const std::string&  name,
                           uint32_t            chunkCount,
                           uint32_t            chunkSize);

    // Deposit `bytes[0..len)` at slot `chunkIndex`. Returns true on
    // success. Refuses:
    //   - chunkIndex >= chunkCount
    //   - len mismatch with the expected chunkSize / tail size
    //   - double deposit of the same chunk
    //   - deposit after `complete` is already true
    bool depositChunk(uint32_t       chunkIndex,
                      const uint8_t* bytes,
                      size_t         len);

    // Compute SHA-256 over assembled bytes and compare against the
    // announced hash. Undefined if `complete` is false.
    bool verifyHash() const;

    // Reset to idle.
    void clear();
};

} // namespace sv
