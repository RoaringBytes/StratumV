// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── EditorBridge ──────────────────────────────────────────────────────
// Loopback-only TCP gateway that translates StratumV's QUIC replication
// stream into a small, stable protocol that a Blender addon (or any
// other Python / scripting editor) can speak without linking msquic or
// any other C++ dependency.
//
// ── Topology ──────────────────────────────────────────────────────
//
//   Blender (Python addon)
//       │
//       │ TCP 127.0.0.1:<port>   (this module — plain sockets,
//       │                         length-prefixed message framing)
//       ▼
//   skinned_test (bridge mode) ──────────────► QUIC ──────────────► stratumv_server
//       │                                        ▲
//       │                                        │
//       │                                     other clients also
//       │                                     speak QUIC directly
//       ▼
//   Vulkan viewport
//
// skinned_test is a first-class QUIC client of the server.
// This bridge is a *side door* that lets an
// external editor piggyback on skinned_test's existing QUIC connection.
// The bridge never talks to the server directly; it translates between
// its own tiny wire format and edit transactions that the lab harness
// emits through its existing `m_netConn.sendReliableMessage` path.
//
// ── Why not let Blender speak QUIC directly? ─────────────────────
//
// Pure-Python QUIC (aioquic) requires the user to `pip install`
// aioquic + cryptography into Blender's bundled Python, and the
// asyncio mainloop fights with Blender's single-threaded operator
// model. A ctypes wrapper over msquic.dll is ~400 lines of binding
// code plus a callback-from-C-to-Python thread nightmare. Both paths
// couple editor tooling to the engine's transport choice.
//
// A plain-TCP loopback bridge decouples them: Blender uses only
// Python's `socket` stdlib, the protocol is ~6 message types, and
// swapping the underlying server transport later (QUIC ↔ something
// else) does not break the addon.
//
// ── Wire frame ───────────────────────────────────────────────────
//
// Every message on either direction is length-prefixed:
//
//   [u32 payloadLen  little-endian]     (does NOT count these 4 bytes)
//   [u8  msgType]
//   [payloadLen - 1 payload bytes]
//
// The msgType byte is part of the payload, so the receiver does
// recv(4) → u32 len → recv(len) → first byte is type, rest is payload.
// Keeping msgType inside the length lets the reader pre-allocate the
// buffer once per message.
//
// ── Message types ────────────────────────────────────────────────
//
// Bridge → Client (downstream):
//
//   kBridgeMsgHello        = 0x01
//       [u32 bridgeClientId]        // 0 if not yet welcomed by server
//       [u32 bridgeAvatarEntityId]  // 0 if no avatar yet
//       [u8  scope]                 // PermissionScope raw byte
//       [u32 serverSemver]          // (major<<16)|(minor<<8)|patch
//       [u16 netTransformSchemaVer] // fnv16 of the NetTransform schema
//       [u8  serverState]           // 0 pending, 1 TLS ok, 2 welcomed
//       [u16 appNameLen][appName bytes]
//
//   kBridgeMsgEntityState  = 0x02 (fixed size after header)
//       [u32 entityId]
//       [u32 ownerClientId]
//       [u8  isSelf]                // 1 if this is the bridge's avatar
//       [u8  authority]             // Authority raw byte
//       [7× f32 transform]          // posX posY posZ rotX rotY rotZ rotW
//       [u16 labelLen][label bytes]
//
//   kBridgeMsgEntityGone   = 0x03
//       [u32 entityId]
//
//   kBridgeMsgServerState  = 0x04
//       [u8 state]                  // 0 pending, 1 TLS ok, 2 welcomed
//
// Client → Bridge (upstream):
//
//   kBridgeMsgMoveSelf     = 0x81
//       [7× f32 transform]          // target state for bridge's avatar
//
//   kBridgeMsgPing         = 0x82
//       (empty — keepalive)
//
//   kBridgeMsgAssetAnnounce = 0x83
//       [32 u8  sha256]
//       [u32    byteSize]
//       [u8     assetKind]
//       [u16    nameLen][name bytes]   // forward-slash path
//
//   kBridgeMsgAssetChunk    = 0x84
//       [32 u8  sha256]
//       [u32    chunkIndex]            // 0 .. chunkCount-1
//       [u32    chunkCount]            // total chunks in this asset
//       [u32    chunkLen]              // this chunk's byte count
//       [chunkLen bytes chunk data]
//
//   kBridgeMsgSetParent     = 0x86
//       [u32    parentEntityId]        // 0 = unparented
//
//   kBridgeMsgSetLight      = 0x87
//       [u32    type]                  // 0=Disabled 1=Directional 2=Point 3=Spot
//       [f32    colorR][f32 colorG][f32 colorB]
//       [f32    intensity]
//       [f32    range]
//       [f32    coneInnerDeg][f32 coneOuterDeg]
//       // 32 bytes total. Applies to the bridge's own avatar entity
//       // (m_helloAvatarEntityId); there is no explicit entityId byte,
//       // same convention as MoveSelf and SetParent.
//
//   kBridgeMsgSetCamera     = 0x88
//       [f32    fovDeg]                // 0 = no override
//       [f32    aspect]                // 0 = use window aspect
//       [f32    nearPlane]
//       [f32    farPlane]              // must be > nearPlane to enable
//       // 16 bytes total. Same "applies to bridge avatar" convention.
//
//   kBridgeMsgSetMaterial   = 0x89
//       [f32    baseColorR][f32 baseColorG][f32 baseColorB]
//       [f32    overrideStrength]      // [0..1], 0 = no effect
//       // 16 bytes total. Same "applies to bridge avatar" convention.
//
// The asset Announce/Chunk wire layout matches the QUIC-side
// `kFrameAssetAnnounce` / `kFrameAssetChunk` bodies byte-for-byte —
// the only difference is the outer framing (length-prefixed TCP vs
// raw QUIC stream message). This lets the Python side share a
// single layout spec with the C++ sv::net::AssetAnnounceMessage /
// sv::net::AssetChunkMessage structs.
//
// ── Scope constraints ────────────────────────────────────────────
//
// The bridge is a dev / editor tool and is NOT exposed on the WAN.
// It binds to 127.0.0.1 only. Every accepted client inherits the
// bridge's own permission scope (set by the Welcome message from the
// server). There is no per-Blender-client identity — from the server's
// POV, a Blender edit looks like a SetField from the bridge's
// skinned_test client.

#include "AssetPersistence.h"
#include "AssetUploadClient.h"
#include "CameraComponent.h"
#include "LightComponent.h"
#include "MaterialComponent.h"
#include "NetTransform.h"
#include "ParentLink.h"
#include "ReplicationRegistry.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sv {
namespace net {

// ── Message type bytes ────────────────────────────────────────────
constexpr uint8_t kBridgeMsgHello         = 0x01;
constexpr uint8_t kBridgeMsgEntityState   = 0x02;
constexpr uint8_t kBridgeMsgEntityGone    = 0x03;
constexpr uint8_t kBridgeMsgServerState   = 0x04;
constexpr uint8_t kBridgeMsgMoveSelf      = 0x81;
constexpr uint8_t kBridgeMsgPing          = 0x82;
constexpr uint8_t kBridgeMsgAssetAnnounce = 0x83;
constexpr uint8_t kBridgeMsgAssetChunk    = 0x84;
constexpr uint8_t kBridgeMsgSetParent     = 0x86;
constexpr uint8_t kBridgeMsgSetLight      = 0x87;
constexpr uint8_t kBridgeMsgSetCamera     = 0x88;
constexpr uint8_t kBridgeMsgSetMaterial   = 0x89;

// Body byte counts for the explicit upstream messages. Exported so the
// wire tests can static_assert the sizes and the reader thread short-
// buffer checks share a single source of truth.
constexpr size_t  kBridgeSetParentBodyBytes   = 4;   // [u32 parentEntityId]
constexpr size_t  kBridgeSetLightBodyBytes    = 32;  // [u32 type][7*f32]
constexpr size_t  kBridgeSetCameraBodyBytes   = 16;  // [4*f32]
constexpr size_t  kBridgeSetMaterialBodyBytes = 16;  // [4*f32]

// ── Bridge server state enum (what the bridge knows about the QUIC
//    link to the real stratumv_server) ──────────────────────────────
constexpr uint8_t kBridgeServerPending  = 0;  // not yet handshaken
constexpr uint8_t kBridgeServerTlsOk    = 1;  // QUIC+TLS handshake ok
constexpr uint8_t kBridgeServerWelcomed = 2;  // Welcome message received

// ── POD snapshot of an entity the bridge knows about ─────────────
struct EditorBridgeEntityState {
    uint32_t      entityId      = 0;
    uint32_t      ownerClientId = 0;
    bool          isSelf        = false;
    uint8_t       authority     = 0;
    NetTransform  transform     {};
    std::string   label;
};

// ── Move request from a connected Blender/editor client ──────────
struct EditorBridgeMove {
    NetTransform target{};
};

// ── Asset push from a connected Blender/editor client ────────────
// Produced by the bridge after it has assembled every chunk of an
// inbound AssetAnnounce + AssetChunk sequence and verified the
// declared sha256 against the bytes. The main thread converts each
// into an AssetUploadRequest and pumps it through the same path
// `uploadAssetFromDisk` uses for the "Upload to server" button.
struct EditorBridgeAsset {
    AssetHash            hash{};
    uint32_t             byteSize  = 0;
    uint8_t              assetKind = 0;
    std::string          name;
    std::vector<uint8_t> bytes;
};

// ── Parent-change request from a connected Blender/editor client ─
// Produced by the bridge when a connected Blender sends a
// kBridgeMsgSetParent frame. The main thread turns this into a
// SetField transaction targeting the bridge's avatar's ParentLink
// component and pushes it through the existing reliable-stream
// path.
struct EditorBridgeParentChange {
    uint32_t parentEntityId = 0;
};

// ── Light-set request from a connected Blender/editor client ─────
// Produced by the bridge when a connected Blender sends a
// kBridgeMsgSetLight frame. The main thread turns this into a
// full-mask SetField EditTransaction targeting the bridge's
// avatar's LightComponent sidecar and pushes it through the same
// reliable-stream path MoveSelf + SetParent use.
struct EditorBridgeLightSet {
    LightComponent light{};
};

// ── Camera-set request from a connected Blender/editor client ────
// Produced by the bridge when a connected Blender sends a
// kBridgeMsgSetCamera frame. The main thread turns this into a
// full-mask SetField EditTransaction targeting the bridge's
// avatar's CameraComponent sidecar.
struct EditorBridgeCameraSet {
    CameraComponent camera{};
};

// ── Material-set request from a connected Blender/editor client ──
// Produced by the bridge when a connected Blender sends a
// kBridgeMsgSetMaterial frame. The main thread turns this into a
// full-mask SetField EditTransaction targeting the bridge's
// avatar's MaterialComponent sidecar.
struct EditorBridgeMaterialSet {
    MaterialComponent material{};
};

// ── The bridge itself ────────────────────────────────────────────
//
// Thread model:
//   - One listener thread (blocking accept loop) created in start()
//   - One reader thread per accepted client
//   - The main thread calls setHello / setServerState / pushEntityState
//     / pushEntityGone to push outgoing frames
//   - The main thread calls drainMoves() once per frame to pull
//     inbound Blender edits
//
// start() / stop() are idempotent. stop() breaks the listener socket
// which forces the accept loop to exit, and closes each client socket
// which forces the reader threads to exit.
class EditorBridge {
public:
    EditorBridge();
    ~EditorBridge();

    EditorBridge(const EditorBridge&)            = delete;
    EditorBridge& operator=(const EditorBridge&) = delete;

    // Start a listener on 127.0.0.1:port. Returns true on success.
    // Calling start() on an already-running bridge is a no-op (logs).
    bool start(uint16_t port);

    // Idempotent shutdown. Safe to call from ~EditorBridge or from
    // the main thread mid-session.
    void stop();

    bool     running() const { return m_running.load(); }
    uint16_t port()    const { return m_port; }

    // Update the cached Hello snapshot. Called by the lab harness as
    // soon as it knows its own identity (after the Welcome message
    // from the server arrives). Pushes a fresh Hello to every
    // connected client so they see the update even if they connected
    // before it was available.
    void setHello(uint32_t      clientId,
                  uint32_t      avatarEntityId,
                  uint16_t      netTransformSchemaVersion,
                  uint8_t       scope,
                  uint32_t      serverSemver,
                  uint8_t       serverState,
                  const std::string& appName);

    // Update just the server-state byte (what the bridge knows about
    // the real QUIC link). Pushes a ServerState message to every
    // connected client. Cheap.
    void setServerState(uint8_t serverState);

    // Publish a snapshot for `state.entityId` to every connected
    // client. The bridge caches the latest state per entity so newly
    // connected clients are immediately brought up to date.
    void pushEntityState(const EditorBridgeEntityState& state);

    // Announce that an entity is gone. Removes it from the cache and
    // broadcasts an EntityGone message.
    void pushEntityGone(uint32_t entityId);

    // Drain pending MoveSelf messages. Called once per frame by the
    // lab harness; each returned EditorBridgeMove is translated into
    // a SetField EditTransaction aimed at the bridge's own avatar.
    std::vector<EditorBridgeMove> drainMoves();

    // Drain pending asset uploads (complete + hash-verified). Called
    // once per frame by the lab harness; each returned
    // EditorBridgeAsset is translated into a local
    // AssetPersistence::save + a server upload via the existing
    // uploadAssetFromDisk path.
    std::vector<EditorBridgeAsset> drainAssets();

    // Drain pending parent-change requests. Each returned
    // EditorBridgeParentChange becomes one ParentLink SetField
    // EditTransaction targeting the bridge's avatar.
    std::vector<EditorBridgeParentChange> drainParentChanges();

    // Drain pending light-set requests. Each returned
    // EditorBridgeLightSet becomes one full-mask LightComponent
    // SetField EditTransaction targeting the bridge's avatar.
    std::vector<EditorBridgeLightSet> drainLights();

    // Drain pending camera-set requests. Each returned
    // EditorBridgeCameraSet becomes one full-mask CameraComponent
    // SetField EditTransaction targeting the bridge's avatar.
    std::vector<EditorBridgeCameraSet> drainCameras();

    // Drain pending material-set requests. Each returned
    // EditorBridgeMaterialSet becomes one full-mask MaterialComponent
    // SetField EditTransaction targeting the bridge's avatar.
    std::vector<EditorBridgeMaterialSet> drainMaterials();

    // Observability.
    uint64_t clientsAccepted()   const { return m_clientsAccepted.load(); }
    uint64_t messagesInbound()   const { return m_inbound.load(); }
    uint64_t messagesOutbound()  const { return m_outbound.load(); }
    uint64_t assetsReceived()    const { return m_assetsReceived.load(); }
    uint64_t parentChanges()     const { return m_parentChanges.load(); }
    uint64_t lightsReceived()    const { return m_lightsReceived.load(); }
    uint64_t camerasReceived()   const { return m_camerasReceived.load(); }
    uint64_t materialsReceived() const { return m_materialsReceived.load(); }
    size_t   clientCount()       const;

private:
    struct Client;

    void listenerThreadBody();
    void clientRxLoopBody(std::shared_ptr<Client> client);

    void sendToClient(Client& client, const std::vector<uint8_t>& frame);
    void broadcastFrame(const std::vector<uint8_t>& frame);
    void bringClientUpToDate(Client& client);
    void reapDeadClients();

    // Framing helpers (little-endian, no exceptions).
    static void writeU8(std::vector<uint8_t>& out, uint8_t v);
    static void writeU16(std::vector<uint8_t>& out, uint16_t v);
    static void writeU32(std::vector<uint8_t>& out, uint32_t v);
    static void writeF32(std::vector<uint8_t>& out, float v);
    static void writeString(std::vector<uint8_t>& out, const std::string& s);

    // Wraps `[u8 type][payloadBytes]` into a `[u32 len][bytes]` frame.
    static std::vector<uint8_t> makeFrame(uint8_t msgType,
                                          const std::vector<uint8_t>& body);

    std::vector<uint8_t> buildHelloFrame() const;
    std::vector<uint8_t> buildEntityStateFrame(const EditorBridgeEntityState& s) const;
    std::vector<uint8_t> buildEntityGoneFrame(uint32_t entityId) const;
    std::vector<uint8_t> buildServerStateFrame(uint8_t state) const;

    std::atomic<bool> m_running{false};
    uint16_t          m_port = 0;

    // Raw socket handle — uintptr_t so we can compile on both Windows
    // (SOCKET = UINT_PTR) and POSIX (int). Sentinel value lives in
    // the .cpp so the header does not need winsock headers.
    uintptr_t         m_listenSocket = 0;  // 0 means "no socket"
    std::thread       m_listenerThread;

    // Hello snapshot — protected by m_stateMu.
    mutable std::mutex m_stateMu;
    uint32_t           m_helloClientId       = 0;
    uint32_t           m_helloAvatarEntityId = 0;
    uint16_t           m_helloSchemaVersion  = 0;
    uint8_t            m_helloScope          = 0;
    uint32_t           m_helloSemver         = 0;
    uint8_t            m_helloServerState    = kBridgeServerPending;
    std::string        m_helloAppName        = "stratumv_lab";

    // Entity cache — also under m_stateMu.
    std::unordered_map<uint32_t, EditorBridgeEntityState> m_entityCache;

    // Connected clients.
    mutable std::mutex                        m_clientsMu;
    std::vector<std::shared_ptr<Client>>      m_clients;

    // Pending move-self requests from any client.
    std::mutex                        m_movesMu;
    std::vector<EditorBridgeMove>     m_pendingMoves;

    // In-flight asset uploads. The reader thread
    // parses Announce + Chunk frames into per-hash AssetReceiver
    // state; on completion + hash match, the assembled payload is
    // moved onto m_pendingAssets and the receiver slot is freed.
    std::mutex                                         m_assetsMu;
    std::unordered_map<std::string, AssetReceiver>     m_assetReceivers;
    std::vector<EditorBridgeAsset>                     m_pendingAssets;

    // Pending parent-change requests.
    std::mutex                                  m_parentsMu;
    std::vector<EditorBridgeParentChange>       m_pendingParents;

    // Pending light-set requests. Each entry is a
    // full-mask LightComponent snapshot; the main thread translates it
    // into a SetField EditTransaction aimed at the bridge's avatar.
    std::mutex                                  m_lightsMu;
    std::vector<EditorBridgeLightSet>           m_pendingLights;

    // Pending camera-set + material-set requests.
    // Same shape + ownership rules as m_pendingLights — each entry
    // becomes one full-mask SetField EditTransaction on the bridge's
    // avatar.
    std::mutex                                  m_camerasMu;
    std::vector<EditorBridgeCameraSet>          m_pendingCameras;

    std::mutex                                  m_materialsMu;
    std::vector<EditorBridgeMaterialSet>        m_pendingMaterials;

    std::atomic<uint64_t> m_clientsAccepted{0};
    std::atomic<uint64_t> m_inbound{0};
    std::atomic<uint64_t> m_outbound{0};
    std::atomic<uint64_t> m_assetsReceived{0};
    std::atomic<uint64_t> m_parentChanges{0};
    std::atomic<uint64_t> m_lightsReceived{0};
    std::atomic<uint64_t> m_camerasReceived{0};
    std::atomic<uint64_t> m_materialsReceived{0};
};

} // namespace net
} // namespace sv
