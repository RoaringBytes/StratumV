// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ============================================================
// stratumv_server/main.cpp — StratumV dedicated server
// ============================================================
//
// Headless StratumV dedicated server entry point. Capabilities:
//
//   - Transport bring-up + graceful SIGINT exit
//   - 30 Hz fixed tick broadcasting an authoritative NetTransform
//     cube snapshot via QUIC datagrams
//   - One-shot schema handshake preamble on a reliable
//     unidirectional stream right after the TLS handshake
//   - Per-client avatar spawn/despawn + collaborative editing
//     transactions (SetField / Undo / Redo) on the reliable stream,
//     server-side UndoLog, per-client permission scopes
//     (default Editor)
//   - Generic SetField/Spawn payload dispatch through the
//     ReplicationRegistry (any SV_REPLICATE'd type flows),
//     join-with-snapshot replays every entity's CURRENT state
//     (not initial), and periodic + SIGINT-flush world persistence
//     via --server-data
//
// ── Entity layout ──────────────────────────────────────────────
//
//   entityId 1       — server-owned orbiting cube (Authority::Server,
//                      deterministic wall-clock motion)
//   entityId 100+    — per-connection avatars (Authority::Owner, one
//                      allocated on accept, despawned on disconnect,
//                      drivable via SetField transactions from the
//                      owning client)
//
// ── What this binary does NOT do ───────────────────────────────
// - No ECS, no physics, no game world beyond the entity map
// - No interest management (every datagram goes to every client)
// - No scale-B bandwidth budgeting
//
// ── Usage ──────────────────────────────────────────────────────
//
//   stratumv_server [--port N] [--idle-timeout-ms N]
//                   [--tick-hz N] [--orbit-radius F]
//                   [--server-data DIR] [--save-interval-sec N]
//                   [--help]
//
// Defaults: --port 9001 --idle-timeout-ms 60000 --tick-hz 30
//           --orbit-radius 100 --save-interval-sec 30
//
// --server-data DIR enables world persistence. If DIR/world.svbin
// exists at startup, the server rehydrates its entity map via
// decodeWorldFromBytes. Every --save-interval-sec seconds the tick
// loop flushes the current state back to disk via saveWorldToFile
// (atomic temp-rename). On SIGINT a final flush runs before
// Transport::stop. Omitting --server-data leaves the server running
// fully ephemeral.
//
// ── Return codes ───────────────────────────────────────────────
//   0 — clean shutdown on SIGINT
//   1 — MsQuic unavailable or Transport::start failed
//   2 — startListener failed (port in use, permission, etc.)
//   3 — CLI parse error

#include "AssetPersistence.h"
#include "AssetUploadClient.h"
#include "EditTransaction.h"
#include "CameraComponent.h"
#include "LightComponent.h"
#include "MaterialComponent.h"
#include "EngineLog.h"
#include "NetTransform.h"
#include "ParentLink.h"
#include "PermissionScope.h"
#include "ReplicationRegistry.h"
#include "StratumVVersion.h"
#include "UndoLog.h"
#include "WorldPersistence.h"
#include "net/MsQuicTransport.h"
#include "net/ReplicationProtocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// Polled by the main loop; set to false by the SIGINT handler.
// std::atomic<bool> is lock-free on every platform that matters,
// so it's safe to mutate from a signal handler.
std::atomic<bool> g_running{true};

extern "C" void onSigint(int /*signo*/) {
    g_running.store(false, std::memory_order_relaxed);
}

void printUsage() {
    std::fprintf(stdout,
        "stratumv_server - StratumV dedicated server\n"
        "\n"
        "Usage: stratumv_server [--port N] [--idle-timeout-ms N]\n"
        "                       [--tick-hz N] [--orbit-radius F]\n"
        "                       [--server-data DIR] [--save-interval-sec N]\n"
        "                       [--help]\n"
        "\n"
        "Options:\n"
        "  --port N              TCP/UDP port to bind (default 9001)\n"
        "  --idle-timeout-ms N   QUIC idle timeout (default 60000)\n"
        "  --tick-hz N           Server tick rate in Hz (default 30)\n"
        "  --orbit-radius F      Cube orbit radius (default 100.0)\n"
        "  --server-data DIR     World persistence directory (disables\n"
        "                        persistence if omitted)\n"
        "  --save-interval-sec N Autosave cadence (default 30)\n"
        "  --help, -h            Print this message and exit\n");
}

bool parseU32(const char* arg, uint32_t& out) {
    if (!arg || !*arg) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(arg, &end, 10);
    if (end == arg || *end != '\0') return false;
    if (v > 0xFFFFFFFFu) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool parsePort(const char* arg, uint16_t& out) {
    uint32_t v = 0;
    if (!parseU32(arg, v)) return false;
    if (v == 0 || v > 65535) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

bool parseFloat(const char* arg, float& out) {
    if (!arg || !*arg) return false;
    char* end = nullptr;
    float v = std::strtof(arg, &end);
    if (end == arg || *end != '\0') return false;
    out = v;
    return true;
}

// ── Wall-clock time helper ────────────────────────────────────────
// Returns monotonic milliseconds since process start. Used for
// transaction.timestampMs so clients can display relative ordering
// in their UI.
uint64_t monotonicMs() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now() - start).count());
}

// ── Server-owned cube state (entity 1) ──────────────────────────
// Wall-clock deterministic orbit — multiple clients connecting at
// different times see the same phase. Radius and period are tuned
// for visibility in the top-down XZ canvas of the lab harness.
void updateCubeTransform(sv::NetTransform& cube,
                         double              elapsedSec,
                         float               radius) {
    constexpr double kPeriodSec = 4.0;          // full orbit in 4 seconds
    const double phase = (elapsedSec / kPeriodSec) * (2.0 * 3.14159265358979);
    cube.posX = static_cast<float>(std::sin(phase) * radius);
    cube.posY = 50.0f +
                static_cast<float>(std::sin(phase * 2.0) * 10.0);
    cube.posZ = static_cast<float>(std::cos(phase) * radius);
    cube.rotX = 0.0f;
    cube.rotY = 0.0f;
    cube.rotZ = 0.0f;
    cube.rotW = 1.0f;
}

// ── Replicated entity record ────────────────────────────────────
// Server-side authoritative state for a single replicated entity.
// Carries a NetTransform plus a ParentLink sidecar and the light /
// camera / material component sidecars. More components slot in the
// same way — another sidecar field + another dispatch branch in
// applyClientTransaction. A fully generic per-entity component map
// is a future refactor.
struct ReplicatedEntity {
    uint32_t            entityId       = 0;
    sv::Authority       authority      = sv::Authority::Server;
    uint32_t            ownerClientId  = 0;   // only used when authority == Owner
    sv::NetTransform    transform      {};
    sv::ParentLink      parent         {};    // default unparented
    sv::LightComponent  light          {};    // default disabled (type=0, intensity=0)
    sv::CameraComponent camera         {};    // default fovDeg=0 = no override
    sv::MaterialComponent material     {};    // default strength=0 = no effect
    std::string         label;                 // diagnostic (e.g. "Client1")
    bool                alive          = true;
};

// ── Per-client connection state ─────────────────────────────────
// Lives inside a std::shared_ptr<ClientState> held in the server's
// clients vector. The shared_ptr indirection keeps the inbox mutex
// at a stable address while letting the MsQuic worker thread's
// capture go via std::weak_ptr, so the worker doesn't dangle when
// a client disconnects before the main loop gets to reap it.
struct ClientState {
    sv::net::Connection conn;
    uint32_t            clientId        = 0;
    uint32_t            avatarEntityId  = 0;
    sv::PermissionScope scope           = sv::PermissionScope::Editor;

    // Per-connection progress markers driving the connect
    // state-machine: preamble → welcome → world-catch-up → active.
    bool preambleSent = false;
    bool welcomeSent  = false;
    bool worldSynced  = false;
    bool dead         = false;   // flipped when shutdown seen or explicitly reaped

    // Inbound reliable messages. Filled from the MsQuic worker
    // thread via the setReliableMessageHandler lambda; drained on
    // the main thread at the top of every tick.
    std::mutex                        inboxMu;
    std::vector<std::vector<uint8_t>> inbox;

    // Per-client in-progress asset uploads. Keyed by the
    // hex hash of the announced asset. Each receiver holds an
    // assembled byte buffer + per-chunk receipt tracking. Entries
    // are dropped as soon as the assembly completes + persists.
    std::unordered_map<std::string, sv::AssetReceiver> pendingUploads;
};

// ── Server world state ──────────────────────────────────────────
struct ServerWorld {
    std::unordered_map<uint32_t, ReplicatedEntity> entities;
    uint32_t     nextEntityId = 100;   // 1 is reserved for the orbiting cube
    uint32_t     nextClientId = 1;     // 0 = server
    uint64_t     nextTxId     = 1;
    sv::UndoLog  undoLog;
};

// ── Wire-building helpers ───────────────────────────────────────
//
// Every transaction the server emits (Spawn, Despawn, SetField,
// Undo, Redo on broadcast) flows through here. The server stamps
// `txId` and `timestampMs` at build time so the wire log is
// monotonic across all clients.

sv::EditTransaction makeSpawnTransaction(ServerWorld&               world,
                                         const ReplicatedEntity&    ent,
                                         uint32_t                   originClientId,
                                         const sv::ReplicationMeta& meta) {
    // The Spawn payload is built via the generic dispatch path — a
    // 4-byte ownerClientId prefix followed by a full-mask
    // encodeSnapshot blob. Any SV_REPLICATE'd component type flows
    // here, but the spawn path currently uses NetTransform only so
    // meta always refers to it; adding a second component type would
    // route the per-entity spawn through the meta lookup for that
    // component.
    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::Spawn;
    tx.txId           = world.nextTxId++;
    tx.originClientId = originClientId;
    tx.requiredScope  = sv::PermissionScope::Admin;
    tx.entityId       = ent.entityId;
    tx.typeNameHash   = meta.typeNameHash;
    tx.timestampMs    = monotonicMs();
    if (!sv::writeGenericSpawnPayload(meta, &ent.transform,
                                      ent.ownerClientId, tx.payload)) {
        SV_LOG_WARN("Server",
            "writeGenericSpawnPayload failed for entity %u",
            static_cast<unsigned>(ent.entityId));
    }
    return tx;
}

sv::EditTransaction makeDespawnTransaction(ServerWorld&               world,
                                           uint32_t                    entityId,
                                           const sv::ReplicationMeta&  meta) {
    sv::EditTransaction tx;
    tx.kind           = sv::EditKind::Despawn;
    tx.txId           = world.nextTxId++;
    tx.originClientId = 0;
    tx.requiredScope  = sv::PermissionScope::Admin;
    tx.entityId       = entityId;
    tx.typeNameHash   = meta.typeNameHash;
    tx.timestampMs    = monotonicMs();
    return tx;
}

// Encode a transaction and push it on the given connection's
// reliable stream. Silently drops if the connection isn't ready;
// callers are expected to re-check on subsequent ticks.
void pushReliableTransaction(sv::net::Connection& conn,
                             const sv::EditTransaction& tx) {
    if (!conn.valid()) return;
    const auto stats = conn.stats();
    if (!stats.connected || stats.shutdownStarted) return;
    std::vector<uint8_t> bytes;
    if (!sv::encodeEditTransaction(tx, bytes)) return;
    conn.sendReliableMessage(bytes.data(), bytes.size());
}

// Broadcast a transaction to all connected clients that have
// passed the welcome step. The clients vector is shared_ptr so
// we don't invalidate iterators mid-loop.
void broadcastTransaction(const sv::EditTransaction& tx,
                          const std::vector<std::shared_ptr<ClientState>>& clients) {
    std::vector<uint8_t> bytes;
    if (!sv::encodeEditTransaction(tx, bytes)) return;
    for (const auto& cs : clients) {
        if (!cs || cs->dead || !cs->welcomeSent) continue;
        if (!cs->conn.valid()) continue;
        const auto stats = cs->conn.stats();
        if (!stats.connected || stats.shutdownStarted) continue;
        cs->conn.sendReliableMessage(bytes.data(), bytes.size());
    }
}

// ── Asset sync helpers ──────────────────────────────────────────
// Send an asset's cached Announce + Chunks on one connection.
// Used both for server-originated broadcasts to other clients and
// for the "dedup hit" path where a client uploads an asset that
// happens to already be cached server-side.
void pushAssetToConnection(sv::net::Connection&    conn,
                           const sv::AssetRecord&  rec) {
    if (!conn.valid()) return;
    const auto stats = conn.stats();
    if (!stats.connected || stats.shutdownStarted) return;

    sv::AssetUploadRequest req;
    req.hash      = rec.hash;
    req.byteSize  = rec.byteSize;
    req.assetKind = rec.assetKind;
    req.name      = rec.name;
    req.bytes     = rec.bytes.data();
    req.chunkSize = sv::net::kAssetChunkSize;

    std::vector<uint8_t> announceBytes;
    if (!sv::buildAssetAnnounce(req, announceBytes)) return;
    if (!conn.sendReliableMessage(announceBytes.data(), announceBytes.size())) {
        return;
    }

    std::vector<std::vector<uint8_t>> chunks;
    if (!sv::buildAssetChunks(req, chunks)) return;
    for (const auto& c : chunks) {
        if (!conn.sendReliableMessage(c.data(), c.size())) break;
    }
}

// Broadcast a cached asset to every welcomed client except `origin`.
void broadcastAsset(const sv::AssetRecord& rec,
                    const std::shared_ptr<ClientState>& origin,
                    const std::vector<std::shared_ptr<ClientState>>& clients) {
    for (const auto& cs : clients) {
        if (!cs || cs.get() == origin.get()) continue;
        if (cs->dead || !cs->welcomeSent) continue;
        pushAssetToConnection(cs->conn, rec);
    }
}

// Reply to an uploading client with an Ack(hash, status).
void sendAssetAck(sv::net::Connection&        conn,
                  const sv::AssetHash&        hash,
                  sv::net::AssetAckStatus     status) {
    if (!conn.valid()) return;
    const auto stats = conn.stats();
    if (!stats.connected || stats.shutdownStarted) return;
    sv::net::AssetAckMessage ack;
    ack.hash   = hash;
    ack.status = status;
    std::vector<uint8_t> bytes;
    if (!sv::net::encodeAssetAck(ack, bytes)) return;
    conn.sendReliableMessage(bytes.data(), bytes.size());
}

// Handle one inbound `AssetAnnounce` from `cs`.
void handleAssetAnnounce(ClientState&                                    cs,
                         const std::shared_ptr<ClientState>&             csShared,
                         sv::AssetPersistence&                           store,
                         const sv::net::AssetAnnounceMessage&            msg,
                         std::vector<std::shared_ptr<ClientState>>&      allClients,
                         uint64_t&                                       broadcastCounter) {
    // Server-side dedup check. If the asset is already cached,
    // short-circuit the upload — tell the client we have it and
    // broadcast the cached bytes to everyone else.
    if (msg.byteSize > sv::net::kAssetByteLimit) {
        SV_LOG_WARN("Server",
            "client %u announced oversized asset %u bytes > limit",
            static_cast<unsigned>(cs.clientId),
            static_cast<unsigned>(msg.byteSize));
        return;
    }

    const std::string hex = sv::digestToHex(msg.hash);
    const sv::AssetRecord* cached = store.find(msg.hash);
    if (cached) {
        sendAssetAck(cs.conn, msg.hash, sv::net::AssetAckStatus::HaveIt);
        SV_LOG_INFO("Server",
            "client %u announced cached asset '%s' (%s...) — dedup hit, broadcasting",
            static_cast<unsigned>(cs.clientId),
            msg.name.c_str(),
            hex.substr(0, 12).c_str());
        broadcastAsset(*cached, csShared, allClients);
        ++broadcastCounter;
        return;
    }

    // Ask the client to stream chunks. Allocate a receiver slot
    // keyed by the hex hash so we can route inbound Chunk messages.
    auto [it, _] = cs.pendingUploads.try_emplace(hex, sv::AssetReceiver{});
    it->second.beginFromAnnounce(msg.hash, msg.byteSize, msg.assetKind,
                                 msg.name,
                                 sv::assetChunkCount(msg.byteSize,
                                                     sv::net::kAssetChunkSize),
                                 sv::net::kAssetChunkSize);
    sendAssetAck(cs.conn, msg.hash, sv::net::AssetAckStatus::NeedChunks);
    SV_LOG_INFO("Server",
        "client %u announced new asset '%s' (%s..., %u bytes) — requesting chunks",
        static_cast<unsigned>(cs.clientId),
        msg.name.c_str(),
        hex.substr(0, 12).c_str(),
        static_cast<unsigned>(msg.byteSize));
}

// Handle one inbound `AssetChunk` from `cs`.
void handleAssetChunk(ClientState&                                    cs,
                      const std::shared_ptr<ClientState>&             csShared,
                      sv::AssetPersistence&                           store,
                      const sv::net::AssetChunkMessage&               msg,
                      std::vector<std::shared_ptr<ClientState>>&      allClients,
                      uint64_t&                                       broadcastCounter) {
    const std::string hex = sv::digestToHex(msg.hash);
    auto it = cs.pendingUploads.find(hex);
    if (it == cs.pendingUploads.end()) {
        SV_LOG_WARN("Server",
            "client %u sent chunk for unknown hash %s...",
            static_cast<unsigned>(cs.clientId),
            hex.substr(0, 12).c_str());
        return;
    }
    sv::AssetReceiver& rx = it->second;
    if (!rx.depositChunk(msg.chunkIndex, msg.chunk, msg.chunkLen)) {
        SV_LOG_WARN("Server",
            "client %u: depositChunk(%u/%u) refused for %s...",
            static_cast<unsigned>(cs.clientId),
            static_cast<unsigned>(msg.chunkIndex),
            static_cast<unsigned>(msg.chunkCount),
            hex.substr(0, 12).c_str());
        cs.pendingUploads.erase(it);
        return;
    }
    if (!rx.complete) return;

    if (!rx.verifyHash()) {
        SV_LOG_WARN("Server",
            "client %u upload %s... hash verification failed — dropping",
            static_cast<unsigned>(cs.clientId),
            hex.substr(0, 12).c_str());
        cs.pendingUploads.erase(it);
        return;
    }

    // Persist into the CAS. The save path is a hot-path no-op if
    // another client got there first (dedup), which keeps the
    // common-case simultaneous-drop flow cheap.
    const auto status = store.save(rx.hash, rx.assetKind, rx.name,
                                   rx.assembled.data(), rx.assembled.size());
    if (status != sv::AssetPersistenceStatus::Ok) {
        SV_LOG_WARN("Server",
            "AssetPersistence::save failed for %s...: %s",
            hex.substr(0, 12).c_str(),
            sv::assetPersistenceStatusToString(status));
        cs.pendingUploads.erase(it);
        return;
    }

    SV_LOG_INFO("Server",
        "client %u uploaded asset '%s' (%s..., %u bytes) — saved to CAS",
        static_cast<unsigned>(cs.clientId),
        rx.name.c_str(),
        hex.substr(0, 12).c_str(),
        static_cast<unsigned>(rx.byteSize));

    // Broadcast to the other welcomed clients from the now-cached
    // record. Look it up freshly from the store so we use the
    // authoritative bytes.
    const sv::AssetRecord* cached = store.find(rx.hash);
    if (cached) {
        broadcastAsset(*cached, csShared, allClients);
        ++broadcastCounter;
    }
    cs.pendingUploads.erase(it);
}

// ── Handshake state machine ─────────────────────────────────────
// Advances each client one step per tick: preamble → welcome →
// world-catch-up. Runs before the inbox drain so a client that
// finishes its handshake on this tick can also be the recipient
// of the same tick's snapshot broadcast.
void tickClientHandshake(ClientState&                 cs,
                         ServerWorld&                 world,
                         const std::vector<uint8_t>&  schemaPreamble,
                         const sv::ReplicationMeta&   netTransformMeta,
                         std::vector<std::shared_ptr<ClientState>>& allClients) {
    if (!cs.conn.valid() || cs.dead) return;
    const auto stats = cs.conn.stats();
    if (stats.shutdownStarted || stats.shutdownComplete) {
        cs.dead = true;
        return;
    }
    if (!stats.connected) return;

    // Step 1: schema handshake preamble.
    if (!cs.preambleSent) {
        if (schemaPreamble.empty()) return;
        if (cs.conn.sendReliableMessage(schemaPreamble.data(),
                                        schemaPreamble.size())) {
            cs.preambleSent = true;
            SV_LOG_INFO("Server",
                        "Sent schema preamble to client %u (%s, %zu bytes)",
                        static_cast<unsigned>(cs.clientId),
                        stats.peerAddress.c_str(),
                        schemaPreamble.size());
        }
        return;
    }

    // Step 2: welcome message. Tells the client its assigned
    // identity so it can recognise its own transactions in future
    // broadcasts.
    if (!cs.welcomeSent) {
        sv::net::WelcomeMessage welcome;
        welcome.clientId       = cs.clientId;
        welcome.scope          = static_cast<uint8_t>(cs.scope);
        welcome.avatarEntityId = cs.avatarEntityId;
        std::vector<uint8_t> bytes;
        if (!sv::net::encodeWelcomeMessage(welcome, bytes)) return;
        if (cs.conn.sendReliableMessage(bytes.data(), bytes.size())) {
            cs.welcomeSent = true;
            SV_LOG_INFO("Server",
                        "Welcomed client %u (avatarEnt=%u scope=%s)",
                        static_cast<unsigned>(cs.clientId),
                        static_cast<unsigned>(cs.avatarEntityId),
                        sv::permissionScopeToString(cs.scope));
        }
        return;
    }

    // Step 3: world catch-up (join-with-snapshot).
    // Replay a Spawn transaction for every currently-alive entity
    // so the new client's entity map matches the server's. This
    // uses the ent.transform member as the spawn payload, which is
    // the LIVE current state (updated every tick for the cube, and
    // per-mutation for avatars via SetField). A client that joins
    // mid-session therefore sees the cube at its CURRENT orbit
    // phase + every avatar at its most-recently-edited position,
    // not some stale snapshot from when the entity was originally
    // allocated. Then mark as worldSynced and let the normal
    // datagram broadcast take over from here.
    if (!cs.worldSynced) {
        size_t sent = 0;
        for (const auto& [entId, ent] : world.entities) {
            if (!ent.alive) continue;
            sv::EditTransaction tx =
                makeSpawnTransaction(world, ent, /*origin=*/0, netTransformMeta);
            pushReliableTransaction(cs.conn, tx);
            ++sent;
        }
        cs.worldSynced = true;
        SV_LOG_INFO("Server",
                    "Replayed %zu spawn tx to client %u",
                    sent, static_cast<unsigned>(cs.clientId));

        // Broadcast this client's avatar spawn to the OTHER welcomed
        // clients so they can render it. The new client has
        // already received its own avatar spawn through the
        // catch-up replay above.
        auto avatarIt = world.entities.find(cs.avatarEntityId);
        if (avatarIt != world.entities.end()) {
            sv::EditTransaction tx =
                makeSpawnTransaction(world, avatarIt->second, /*origin=*/0,
                                     netTransformMeta);
            for (const auto& other : allClients) {
                if (!other || other.get() == &cs) continue;
                if (other->dead || !other->welcomeSent) continue;
                pushReliableTransaction(other->conn, tx);
            }
        }
        return;
    }
}

// ── Apply a client-originated transaction ───────────────────────
// Returns true if the transaction was applied successfully. All
// failure modes log a warning but do not shut down the client —
// a single malformed or unauthorised request shouldn't drop the
// whole session.
bool applyClientTransaction(sv::EditTransaction& tx,
                            ClientState&         cs,
                            ServerWorld&         world,
                            const sv::ReplicationMeta& netTransformMeta,
                            const sv::ReplicationMeta& parentLinkMeta,
                            const sv::ReplicationMeta& lightComponentMeta,
                            const sv::ReplicationMeta& cameraComponentMeta,
                            const sv::ReplicationMeta& materialComponentMeta,
                            std::vector<std::shared_ptr<ClientState>>& allClients) {
    // Overwrite server-controlled fields. Clients cannot forge
    // their clientId, their txId, or their server timestamp.
    tx.originClientId = cs.clientId;
    tx.txId           = world.nextTxId++;
    tx.timestampMs    = monotonicMs();

    // Scope gate. SetField/Undo/Redo all require Editor.
    if (tx.requiredScope < sv::PermissionScope::Editor) {
        tx.requiredScope = sv::PermissionScope::Editor;
    }
    if (cs.scope < tx.requiredScope) {
        SV_LOG_WARN("Server",
                    "client %u scope %s below required %s — denied",
                    static_cast<unsigned>(cs.clientId),
                    sv::permissionScopeToString(cs.scope),
                    sv::permissionScopeToString(tx.requiredScope));
        return false;
    }

    switch (tx.kind) {
        case sv::EditKind::SetField: {
            auto it = world.entities.find(tx.entityId);
            if (it == world.entities.end() || !it->second.alive) {
                SV_LOG_WARN("Server",
                            "client %u SetField on missing entity %u",
                            static_cast<unsigned>(cs.clientId),
                            static_cast<unsigned>(tx.entityId));
                return false;
            }

            // LightComponent is dispatched BEFORE
            // the entity-level owner/server gates because its
            // authority class is Editor, not Owner. Any Editor-scope
            // client may mutate any entity's light sidecar — this is
            // the collaborative-editing primitive. The earlier scope
            // gate (cs.scope < Editor → denied) already guarantees
            // we only reach this code path on Editor+ clients.
            if (tx.typeNameHash == lightComponentMeta.typeNameHash) {
                sv::LightComponent afterLight = it->second.light;
                sv::DirtyMask afterMask(lightComponentMeta.fields.size());
                if (!sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                     tx.payload.data(),
                                                     tx.payload.size(),
                                                     &afterLight,
                                                     afterMask)) {
                    SV_LOG_WARN("Server",
                        "LightComponent SetField payload malformed from client %u",
                        static_cast<unsigned>(cs.clientId));
                    return false;
                }
                it->second.light = afterLight;
                SV_LOG_INFO("Server",
                    "client %u LightComponent entity=%u type=%u I=%.2f range=%.2f",
                    static_cast<unsigned>(cs.clientId),
                    static_cast<unsigned>(tx.entityId),
                    static_cast<unsigned>(afterLight.type),
                    afterLight.intensity,
                    afterLight.range);
                // Rebroadcast so every welcomed client mirrors the
                // new light state. There is no datagram path for
                // LightComponent — this rebroadcast IS the state
                // sync, same shape as ParentLink.
                broadcastTransaction(tx, allClients);
                return true;
            }

            // CameraComponent. Same Editor-authority
            // class as LightComponent — placed BEFORE the entity-level
            // owner gate so any Editor client can mutate any entity's
            // camera sidecar regardless of who owns the entity.
            if (tx.typeNameHash == cameraComponentMeta.typeNameHash) {
                sv::CameraComponent afterCamera = it->second.camera;
                sv::DirtyMask afterMask(cameraComponentMeta.fields.size());
                if (!sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                     tx.payload.data(),
                                                     tx.payload.size(),
                                                     &afterCamera,
                                                     afterMask)) {
                    SV_LOG_WARN("Server",
                        "CameraComponent SetField payload malformed from client %u",
                        static_cast<unsigned>(cs.clientId));
                    return false;
                }
                it->second.camera = afterCamera;
                SV_LOG_INFO("Server",
                    "client %u CameraComponent entity=%u fov=%.1f near=%.2f far=%.1f",
                    static_cast<unsigned>(cs.clientId),
                    static_cast<unsigned>(tx.entityId),
                    afterCamera.fovDeg,
                    afterCamera.nearPlane,
                    afterCamera.farPlane);
                broadcastTransaction(tx, allClients);
                return true;
            }

            // MaterialComponent. Same Editor-authority
            // path as LightComponent + CameraComponent.
            if (tx.typeNameHash == materialComponentMeta.typeNameHash) {
                sv::MaterialComponent afterMaterial = it->second.material;
                sv::DirtyMask afterMask(materialComponentMeta.fields.size());
                if (!sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                     tx.payload.data(),
                                                     tx.payload.size(),
                                                     &afterMaterial,
                                                     afterMask)) {
                    SV_LOG_WARN("Server",
                        "MaterialComponent SetField payload malformed from client %u",
                        static_cast<unsigned>(cs.clientId));
                    return false;
                }
                it->second.material = afterMaterial;
                SV_LOG_INFO("Server",
                    "client %u MaterialComponent entity=%u rgb=(%.2f,%.2f,%.2f) strength=%.2f",
                    static_cast<unsigned>(cs.clientId),
                    static_cast<unsigned>(tx.entityId),
                    afterMaterial.baseColorR,
                    afterMaterial.baseColorG,
                    afterMaterial.baseColorB,
                    afterMaterial.overrideStrength);
                broadcastTransaction(tx, allClients);
                return true;
            }

            // Owner-authority check: only the owning client may
            // SetField an Authority::Owner entity. Applies to
            // NetTransform + ParentLink (both Authority::Owner
            // practically, regardless of their registered tag —
            // entity-level authority comes from the ReplicatedEntity
            // record, not the component meta).
            if (it->second.authority == sv::Authority::Owner &&
                it->second.ownerClientId != cs.clientId) {
                SV_LOG_WARN("Server",
                            "client %u not owner of entity %u (owner=%u)",
                            static_cast<unsigned>(cs.clientId),
                            static_cast<unsigned>(tx.entityId),
                            static_cast<unsigned>(it->second.ownerClientId));
                return false;
            }
            // Server-authoritative entities are server-only.
            if (it->second.authority == sv::Authority::Server) {
                SV_LOG_WARN("Server",
                            "client %u SetField on server-authoritative entity %u",
                            static_cast<unsigned>(cs.clientId),
                            static_cast<unsigned>(tx.entityId));
                return false;
            }

            // Dispatch on typeNameHash across the
            // known component types. NetTransform SetFields get
            // the full undo-log + live mutation path. ParentLink
            // SetFields get a shorter path — no undo log entry, no
            // datagram echo (state flows only via this rebroadcast).
            // Unknown types are rejected.
            if (tx.typeNameHash == netTransformMeta.typeNameHash) {
                // Decode via the generic SetField path. The
                // wire payload is encodeSnapshot output for
                // NetTransform; start from the current transform so
                // any fields the client did NOT mark dirty preserve
                // their server-side value.
                sv::NetTransform afterState = it->second.transform;
                sv::DirtyMask afterMask(netTransformMeta.fields.size());
                if (!sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                     tx.payload.data(),
                                                     tx.payload.size(),
                                                     &afterState,
                                                     afterMask)) {
                    SV_LOG_WARN("Server",
                        "NetTransform SetField payload malformed from client %u",
                        static_cast<unsigned>(cs.clientId));
                    return false;
                }
                // Push onto undo log before overwriting the world.
                sv::UndoEntry entry;
                entry.txId           = tx.txId;
                entry.originClientId = cs.clientId;
                entry.entityId       = tx.entityId;
                entry.typeNameHash   = tx.typeNameHash;
                entry.beforeState    = it->second.transform;
                entry.afterState     = afterState;
                world.undoLog.recordApplied(entry);
                // Apply. The next datagram snapshot will carry the
                // new state to every client.
                it->second.transform = afterState;
                // Rebroadcast the transaction itself so client-side
                // UIs can mirror the wire log.
                broadcastTransaction(tx, allClients);
                return true;
            }

            if (tx.typeNameHash == parentLinkMeta.typeNameHash) {
                // ParentLink is a single u32.
                // Start from the current parent so a partial-mask
                // SetField would preserve unchanged fields (the
                // current component has only one field, so this is
                // degenerate, but the code shape matches
                // NetTransform's path for consistency).
                sv::ParentLink afterParent = it->second.parent;
                sv::DirtyMask afterMask(parentLinkMeta.fields.size());
                if (!sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                     tx.payload.data(),
                                                     tx.payload.size(),
                                                     &afterParent,
                                                     afterMask)) {
                    SV_LOG_WARN("Server",
                        "ParentLink SetField payload malformed from client %u",
                        static_cast<unsigned>(cs.clientId));
                    return false;
                }
                it->second.parent = afterParent;
                SV_LOG_INFO("Server",
                    "client %u ParentLink entity=%u parent=%u",
                    static_cast<unsigned>(cs.clientId),
                    static_cast<unsigned>(tx.entityId),
                    static_cast<unsigned>(afterParent.parentEntityId));
                // Rebroadcast so every client mirrors the new
                // parent link. There is no datagram path for
                // ParentLink — this rebroadcast IS the state sync.
                broadcastTransaction(tx, allClients);
                return true;
            }

            SV_LOG_WARN("Server",
                "client %u SetField unknown typeNameHash 0x%08x — denied",
                static_cast<unsigned>(cs.clientId),
                static_cast<unsigned>(tx.typeNameHash));
            return false;
        }
        case sv::EditKind::Undo: {
            const sv::UndoEntry* entry =
                world.undoLog.findLatestUndoable(cs.clientId);
            if (!entry) {
                SV_LOG_INFO("Server", "Undo: client %u has nothing to undo",
                            static_cast<unsigned>(cs.clientId));
                return false;
            }
            const uint64_t targetTxId = entry->txId;
            const uint32_t targetEnt  = entry->entityId;
            const sv::NetTransform before = entry->beforeState;
            // Apply the inverse.
            auto it = world.entities.find(targetEnt);
            if (it != world.entities.end() && it->second.alive) {
                it->second.transform = before;
            }
            world.undoLog.markUndone(targetTxId);
            // Broadcast an Undo transaction so clients can mirror
            // the history in their UI. Payload carries the target
            // txId as little-endian u64.
            sv::EditTransaction echo;
            echo.kind           = sv::EditKind::Undo;
            echo.txId           = world.nextTxId++;
            echo.originClientId = cs.clientId;
            echo.requiredScope  = sv::PermissionScope::Editor;
            echo.entityId       = targetEnt;
            echo.typeNameHash   = entry->typeNameHash;
            echo.timestampMs    = monotonicMs();
            echo.payload.reserve(8);
            for (int i = 0; i < 8; ++i) {
                echo.payload.push_back(
                    static_cast<uint8_t>((targetTxId >> (i * 8)) & 0xFF));
            }
            broadcastTransaction(echo, allClients);
            SV_LOG_INFO("Server",
                        "client %u undo tx=%llu entity=%u",
                        static_cast<unsigned>(cs.clientId),
                        static_cast<unsigned long long>(targetTxId),
                        static_cast<unsigned>(targetEnt));
            return true;
        }
        case sv::EditKind::Redo: {
            const sv::UndoEntry* entry =
                world.undoLog.findLatestRedoable(cs.clientId);
            if (!entry) {
                SV_LOG_INFO("Server", "Redo: client %u has nothing to redo",
                            static_cast<unsigned>(cs.clientId));
                return false;
            }
            const uint64_t targetTxId = entry->txId;
            const uint32_t targetEnt  = entry->entityId;
            const sv::NetTransform after = entry->afterState;
            auto it = world.entities.find(targetEnt);
            if (it != world.entities.end() && it->second.alive) {
                it->second.transform = after;
            }
            world.undoLog.markRedone(targetTxId);
            sv::EditTransaction echo;
            echo.kind           = sv::EditKind::Redo;
            echo.txId           = world.nextTxId++;
            echo.originClientId = cs.clientId;
            echo.requiredScope  = sv::PermissionScope::Editor;
            echo.entityId       = targetEnt;
            echo.typeNameHash   = entry->typeNameHash;
            echo.timestampMs    = monotonicMs();
            echo.payload.reserve(8);
            for (int i = 0; i < 8; ++i) {
                echo.payload.push_back(
                    static_cast<uint8_t>((targetTxId >> (i * 8)) & 0xFF));
            }
            broadcastTransaction(echo, allClients);
            SV_LOG_INFO("Server",
                        "client %u redo tx=%llu entity=%u",
                        static_cast<unsigned>(cs.clientId),
                        static_cast<unsigned long long>(targetTxId),
                        static_cast<unsigned>(targetEnt));
            return true;
        }
        case sv::EditKind::Spawn:
        case sv::EditKind::Despawn: {
            // Clients cannot Spawn or Despawn. This is server
            // lifecycle — policed here so a bad client cannot
            // inflate the entity map.
            SV_LOG_WARN("Server",
                        "client %u attempted %s (server-only, rejected)",
                        static_cast<unsigned>(cs.clientId),
                        sv::editKindToString(tx.kind));
            return false;
        }
    }
    return false;
}

// Drain this client's inbox and dispatch every inbound reliable
// message by first-byte msgType. Runs on the main thread; the
// MsQuic worker thread pushes messages under the inbox mutex.
//
// Besides EditTransactions, this routes the three asset-sync
// messages (Announce / Chunk / Ack) into the asset store +
// per-client pending-upload tracker.
void drainClientInbox(ClientState&                                    cs,
                      const std::shared_ptr<ClientState>&             csShared,
                      ServerWorld&                                    world,
                      const sv::ReplicationMeta&                      netTransformMeta,
                      const sv::ReplicationMeta&                      parentLinkMeta,
                      const sv::ReplicationMeta&                      lightComponentMeta,
                      const sv::ReplicationMeta&                      cameraComponentMeta,
                      const sv::ReplicationMeta&                      materialComponentMeta,
                      std::vector<std::shared_ptr<ClientState>>&      allClients,
                      sv::AssetPersistence&                           assetStore,
                      uint64_t&                                       assetBroadcastCounter) {
    std::vector<std::vector<uint8_t>> batch;
    {
        std::lock_guard<std::mutex> lk(cs.inboxMu);
        batch.swap(cs.inbox);
    }
    for (const auto& bytes : batch) {
        if (bytes.empty()) continue;
        const uint8_t msgType = bytes[0];

        // ── Asset sync messages ──────────────────────────────
        if (msgType == sv::net::kFrameAssetAnnounce) {
            sv::net::AssetAnnounceMessage msg;
            if (!sv::net::parseAssetAnnounce(bytes.data(), bytes.size(), msg)) {
                SV_LOG_WARN("Server",
                    "client %u sent malformed AssetAnnounce (%zu bytes)",
                    static_cast<unsigned>(cs.clientId), bytes.size());
                continue;
            }
            // Scope gate — asset uploads require Editor.
            if (cs.scope < sv::PermissionScope::Editor) {
                SV_LOG_WARN("Server",
                    "client %u scope %s below Editor — asset upload denied",
                    static_cast<unsigned>(cs.clientId),
                    sv::permissionScopeToString(cs.scope));
                continue;
            }
            handleAssetAnnounce(cs, csShared, assetStore, msg,
                                allClients, assetBroadcastCounter);
            continue;
        }
        if (msgType == sv::net::kFrameAssetChunk) {
            sv::net::AssetChunkMessage msg;
            if (!sv::net::parseAssetChunk(bytes.data(), bytes.size(), msg)) {
                SV_LOG_WARN("Server",
                    "client %u sent malformed AssetChunk (%zu bytes)",
                    static_cast<unsigned>(cs.clientId), bytes.size());
                continue;
            }
            if (cs.scope < sv::PermissionScope::Editor) {
                // Silent drop on scope — the announce path already
                // logged a denial; here we only see orphan chunks.
                continue;
            }
            handleAssetChunk(cs, csShared, assetStore, msg,
                             allClients, assetBroadcastCounter);
            continue;
        }
        if (msgType == sv::net::kFrameAssetAck) {
            // Clients do not currently send Ack upstream — the
            // server is always the ack producer, not the consumer.
            // Log and drop; a future session may use client-side
            // acks to signal "I already have this hash, stop
            // sending chunks".
            SV_LOG_INFO("Server",
                "client %u sent unexpected upstream AssetAck — ignored",
                static_cast<unsigned>(cs.clientId));
            continue;
        }

        // ── Edit transactions ────────────────────────────────
        if (msgType == sv::net::kFrameEditTransaction) {
            auto txOpt = sv::parseEditTransaction(bytes.data(), bytes.size());
            if (!txOpt) {
                SV_LOG_WARN("Server",
                            "client %u sent malformed edit transaction "
                            "(%zu bytes)",
                            static_cast<unsigned>(cs.clientId),
                            bytes.size());
                continue;
            }
            sv::EditTransaction tx = std::move(*txOpt);
            applyClientTransaction(tx, cs, world, netTransformMeta,
                                   parentLinkMeta, lightComponentMeta,
                                   cameraComponentMeta, materialComponentMeta,
                                   allClients);
            continue;
        }

        SV_LOG_WARN("Server",
            "client %u sent unknown reliable msgType=%u (%zu bytes)",
            static_cast<unsigned>(cs.clientId),
            static_cast<unsigned>(msgType),
            bytes.size());
    }
}

// ── Snapshot broadcast for all live entities ────────────────────
// Walks the entity map once per tick and emits one datagram per
// entity per welcomed client. This is trivially N*M complexity
// (clients * entities) — fine for small sessions with ≤8 clients
// and ≤10 entities. Scale-B tuning is future work.
void broadcastAllEntities(std::vector<std::shared_ptr<ClientState>>& clients,
                          uint32_t                                    tickIndex,
                          const sv::ReplicationMeta&                  meta,
                          ServerWorld&                                world,
                          const sv::DirtyMask&                        fullMask) {
    size_t sent    = 0;
    size_t dropped = 0;
    size_t pending = 0;

    for (const auto& [entId, ent] : world.entities) {
        if (!ent.alive) continue;
        std::vector<uint8_t> frame;
        if (!sv::net::encodeSnapshotFrame(tickIndex, ent.entityId, meta,
                                          &ent.transform, fullMask, frame)) {
            SV_LOG_WARN("Server",
                        "encodeSnapshotFrame failed for entity %u on tick %u",
                        static_cast<unsigned>(ent.entityId),
                        static_cast<unsigned>(tickIndex));
            continue;
        }
        for (const auto& cs : clients) {
            if (!cs || cs->dead) continue;
            if (!cs->conn.valid()) continue;
            const auto stats = cs->conn.stats();
            if (stats.shutdownStarted || stats.shutdownComplete) {
                cs->dead = true;
                continue;
            }
            if (!stats.connected || !cs->worldSynced) {
                ++pending;
                continue;
            }
            if (cs->conn.sendDatagram(frame.data(), frame.size())) {
                ++sent;
            } else {
                ++dropped;
            }
        }
    }

    if ((tickIndex % 30u) == 0u && (sent + pending + dropped) > 0) {
        SV_LOG_INFO("Server",
                    "tick=%u entities=%zu clients=%zu sent=%zu pending=%zu",
                    static_cast<unsigned>(tickIndex),
                    world.entities.size(),
                    clients.size(),
                    sent, pending);
    }
    if (dropped > 0) {
        SV_LOG_WARN("Server",
                    "broadcast tick=%u dropped=%zu",
                    static_cast<unsigned>(tickIndex), dropped);
    }
}

// ── Avatar allocation on accept ──────────────────────────────────
// Spreads avatars on a circle so two or three clients don't start
// on top of each other. Radius tuned to be well inside the
// orbiting cube's circle so both cubes and avatars stay visible in
// the lab harness's XZ canvas.
ReplicatedEntity makeAvatar(ServerWorld& world, uint32_t clientId) {
    ReplicatedEntity ent;
    ent.entityId      = world.nextEntityId++;
    ent.authority     = sv::Authority::Owner;
    ent.ownerClientId = clientId;
    constexpr float kSpawnRadius = 60.0f;
    const float angle = static_cast<float>(clientId) *
                        (2.0f * 3.14159265f / 8.0f);
    ent.transform.posX = std::cos(angle) * kSpawnRadius;
    ent.transform.posY = 55.0f;
    ent.transform.posZ = std::sin(angle) * kSpawnRadius;
    ent.transform.rotW = 1.0f;
    ent.label = "Client" + std::to_string(clientId);
    return ent;
}

// ── World persistence helpers ───────────────────────────────────
// Bridge between the in-memory ReplicatedEntity map and the core
// PersistedWorld format. The conversion is lossless for the
// fields the file format tracks (entityId/authority/ownerClientId/
// typeNameHash/label/serialised component bytes) — other per-entity
// server state (e.g. `alive` bit) is derived or re-computed.

sv::PersistedWorld snapshotWorldForPersistence(const ServerWorld&         world,
                                                const sv::ReplicationMeta& netTransformMeta) {
    sv::PersistedWorld out;
    out.nextEntityId = world.nextEntityId;
    out.nextClientId = world.nextClientId;
    out.nextTxId     = world.nextTxId;
    out.entities.reserve(world.entities.size());
    for (const auto& [entId, ent] : world.entities) {
        if (!ent.alive) continue;
        // Avatars (Authority::Owner) are tied to a specific live
        // client connection. When the server restarts nobody is
        // connected, so avatars have no business living in the
        // persisted snapshot — they would point at stale client
        // ids and get despawned on the next reap anyway. Only
        // server-authoritative entities (like the orbiting cube)
        // are persisted across restarts.
        if (ent.authority != sv::Authority::Server) continue;

        sv::PersistedEntity pe;
        pe.entityId      = ent.entityId;
        pe.authority     = static_cast<uint8_t>(ent.authority);
        pe.ownerClientId = ent.ownerClientId;
        pe.typeNameHash  = netTransformMeta.typeNameHash;
        pe.label         = ent.label;
        if (!sv::writeGenericSetFieldPayload(
                netTransformMeta,
                &ent.transform,
                [&]() {
                    sv::DirtyMask fullMask(netTransformMeta.fields.size());
                    fullMask.setAll();
                    return fullMask;
                }(),
                pe.payload)) {
            SV_LOG_WARN("Server",
                "writeGenericSetFieldPayload failed for entity %u during persistence",
                static_cast<unsigned>(ent.entityId));
            continue;
        }
        out.entities.push_back(std::move(pe));
    }
    return out;
}

// Rehydrate a ServerWorld from a PersistedWorld snapshot. Only
// NetTransform entities currently round-trip (one replicated type
// is persisted); unknown typeNameHashes are logged and skipped
// rather than aborting the whole load, so an operator can still
// recover partial state from a world.svbin produced by a future
// schema version.
void applyPersistedWorld(ServerWorld&                world,
                          const sv::PersistedWorld&   persisted,
                          const sv::ReplicationMeta&  netTransformMeta) {
    // Clear any pre-existing entities before replacing — a call to
    // applyPersistedWorld is treated as a whole-world swap, not a
    // merge. The server is expected to call this exactly once at
    // startup, before any client is accepted.
    world.entities.clear();

    world.nextEntityId = std::max<uint32_t>(persisted.nextEntityId, 100u);
    world.nextClientId = std::max<uint32_t>(persisted.nextClientId, 1u);
    world.nextTxId     = std::max<uint64_t>(persisted.nextTxId,     1u);

    for (const sv::PersistedEntity& pe : persisted.entities) {
        if (pe.typeNameHash != netTransformMeta.typeNameHash) {
            SV_LOG_WARN("Server",
                "persisted entity %u has typeNameHash 0x%08x — skipping "
                "(server only rehydrates NetTransform)",
                static_cast<unsigned>(pe.entityId),
                static_cast<unsigned>(pe.typeNameHash));
            continue;
        }
        ReplicatedEntity ent;
        ent.entityId      = pe.entityId;
        ent.authority     = static_cast<sv::Authority>(pe.authority);
        ent.ownerClientId = pe.ownerClientId;
        ent.label         = pe.label;
        ent.alive         = true;
        sv::DirtyMask m(netTransformMeta.fields.size());
        if (!sv::readGenericSetFieldPayload(pe.typeNameHash,
                                             pe.payload.data(),
                                             pe.payload.size(),
                                             &ent.transform,
                                             m)) {
            SV_LOG_WARN("Server",
                "readGenericSetFieldPayload failed for persisted entity %u",
                static_cast<unsigned>(pe.entityId));
            continue;
        }
        world.entities[ent.entityId] = std::move(ent);
    }
}

// Build the final save path under the configured --server-data dir.
std::string worldFilePath(const std::string& serverDataDir) {
    if (serverDataDir.empty()) return {};
    // Pick a portable subpath separator. The file is created with
    // std::filesystem inside WorldPersistence so forward slashes
    // work on both Windows and POSIX.
    if (!serverDataDir.empty() && serverDataDir.back() == '/') {
        return serverDataDir + "world.svbin";
    }
    if (!serverDataDir.empty() && serverDataDir.back() == '\\') {
        return serverDataDir + "world.svbin";
    }
    return serverDataDir + "/world.svbin";
}

// ── Schema preamble helper ──────────────────────────────────────
std::vector<uint8_t> buildSchemaHandshakePreamble() {
    sv::net::SchemaHandshake hs;
    hs.semver = sv::net::packSemver(
        STRATUMV_VERSION_MAJOR,
        STRATUMV_VERSION_MINOR,
        STRATUMV_VERSION_PATCH);
    for (const auto& entry : sv::ReplicationRegistry::get().getSchemaTable()) {
        hs.types.push_back({entry.typeNameHash, entry.schemaVersion});
    }
    std::vector<uint8_t> bytes;
    if (!sv::net::encodeSchemaHandshake(hs, bytes)) {
        bytes.clear();
    }
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    uint16_t    port             = 9001;
    uint32_t    idleTimeoutMs    = 60000;
    uint32_t    tickHz           = 30;
    float       orbitRadius      = 100.0f;
    std::string serverDataDir;                 // empty → persistence OFF
    uint32_t    saveIntervalSec  = 30;

    // ── CLI parse ────────────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            printUsage();
            return 0;
        }
        if (std::strcmp(arg, "--port") == 0) {
            if (i + 1 >= argc || !parsePort(argv[i + 1], port)) {
                std::fprintf(stderr,
                    "[stratumv_server] --port requires a positive 16-bit integer\n");
                return 3;
            }
            ++i;
            continue;
        }
        if (std::strcmp(arg, "--idle-timeout-ms") == 0) {
            if (i + 1 >= argc || !parseU32(argv[i + 1], idleTimeoutMs)) {
                std::fprintf(stderr,
                    "[stratumv_server] --idle-timeout-ms requires a non-negative integer\n");
                return 3;
            }
            ++i;
            continue;
        }
        if (std::strcmp(arg, "--tick-hz") == 0) {
            if (i + 1 >= argc || !parseU32(argv[i + 1], tickHz) || tickHz == 0) {
                std::fprintf(stderr,
                    "[stratumv_server] --tick-hz requires a positive integer\n");
                return 3;
            }
            ++i;
            continue;
        }
        if (std::strcmp(arg, "--orbit-radius") == 0) {
            if (i + 1 >= argc || !parseFloat(argv[i + 1], orbitRadius)) {
                std::fprintf(stderr,
                    "[stratumv_server] --orbit-radius requires a float\n");
                return 3;
            }
            ++i;
            continue;
        }
        if (std::strcmp(arg, "--server-data") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                    "[stratumv_server] --server-data requires a directory path\n");
                return 3;
            }
            serverDataDir = argv[i + 1];
            ++i;
            continue;
        }
        if (std::strcmp(arg, "--save-interval-sec") == 0) {
            if (i + 1 >= argc || !parseU32(argv[i + 1], saveIntervalSec) || saveIntervalSec == 0) {
                std::fprintf(stderr,
                    "[stratumv_server] --save-interval-sec requires a positive integer\n");
                return 3;
            }
            ++i;
            continue;
        }
        std::fprintf(stderr, "[stratumv_server] unknown arg: %s\n", arg);
        printUsage();
        return 3;
    }

    // ── Signal handling (graceful SIGINT shutdown) ───────────────
    std::signal(SIGINT, onSigint);

    // ── MsQuic availability check ────────────────────────────────
    if (!sv::net::Transport::isMsquicAvailable()) {
        std::fprintf(stderr,
            "[stratumv_server] MsQuic runtime not available - "
            "stratumv.lib was built with STRATUMV_ENABLE_MSQUIC=OFF\n");
        return 1;
    }

    SV_LOG_INFO("Server",
                "StratumV dedicated server starting (StratumV %s, msquic %s, tick %u Hz)",
                STRATUMV_VERSION_STRING,
                sv::net::Transport::msquicVersionString().c_str(),
                static_cast<unsigned>(tickHz));

    // ── Pre-flight: ensure NetTransform is registered ───────────
    const sv::ReplicationMeta& netTransformMetaRef =
        sv::ensureNetTransformRegistered();
    const sv::ReplicationMeta* const netTransformMeta = &netTransformMetaRef;
    if (netTransformMeta->authority != sv::Authority::Server) {
        std::fprintf(stderr,
            "[stratumv_server] NetTransform authority is %s, expected Server\n",
            sv::authorityToString(netTransformMeta->authority));
        return 1;
    }

    // ── Pre-flight: ensure ParentLink is registered ─────────────────
    const sv::ReplicationMeta& parentLinkMetaRef =
        sv::ensureParentLinkRegistered();
    const sv::ReplicationMeta* const parentLinkMeta = &parentLinkMetaRef;
    if (parentLinkMeta->authority != sv::Authority::Owner) {
        std::fprintf(stderr,
            "[stratumv_server] ParentLink authority is %s, expected Owner\n",
            sv::authorityToString(parentLinkMeta->authority));
        return 1;
    }

    // Third replicated component. Anchor + authority
    // sanity check same as ParentLink. LightComponent is the first
    // component that actually exercises Authority::Editor.
    const sv::ReplicationMeta& lightComponentMetaRef =
        sv::ensureLightComponentRegistered();
    const sv::ReplicationMeta* const lightComponentMeta = &lightComponentMetaRef;
    if (lightComponentMeta->authority != sv::Authority::Editor) {
        std::fprintf(stderr,
            "[stratumv_server] LightComponent authority is %s, expected Editor\n",
            sv::authorityToString(lightComponentMeta->authority));
        return 1;
    }

    // Fourth + fifth replicated components.
    // CameraComponent + MaterialComponent both ride on Authority::Editor
    // and follow the LightComponent anchor pattern. The pre-flight
    // check enforces that intent so a static-init regression cannot
    // silently downgrade them to the default Server tag.
    const sv::ReplicationMeta& cameraComponentMetaRef =
        sv::ensureCameraComponentRegistered();
    const sv::ReplicationMeta* const cameraComponentMeta = &cameraComponentMetaRef;
    if (cameraComponentMeta->authority != sv::Authority::Editor) {
        std::fprintf(stderr,
            "[stratumv_server] CameraComponent authority is %s, expected Editor\n",
            sv::authorityToString(cameraComponentMeta->authority));
        return 1;
    }

    const sv::ReplicationMeta& materialComponentMetaRef =
        sv::ensureMaterialComponentRegistered();
    const sv::ReplicationMeta* const materialComponentMeta = &materialComponentMetaRef;
    if (materialComponentMeta->authority != sv::Authority::Editor) {
        std::fprintf(stderr,
            "[stratumv_server] MaterialComponent authority is %s, expected Editor\n",
            sv::authorityToString(materialComponentMeta->authority));
        return 1;
    }

    // ── Build schema handshake preamble ─────────────────────────
    const std::vector<uint8_t> schemaPreamble = buildSchemaHandshakePreamble();
    if (schemaPreamble.empty()) {
        std::fprintf(stderr,
            "[stratumv_server] buildSchemaHandshakePreamble returned empty\n");
        return 1;
    }
    SV_LOG_INFO("Server",
                "Built schema handshake preamble (%zu bytes, %zu types)",
                schemaPreamble.size(),
                sv::ReplicationRegistry::get().getSchemaTable().size());

    // ── Transport bring-up ───────────────────────────────────────
    sv::net::Transport transport;
    sv::net::Transport::Config cfg;
    cfg.alpn                      = "stratumv/1";
    cfg.idleTimeoutMs             = idleTimeoutMs;
    cfg.useSelfSignedLoopbackCert = true;
    cfg.clientInsecureNoVerify    = false;
    cfg.appName                   = "stratumv_server";

    const auto startStatus = transport.start(cfg);
    if (startStatus != sv::net::TransportStatus::Ok) {
        std::fprintf(stderr,
            "[stratumv_server] Transport::start failed: %s\n",
            sv::net::transportStatusToString(startStatus));
        return 1;
    }

    // ── Server world init ───────────────────────────────────────
    ServerWorld world;
    const std::string worldSavePath = worldFilePath(serverDataDir);
    bool persistenceEnabled = !worldSavePath.empty();
    bool worldLoadedFromDisk = false;
    if (persistenceEnabled) {
        sv::PersistedWorld loaded;
        const auto loadStatus = sv::loadWorldFromFile(worldSavePath, loaded);
        switch (loadStatus) {
            case sv::WorldPersistenceStatus::Ok:
                applyPersistedWorld(world, loaded, *netTransformMeta);
                worldLoadedFromDisk = true;
                SV_LOG_INFO("Server",
                    "Loaded %zu entities from %s "
                    "(nextEntityId=%u nextClientId=%u nextTxId=%llu)",
                    world.entities.size(),
                    worldSavePath.c_str(),
                    static_cast<unsigned>(world.nextEntityId),
                    static_cast<unsigned>(world.nextClientId),
                    static_cast<unsigned long long>(world.nextTxId));
                break;
            case sv::WorldPersistenceStatus::MissingFile:
                SV_LOG_INFO("Server",
                    "No existing world file at %s — starting fresh",
                    worldSavePath.c_str());
                break;
            default:
                SV_LOG_WARN("Server",
                    "Failed to load %s: %s — starting fresh",
                    worldSavePath.c_str(),
                    sv::worldPersistenceStatusToString(loadStatus));
                break;
        }
    }
    if (world.entities.find(1) == world.entities.end()) {
        ReplicatedEntity cube;
        cube.entityId      = 1;
        cube.authority     = sv::Authority::Server;
        cube.ownerClientId = 0;
        cube.label         = "OrbitingCube";
        cube.transform.rotW = 1.0f;
        world.entities[cube.entityId] = std::move(cube);
    }
    sv::DirtyMask fullMask(netTransformMeta->fields.size());
    fullMask.setAll();

    // ── Asset persistence ───────────────────────────────────────
    // The CAS lives at `<serverDataDir>/assets/` when persistence
    // is enabled. Without `--server-data`, assets stay in the
    // in-memory cache only and are lost on restart; the upload +
    // broadcast flow still works for the current session.
    sv::AssetPersistence assetStore;
    uint64_t             assetBroadcastCount = 0;
    if (persistenceEnabled) {
        std::string assetsRoot = serverDataDir;
        if (!assetsRoot.empty() &&
            assetsRoot.back() != '/' &&
            assetsRoot.back() != '\\') {
            assetsRoot.push_back('/');
        }
        assetsRoot += "assets";
        const auto status = assetStore.setRootDir(assetsRoot);
        if (status != sv::AssetPersistenceStatus::Ok) {
            SV_LOG_WARN("Server",
                "AssetPersistence::setRootDir('%s') failed: %s",
                assetsRoot.c_str(),
                sv::assetPersistenceStatusToString(status));
        } else {
            SV_LOG_INFO("Server",
                "Asset CAS rooted at %s (%zu cached)",
                assetsRoot.c_str(), assetStore.size());
        }
    } else {
        SV_LOG_INFO("Server",
            "Asset CAS running in-memory only (no --server-data)");
    }

    // Helper lambda captured by the tick loop + the SIGINT flush
    // path below. Using a local lambda keeps the call site tight
    // and avoids yet another free function.
    auto persistWorldNow = [&](const char* reason) {
        if (!persistenceEnabled) return;
        const sv::PersistedWorld snap =
            snapshotWorldForPersistence(world, *netTransformMeta);
        const auto status = sv::saveWorldToFile(worldSavePath, snap);
        if (status == sv::WorldPersistenceStatus::Ok) {
            SV_LOG_INFO("Server",
                "Saved world (%s): %zu entities -> %s",
                reason,
                snap.entities.size(),
                worldSavePath.c_str());
        } else {
            SV_LOG_WARN("Server",
                "Failed to save world (%s): %s",
                reason,
                sv::worldPersistenceStatusToString(status));
        }
    };

    // ── Listener + tick loop ─────────────────────────────────────
    // Inner scope: the Listener and every held Connection must be
    // destroyed BEFORE Transport::stop() — RegistrationClose blocks
    // on live child handles.
    int exitCode      = 0;
    int acceptedCount = 0;
    uint32_t tickIndex = 0;
    {
        sv::net::Listener listener;
        const auto listenerStatus = transport.startListener(port, listener);
        if (listenerStatus != sv::net::TransportStatus::Ok) {
            std::fprintf(stderr,
                "[stratumv_server] startListener failed on port %u: %s\n",
                static_cast<unsigned>(port),
                sv::net::transportStatusToString(listenerStatus));
            transport.stop();
            return 2;
        }

        SV_LOG_INFO("Server",
                    "Listening on 127.0.0.1:%u (idle timeout %u ms)",
                    static_cast<unsigned>(listener.localPort()),
                    static_cast<unsigned>(idleTimeoutMs));

        std::vector<std::shared_ptr<ClientState>> clients;
        clients.reserve(16);

        using clock_t = std::chrono::steady_clock;
        const auto startWall = clock_t::now();
        const auto tickPeriod = std::chrono::nanoseconds(
            static_cast<int64_t>(1e9 / static_cast<double>(tickHz)));
        auto nextTick = startWall + tickPeriod;

        const uint32_t acceptTimeoutMs =
            std::max<uint32_t>(5, 1000u / (tickHz * 3u));

        // Autosave cadence. Next deadline is
        // `startWall + saveIntervalSec` regardless of whether the
        // initial state was loaded from disk — we always flush the
        // live state on the first cadence so any spawn updates
        // (e.g. the orbiting cube advancing) land in the file.
        const auto saveInterval =
            std::chrono::seconds(static_cast<int64_t>(saveIntervalSec));
        auto nextSave = startWall + saveInterval;

        while (g_running.load(std::memory_order_relaxed)) {
            // ── Accept one incoming connection per loop pass ─────
            sv::net::Connection conn = listener.acceptOne(acceptTimeoutMs);
            if (conn.valid()) {
                ++acceptedCount;
                auto cs = std::make_shared<ClientState>();
                cs->conn      = std::move(conn);
                cs->clientId  = world.nextClientId++;
                cs->scope     = sv::PermissionScope::Editor; // default scope
                // Allocate + register the avatar entity first so
                // its entityId lands inside ClientState before we
                // announce the welcome.
                ReplicatedEntity avatar = makeAvatar(world, cs->clientId);
                cs->avatarEntityId = avatar.entityId;
                world.entities[avatar.entityId] = std::move(avatar);
                // Install the reliable-message handler. Capture
                // via weak_ptr so the worker thread's callback
                // doesn't dangle if we reap the client first.
                std::weak_ptr<ClientState> weak = cs;
                cs->conn.setReliableMessageHandler(
                    [weak](const uint8_t* data, size_t size) {
                        if (!data || size == 0) return;
                        auto shared = weak.lock();
                        if (!shared || shared->dead) return;
                        std::vector<uint8_t> copy(data, data + size);
                        std::lock_guard<std::mutex> lk(shared->inboxMu);
                        shared->inbox.emplace_back(std::move(copy));
                    });
                const auto stats = cs->conn.stats();
                SV_LOG_INFO("Server",
                            "Accepted connection #%d from %s "
                            "(alpn=%s, clientId=%u, avatar=%u)",
                            acceptedCount,
                            stats.peerAddress.c_str(),
                            stats.negotiatedAlpn.c_str(),
                            static_cast<unsigned>(cs->clientId),
                            static_cast<unsigned>(cs->avatarEntityId));
                clients.push_back(std::move(cs));
            }

            // ── Handshake state machine for every client ─────────
            for (auto& cs : clients) {
                if (!cs) continue;
                tickClientHandshake(*cs, world, schemaPreamble,
                                    *netTransformMeta, clients);
            }

            // ── Drain inbound reliable messages + apply ──────────
            for (auto& cs : clients) {
                if (!cs || cs->dead) continue;
                drainClientInbox(*cs, cs, world, *netTransformMeta,
                                 *parentLinkMeta, *lightComponentMeta,
                                 *cameraComponentMeta, *materialComponentMeta,
                                 clients, assetStore, assetBroadcastCount);
            }

            // ── Tick deadline: update + broadcast ────────────────
            auto now = clock_t::now();
            if (now >= nextTick) {
                const double elapsedSec =
                    std::chrono::duration<double>(now - startWall).count();
                // Update entity 1 (the server-owned orbiting cube)
                auto cubeIt = world.entities.find(1);
                if (cubeIt != world.entities.end()) {
                    updateCubeTransform(cubeIt->second.transform,
                                        elapsedSec, orbitRadius);
                }
                ++tickIndex;
                broadcastAllEntities(clients, tickIndex, *netTransformMeta,
                                     world, fullMask);
                nextTick += tickPeriod;
                if (nextTick < now - tickPeriod) {
                    nextTick = now + tickPeriod;
                }
            }

            // ── Periodic world persistence flush ─────────────────
            if (persistenceEnabled && now >= nextSave) {
                persistWorldNow("autosave");
                nextSave = now + saveInterval;
            }

            // ── Reap dead clients + emit Despawn ─────────────────
            for (auto it = clients.begin(); it != clients.end();) {
                auto& cs = *it;
                bool reap = false;
                if (!cs) {
                    reap = true;
                } else if (cs->dead) {
                    reap = true;
                } else if (!cs->conn.valid()) {
                    reap = true;
                } else {
                    const auto stats = cs->conn.stats();
                    if (stats.shutdownStarted || stats.shutdownComplete) {
                        cs->dead = true;
                        reap = true;
                    }
                }
                if (!reap) { ++it; continue; }

                const uint32_t deadClientId     = cs ? cs->clientId       : 0;
                const uint32_t deadAvatarId     = cs ? cs->avatarEntityId : 0;
                const bool     wasAnnounced     = cs && cs->welcomeSent;

                if (deadAvatarId != 0) {
                    auto entIt = world.entities.find(deadAvatarId);
                    if (entIt != world.entities.end()) {
                        world.entities.erase(entIt);
                    }
                    if (wasAnnounced) {
                        sv::EditTransaction despawn =
                            makeDespawnTransaction(world, deadAvatarId,
                                                   *netTransformMeta);
                        broadcastTransaction(despawn, clients);
                    }
                }
                if (cs) {
                    cs->conn.setReliableMessageHandler(nullptr);
                    SV_LOG_INFO("Server",
                                "Client %u disconnected (avatar=%u reaped)",
                                static_cast<unsigned>(deadClientId),
                                static_cast<unsigned>(deadAvatarId));
                }
                it = clients.erase(it);
            }
        }

        // Count entities whose light sidecar is
        // actually active (type != Disabled AND intensity > 0). Cheap
        // linear walk — same scale as entities.size() at this point.
        // Also count active camera + material sidecars on the same
        // walk so the heartbeat shows whether any client has pushed
        // those component types this session.
        size_t litEntityCount      = 0;
        size_t cameraEntityCount   = 0;
        size_t materialEntityCount = 0;
        for (const auto& [entId, ent] : world.entities) {
            if (!ent.alive) continue;
            if (ent.light.type != 0 && ent.light.intensity > 0.0f) {
                ++litEntityCount;
            }
            if (ent.camera.fovDeg > 0.0f &&
                ent.camera.farPlane > ent.camera.nearPlane) {
                ++cameraEntityCount;
            }
            if (ent.material.overrideStrength > 0.0f) {
                ++materialEntityCount;
            }
        }
        SV_LOG_INFO("Server",
                    "SIGINT received: accepted=%d ticks=%u clients=%zu entities=%zu "
                    "lights=%zu cameras=%zu materials=%zu undoLog=%zu "
                    "assets=%zu assetBroadcasts=%llu",
                    acceptedCount,
                    static_cast<unsigned>(tickIndex),
                    clients.size(),
                    world.entities.size(),
                    litEntityCount,
                    cameraEntityCount,
                    materialEntityCount,
                    world.undoLog.size(),
                    assetStore.size(),
                    static_cast<unsigned long long>(assetBroadcastCount));

        // Final flush on the way out. Runs before the
        // Listener + clients vector destruct so any world state
        // mutated by last-tick inbound transactions lands on disk
        // before the process exits.
        if (persistenceEnabled) {
            persistWorldNow("shutdown");
        }
    } // Listener + Connections destruct here

    transport.stop();

    SV_LOG_INFO("Server", "StratumV dedicated server exit (%s%s)",
                persistenceEnabled ? "persistence ON" : "persistence OFF",
                worldLoadedFromDisk ? ", loaded" : "");
    return exitCode;
}
