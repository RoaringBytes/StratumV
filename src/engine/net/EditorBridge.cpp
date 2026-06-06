// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── EditorBridge implementation ────────────────────────────────────
//
// Windows-first plain TCP bridge. On non-Windows the methods compile
// to no-ops (the full stratumv.lib build path only ships on Windows
// today; the Linux carve-out in stratumv_core does not include this
// module at all, see cmake/stratumv_core_sources.cmake).
//
// Threading:
//   - start() spawns m_listenerThread which sits in blocking accept.
//   - Each accepted client gets a std::thread running clientRxLoopBody
//     stored inside Client::reader.
//   - stop() closes the listen socket (breaks accept), then closes
//     each client socket (breaks recv), then joins everything.
//
// Send path:
//   - Main thread calls pushEntityState / pushEntityGone / setHello.
//   - Those helpers build a length-prefixed frame and call
//     broadcastFrame, which walks m_clients under m_clientsMu and
//     calls sendToClient on each live one.
//   - Client::sendMu serialises bytes going out to one socket so
//     two main-thread calls never interleave frames.
//
// Receive path:
//   - clientRxLoopBody reads `[u32 len][bytes]` in a loop. On EOF or
//     error it marks the client dead and returns. The main thread
//     reaps dead clients lazily inside drainMoves() / broadcastFrame
//     — we never touch the vector from the reader thread itself.

#include "net/EditorBridge.h"
#include "AssetPersistence.h"
#include "EngineLog.h"
#include "Sha256.h"

#include <cstring>
#include <cstdio>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using SocketHandle = SOCKET;
    static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
    static int closeSocket(SocketHandle s) { return ::closesocket(s); }
    static int lastSocketError() { return ::WSAGetLastError(); }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    using SocketHandle = int;
    static constexpr SocketHandle kInvalidSocket = -1;
    static int closeSocket(SocketHandle s) { return ::close(s); }
    static int lastSocketError() { return errno; }
#endif

namespace sv {
namespace net {

namespace {

// Process-wide winsock lifecycle. Refcounted so multiple EditorBridge
// instances in the same process don't step on each other. On non-
// Windows these are no-ops.
std::mutex g_wsaMutex;
int        g_wsaRefs = 0;

bool wsaInit() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_wsaMutex);
    if (g_wsaRefs == 0) {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            SV_LOG_ERROR("net", "WSAStartup failed: %d", rc);
            return false;
        }
    }
    ++g_wsaRefs;
#endif
    return true;
}

void wsaShutdown() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_wsaMutex);
    if (g_wsaRefs == 0) return;
    --g_wsaRefs;
    if (g_wsaRefs == 0) {
        WSACleanup();
    }
#endif
}

// Blocking recv that fills `buf` with exactly `len` bytes. Returns
// true on success, false on any error (including clean EOF before
// the buffer is full).
bool recvExact(SocketHandle s, void* buf, size_t len) {
    auto* cur = static_cast<char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        const int got = ::recv(s, cur, static_cast<int>(remaining), 0);
        if (got == 0)  return false;   // clean peer close
        if (got <  0)  return false;   // error
        cur       += got;
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

bool sendAll(SocketHandle s, const void* buf, size_t len) {
    const auto* cur = static_cast<const char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        const int sent = ::send(s, cur, static_cast<int>(remaining), 0);
        if (sent <= 0) return false;
        cur       += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(p[0]) |
        (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32LE(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

float readF32LE(const uint8_t* p) {
    uint32_t bits = readU32LE(p);
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Upper bound on a frame size. Anything larger is a misbehaving
// client or an attacker and gets dropped cleanly.
//
// This was bumped 64 KiB → 256 KiB so a 64 KiB asset
// chunk payload + the 45-byte chunk header + the 4-byte outer length
// prefix fits comfortably. The cap is still a sanity check against
// runaway allocations — real traffic on loopback is far below the
// limit.
constexpr uint32_t kMaxFrameBytes = 256 * 1024;

} // namespace

// ── Client struct definition ─────────────────────────────────────
struct EditorBridge::Client {
    SocketHandle      sock       = kInvalidSocket;
    std::atomic<bool> alive{true};
    std::string       peerAddr;
    std::thread       reader;
    std::mutex        sendMu;    // serialises frames out to this socket
};

// ── Framing helpers ──────────────────────────────────────────────

void EditorBridge::writeU8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}

void EditorBridge::writeU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>( v        & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >>  8) & 0xFFu));
}

void EditorBridge::writeU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>( v        & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >>  8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void EditorBridge::writeF32(std::vector<uint8_t>& out, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32(out, bits);
}

void EditorBridge::writeString(std::vector<uint8_t>& out, const std::string& s) {
    const uint16_t len = static_cast<uint16_t>(
        s.size() > 0xFFFFu ? 0xFFFFu : s.size());
    writeU16(out, len);
    if (len > 0) {
        out.insert(out.end(),
                   s.data(),
                   s.data() + len);
    }
}

std::vector<uint8_t> EditorBridge::makeFrame(uint8_t msgType,
                                             const std::vector<uint8_t>& body) {
    // Frame = [u32 len][u8 type][body]
    // len does NOT include its own 4 bytes; it DOES include the type
    // byte and the body.
    std::vector<uint8_t> out;
    out.reserve(4 + 1 + body.size());
    const uint32_t len = static_cast<uint32_t>(1 + body.size());
    writeU32(out, len);
    writeU8 (out, msgType);
    if (!body.empty()) {
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

// ── Message builders ─────────────────────────────────────────────

std::vector<uint8_t> EditorBridge::buildHelloFrame() const {
    std::vector<uint8_t> body;
    body.reserve(32 + m_helloAppName.size());

    writeU32(body, m_helloClientId);
    writeU32(body, m_helloAvatarEntityId);
    writeU8 (body, m_helloScope);
    writeU32(body, m_helloSemver);
    writeU16(body, m_helloSchemaVersion);
    writeU8 (body, m_helloServerState);
    writeString(body, m_helloAppName);

    return makeFrame(kBridgeMsgHello, body);
}

std::vector<uint8_t>
EditorBridge::buildEntityStateFrame(const EditorBridgeEntityState& s) const {
    std::vector<uint8_t> body;
    body.reserve(4 + 4 + 1 + 1 + 7 * 4 + 2 + s.label.size());

    writeU32(body, s.entityId);
    writeU32(body, s.ownerClientId);
    writeU8 (body, s.isSelf ? 1u : 0u);
    writeU8 (body, s.authority);

    writeF32(body, s.transform.posX);
    writeF32(body, s.transform.posY);
    writeF32(body, s.transform.posZ);
    writeF32(body, s.transform.rotX);
    writeF32(body, s.transform.rotY);
    writeF32(body, s.transform.rotZ);
    writeF32(body, s.transform.rotW);

    writeString(body, s.label);

    return makeFrame(kBridgeMsgEntityState, body);
}

std::vector<uint8_t>
EditorBridge::buildEntityGoneFrame(uint32_t entityId) const {
    std::vector<uint8_t> body;
    body.reserve(4);
    writeU32(body, entityId);
    return makeFrame(kBridgeMsgEntityGone, body);
}

std::vector<uint8_t>
EditorBridge::buildServerStateFrame(uint8_t state) const {
    std::vector<uint8_t> body;
    body.reserve(1);
    writeU8(body, state);
    return makeFrame(kBridgeMsgServerState, body);
}

// ── Lifecycle ────────────────────────────────────────────────────

EditorBridge::EditorBridge() = default;

EditorBridge::~EditorBridge() {
    stop();
}

bool EditorBridge::start(uint16_t port) {
    if (m_running.load()) {
        SV_LOG_WARN("net",
                    "EditorBridge::start called while already running on port %u",
                    static_cast<unsigned>(m_port));
        return true;
    }
    if (port == 0) {
        SV_LOG_ERROR("net", "EditorBridge::start requires a non-zero port");
        return false;
    }

    if (!wsaInit()) return false;

    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
        SV_LOG_ERROR("net", "EditorBridge socket() failed: %d",
                     lastSocketError());
        wsaShutdown();
        return false;
    }

    // Allow quick rebinds during dev loops.
    int reuse = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);    // 127.0.0.1 only

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        SV_LOG_ERROR("net", "EditorBridge bind(127.0.0.1:%u) failed: %d",
                     static_cast<unsigned>(port), lastSocketError());
        closeSocket(s);
        wsaShutdown();
        return false;
    }

    if (::listen(s, 4) != 0) {
        SV_LOG_ERROR("net", "EditorBridge listen() failed: %d",
                     lastSocketError());
        closeSocket(s);
        wsaShutdown();
        return false;
    }

    m_listenSocket = static_cast<uintptr_t>(s);
    m_port         = port;
    m_running.store(true);

    m_listenerThread = std::thread(&EditorBridge::listenerThreadBody, this);

    SV_LOG_INFO("net", "EditorBridge listening on 127.0.0.1:%u",
                static_cast<unsigned>(port));
    return true;
}

void EditorBridge::stop() {
    if (!m_running.exchange(false)) return;

    // Close the listen socket first — unblocks accept().
    if (m_listenSocket != 0) {
        closeSocket(static_cast<SocketHandle>(m_listenSocket));
        m_listenSocket = 0;
    }
    if (m_listenerThread.joinable()) {
        m_listenerThread.join();
    }

    // Close every client socket — unblocks each reader.
    {
        std::lock_guard<std::mutex> lk(m_clientsMu);
        for (auto& c : m_clients) {
            if (c->sock != kInvalidSocket) {
                closeSocket(c->sock);
                c->sock = kInvalidSocket;
            }
            c->alive.store(false);
        }
    }
    // Join readers outside the mutex (they need to acquire sendMu
    // and clientsMu on their way out).
    std::vector<std::shared_ptr<Client>> drained;
    {
        std::lock_guard<std::mutex> lk(m_clientsMu);
        drained.swap(m_clients);
    }
    for (auto& c : drained) {
        if (c->reader.joinable()) c->reader.join();
    }

    // Clear cached state so a fresh start() doesn't broadcast stale
    // entities.
    {
        std::lock_guard<std::mutex> lk(m_stateMu);
        m_entityCache.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_movesMu);
        m_pendingMoves.clear();
    }
    // Clear any in-flight asset state too. Keeping
    // receiver slots alive across a restart would let a misbehaving
    // client resume a partial upload against a fresh bridge and see
    // stale bytes from the previous session.
    {
        std::lock_guard<std::mutex> lk(m_assetsMu);
        m_assetReceivers.clear();
        m_pendingAssets.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_parentsMu);
        m_pendingParents.clear();
    }
    // Drop any in-flight light requests on shutdown.
    {
        std::lock_guard<std::mutex> lk(m_lightsMu);
        m_pendingLights.clear();
    }
    // Drop in-flight camera + material requests too.
    {
        std::lock_guard<std::mutex> lk(m_camerasMu);
        m_pendingCameras.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_materialsMu);
        m_pendingMaterials.clear();
    }

    wsaShutdown();
    m_port = 0;
    SV_LOG_INFO("net", "EditorBridge stopped");
}

// ── Main-thread pushers ──────────────────────────────────────────

void EditorBridge::setHello(uint32_t clientId,
                             uint32_t avatarEntityId,
                             uint16_t schemaVersion,
                             uint8_t  scope,
                             uint32_t serverSemver,
                             uint8_t  serverState,
                             const std::string& appName) {
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lk(m_stateMu);
        m_helloClientId       = clientId;
        m_helloAvatarEntityId = avatarEntityId;
        m_helloSchemaVersion  = schemaVersion;
        m_helloScope          = scope;
        m_helloSemver         = serverSemver;
        m_helloServerState    = serverState;
        if (!appName.empty()) m_helloAppName = appName;
        frame = buildHelloFrame();
    }
    broadcastFrame(frame);
}

void EditorBridge::setServerState(uint8_t serverState) {
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lk(m_stateMu);
        m_helloServerState = serverState;
        frame = buildServerStateFrame(serverState);
    }
    broadcastFrame(frame);
}

void EditorBridge::pushEntityState(const EditorBridgeEntityState& state) {
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lk(m_stateMu);
        m_entityCache[state.entityId] = state;
        frame = buildEntityStateFrame(state);
    }
    broadcastFrame(frame);
}

void EditorBridge::pushEntityGone(uint32_t entityId) {
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lk(m_stateMu);
        m_entityCache.erase(entityId);
        frame = buildEntityGoneFrame(entityId);
    }
    broadcastFrame(frame);
}

std::vector<EditorBridgeMove> EditorBridge::drainMoves() {
    // Opportunistic reap of any reader threads that died since last
    // frame. We do this here because it's the only main-thread
    // callback that always runs — pushEntityState is only called
    // when an entity changes.
    reapDeadClients();

    std::vector<EditorBridgeMove> out;
    {
        std::lock_guard<std::mutex> lk(m_movesMu);
        out.swap(m_pendingMoves);
    }
    return out;
}

std::vector<EditorBridgeAsset> EditorBridge::drainAssets() {
    std::vector<EditorBridgeAsset> out;
    {
        std::lock_guard<std::mutex> lk(m_assetsMu);
        out.swap(m_pendingAssets);
    }
    return out;
}

std::vector<EditorBridgeParentChange> EditorBridge::drainParentChanges() {
    std::vector<EditorBridgeParentChange> out;
    {
        std::lock_guard<std::mutex> lk(m_parentsMu);
        out.swap(m_pendingParents);
    }
    return out;
}

// Symmetric drain for inbound LightComponent SetField
// requests assembled by the reader thread. Called once per frame by
// TestEngine::pumpBridgeLights() under the same "must run on main
// thread" constraint as drainMoves / drainParentChanges — the main
// thread is the only writer to the QUIC reliable stream.
std::vector<EditorBridgeLightSet> EditorBridge::drainLights() {
    std::vector<EditorBridgeLightSet> out;
    {
        std::lock_guard<std::mutex> lk(m_lightsMu);
        out.swap(m_pendingLights);
    }
    return out;
}

// Same shape as drainLights for the new camera +
// material sidecars. Each entry becomes one full-mask SetField on
// the bridge's avatar.
std::vector<EditorBridgeCameraSet> EditorBridge::drainCameras() {
    std::vector<EditorBridgeCameraSet> out;
    {
        std::lock_guard<std::mutex> lk(m_camerasMu);
        out.swap(m_pendingCameras);
    }
    return out;
}

std::vector<EditorBridgeMaterialSet> EditorBridge::drainMaterials() {
    std::vector<EditorBridgeMaterialSet> out;
    {
        std::lock_guard<std::mutex> lk(m_materialsMu);
        out.swap(m_pendingMaterials);
    }
    return out;
}

size_t EditorBridge::clientCount() const {
    std::lock_guard<std::mutex> lk(m_clientsMu);
    size_t live = 0;
    for (const auto& c : m_clients) {
        if (c->alive.load()) ++live;
    }
    return live;
}

// ── Send path ────────────────────────────────────────────────────

void EditorBridge::sendToClient(Client& client,
                                 const std::vector<uint8_t>& frame) {
    if (!client.alive.load() || client.sock == kInvalidSocket) return;
    std::lock_guard<std::mutex> lk(client.sendMu);
    if (!sendAll(client.sock, frame.data(), frame.size())) {
        client.alive.store(false);
        // Don't close the socket here — the reader thread owns the
        // close path so we don't race on double-close. Marking
        // alive=false prevents further sends.
    } else {
        m_outbound.fetch_add(1, std::memory_order_relaxed);
    }
}

void EditorBridge::broadcastFrame(const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lk(m_clientsMu);
    for (auto& c : m_clients) {
        sendToClient(*c, frame);
    }
}

void EditorBridge::bringClientUpToDate(Client& client) {
    // 1) Hello  2) current ServerState  3) every cached entity
    std::vector<uint8_t> helloFrame;
    std::vector<uint8_t> serverStateFrame;
    std::vector<std::vector<uint8_t>> entityFrames;

    {
        std::lock_guard<std::mutex> lk(m_stateMu);
        helloFrame       = buildHelloFrame();
        serverStateFrame = buildServerStateFrame(m_helloServerState);
        entityFrames.reserve(m_entityCache.size());
        for (const auto& kv : m_entityCache) {
            entityFrames.push_back(buildEntityStateFrame(kv.second));
        }
    }

    sendToClient(client, helloFrame);
    sendToClient(client, serverStateFrame);
    for (const auto& f : entityFrames) {
        sendToClient(client, f);
    }
}

void EditorBridge::reapDeadClients() {
    std::vector<std::shared_ptr<Client>> dead;
    {
        std::lock_guard<std::mutex> lk(m_clientsMu);
        auto it = m_clients.begin();
        while (it != m_clients.end()) {
            if (!(*it)->alive.load()) {
                dead.push_back(*it);
                it = m_clients.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : dead) {
        if (c->sock != kInvalidSocket) {
            closeSocket(c->sock);
            c->sock = kInvalidSocket;
        }
        if (c->reader.joinable() &&
            c->reader.get_id() != std::this_thread::get_id()) {
            c->reader.join();
        } else if (c->reader.joinable()) {
            // Self-reap from the reader thread is not allowed — detach
            // so the thread object can be destroyed cleanly.
            c->reader.detach();
        }
    }
}

// ── Listener thread ──────────────────────────────────────────────

void EditorBridge::listenerThreadBody() {
    while (m_running.load()) {
        SocketHandle listen =
            static_cast<SocketHandle>(m_listenSocket);
        if (listen == kInvalidSocket || listen == 0) break;

        sockaddr_in peer{};
#if defined(_WIN32)
        int peerLen = static_cast<int>(sizeof(peer));
#else
        socklen_t peerLen = sizeof(peer);
#endif
        SocketHandle client = ::accept(listen,
                                       reinterpret_cast<sockaddr*>(&peer),
                                       &peerLen);
        if (client == kInvalidSocket) {
            if (!m_running.load()) break;
            // Transient error — a misconfigured client, for example.
            // Log at info level and keep listening.
            SV_LOG_INFO("net", "EditorBridge accept() returned error %d",
                        lastSocketError());
            continue;
        }

        char addrBuf[64] = {0};
        const uint8_t a = (peer.sin_addr.s_addr        & 0xFFu);
        const uint8_t b = ((peer.sin_addr.s_addr >> 8) & 0xFFu);
        const uint8_t c = ((peer.sin_addr.s_addr >>16) & 0xFFu);
        const uint8_t d = ((peer.sin_addr.s_addr >>24) & 0xFFu);
        std::snprintf(addrBuf, sizeof(addrBuf), "%u.%u.%u.%u:%u",
                      a, b, c, d,
                      static_cast<unsigned>(ntohs(peer.sin_port)));

        auto newClient = std::make_shared<Client>();
        newClient->sock     = client;
        newClient->peerAddr = addrBuf;

        {
            std::lock_guard<std::mutex> lk(m_clientsMu);
            m_clients.push_back(newClient);
        }
        m_clientsAccepted.fetch_add(1, std::memory_order_relaxed);

        // Push current state to the new client before the reader
        // thread starts — avoids a tiny race where a MoveSelf arrives
        // before we've sent Hello.
        bringClientUpToDate(*newClient);

        newClient->reader = std::thread(
            &EditorBridge::clientRxLoopBody, this, newClient);

        SV_LOG_INFO("net", "EditorBridge accepted %s (total=%llu)",
                    addrBuf,
                    static_cast<unsigned long long>(
                        m_clientsAccepted.load()));
    }
}

// ── Reader thread ────────────────────────────────────────────────

void EditorBridge::clientRxLoopBody(std::shared_ptr<Client> client) {
    // Read [u32 len][bytes] frames until the socket closes or we hit
    // a framing error, then mark the client dead and return. The
    // main thread reaps dead clients during drainMoves().
    while (m_running.load() && client->alive.load()) {
        uint8_t lenBytes[4] = {0};
        if (!recvExact(client->sock, lenBytes, sizeof(lenBytes))) break;

        const uint32_t len = readU32LE(lenBytes);
        if (len == 0 || len > kMaxFrameBytes) {
            SV_LOG_WARN("net",
                        "EditorBridge %s: invalid frame length %u — dropping",
                        client->peerAddr.c_str(),
                        static_cast<unsigned>(len));
            break;
        }

        std::vector<uint8_t> frame(len);
        if (!recvExact(client->sock, frame.data(), len)) break;

        m_inbound.fetch_add(1, std::memory_order_relaxed);

        const uint8_t msgType = frame[0];
        const uint8_t* payload = frame.data() + 1;
        const size_t   payloadLen = frame.size() - 1;

        switch (msgType) {
            case kBridgeMsgMoveSelf: {
                if (payloadLen < 7 * 4) break;
                EditorBridgeMove m;
                m.target.posX = readF32LE(payload +  0);
                m.target.posY = readF32LE(payload +  4);
                m.target.posZ = readF32LE(payload +  8);
                m.target.rotX = readF32LE(payload + 12);
                m.target.rotY = readF32LE(payload + 16);
                m.target.rotZ = readF32LE(payload + 20);
                m.target.rotW = readF32LE(payload + 24);
                {
                    std::lock_guard<std::mutex> lk(m_movesMu);
                    m_pendingMoves.push_back(m);
                }
                break;
            }
            case kBridgeMsgPing:
                // Keepalive — no response required; just logged as
                // an inbound count.
                break;

            case kBridgeMsgAssetAnnounce: {
                // [32 hash][u32 size][u8 kind]
                //                   [u16 nameLen][name bytes]
                // Fixed part is 39 bytes; the variable name follows.
                constexpr size_t kFixed = 32 + 4 + 1 + 2;
                if (payloadLen < kFixed) break;
                AssetHash hash{};
                std::memcpy(hash.data(), payload, 32);
                const uint32_t byteSize  = readU32LE(payload + 32);
                const uint8_t  assetKind = payload[36];
                const uint16_t nameLen   = readU16LE(payload + 37);
                if (payloadLen < kFixed + nameLen) break;
                std::string name(
                    reinterpret_cast<const char*>(payload + kFixed),
                    nameLen);
                const std::string hex = digestToHex(hash);
                const uint32_t chunkCount =
                    assetChunkCount(byteSize, 65536);
                {
                    std::lock_guard<std::mutex> lk(m_assetsMu);
                    auto& rx = m_assetReceivers[hex];
                    rx.beginFromAnnounce(hash, byteSize, assetKind,
                                         name, chunkCount, 65536);
                }
                SV_LOG_INFO("net",
                    "EditorBridge %s: AssetAnnounce '%s' (%u bytes, %u chunks)",
                    client->peerAddr.c_str(),
                    name.c_str(),
                    static_cast<unsigned>(byteSize),
                    static_cast<unsigned>(chunkCount));
                break;
            }

            case kBridgeMsgAssetChunk: {
                // [32 hash][u32 chunkIndex]
                //                   [u32 chunkCount][u32 chunkLen]
                //                   [chunkLen bytes]
                constexpr size_t kFixed = 32 + 4 + 4 + 4;
                if (payloadLen < kFixed) break;
                AssetHash hash{};
                std::memcpy(hash.data(), payload, 32);
                const uint32_t chunkIndex = readU32LE(payload + 32);
                (void)readU32LE(payload + 36);   // chunkCount — informational
                const uint32_t chunkLen = readU32LE(payload + 40);
                if (payloadLen < kFixed + chunkLen) break;

                const std::string hex = digestToHex(hash);
                EditorBridgeAsset completed;
                bool finished = false;
                {
                    std::lock_guard<std::mutex> lk(m_assetsMu);
                    auto it = m_assetReceivers.find(hex);
                    if (it == m_assetReceivers.end()) {
                        SV_LOG_WARN("net",
                            "EditorBridge %s: chunk for unknown hash %s...",
                            client->peerAddr.c_str(),
                            hex.substr(0, 12).c_str());
                        break;
                    }
                    AssetReceiver& rx = it->second;
                    if (!rx.depositChunk(chunkIndex,
                                         payload + kFixed,
                                         chunkLen)) {
                        SV_LOG_WARN("net",
                            "EditorBridge %s: depositChunk refused (%u / %u)",
                            client->peerAddr.c_str(),
                            static_cast<unsigned>(chunkIndex),
                            static_cast<unsigned>(rx.chunkCount));
                        m_assetReceivers.erase(it);
                        break;
                    }
                    if (rx.complete && rx.verifyHash()) {
                        completed.hash      = rx.hash;
                        completed.byteSize  = rx.byteSize;
                        completed.assetKind = rx.assetKind;
                        completed.name      = rx.name;
                        completed.bytes     = std::move(rx.assembled);
                        m_assetReceivers.erase(it);
                        finished = true;
                    } else if (rx.complete && !rx.verifyHash()) {
                        SV_LOG_WARN("net",
                            "EditorBridge %s: asset %s... hash mismatch — dropping",
                            client->peerAddr.c_str(),
                            hex.substr(0, 12).c_str());
                        m_assetReceivers.erase(it);
                    }
                }
                if (finished) {
                    {
                        std::lock_guard<std::mutex> lk(m_assetsMu);
                        m_pendingAssets.push_back(std::move(completed));
                    }
                    m_assetsReceived.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }

            case kBridgeMsgSetParent: {
                // [u32 parentEntityId]
                if (payloadLen < 4) break;
                EditorBridgeParentChange pc;
                pc.parentEntityId = readU32LE(payload);
                {
                    std::lock_guard<std::mutex> lk(m_parentsMu);
                    m_pendingParents.push_back(pc);
                }
                m_parentChanges.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            case kBridgeMsgSetLight: {
                // 32-byte LightComponent body —
                //   [u32 type]
                //   [f32 colorR][f32 colorG][f32 colorB]
                //   [f32 intensity]
                //   [f32 range]
                //   [f32 coneInnerDeg][f32 coneOuterDeg]
                if (payloadLen < kBridgeSetLightBodyBytes) {
                    SV_LOG_WARN("net",
                        "EditorBridge %s: SetLight body too short (%u bytes)",
                        client->peerAddr.c_str(),
                        static_cast<unsigned>(payloadLen));
                    break;
                }
                EditorBridgeLightSet ls;
                ls.light.type         = readU32LE(payload + 0);
                ls.light.colorR       = readF32LE(payload + 4);
                ls.light.colorG       = readF32LE(payload + 8);
                ls.light.colorB       = readF32LE(payload + 12);
                ls.light.intensity    = readF32LE(payload + 16);
                ls.light.range        = readF32LE(payload + 20);
                ls.light.coneInnerDeg = readF32LE(payload + 24);
                ls.light.coneOuterDeg = readF32LE(payload + 28);
                {
                    std::lock_guard<std::mutex> lk(m_lightsMu);
                    m_pendingLights.push_back(std::move(ls));
                }
                m_lightsReceived.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            case kBridgeMsgSetCamera: {
                // 16-byte CameraComponent body —
                //   [f32 fovDeg]
                //   [f32 aspect]
                //   [f32 nearPlane]
                //   [f32 farPlane]
                if (payloadLen < kBridgeSetCameraBodyBytes) {
                    SV_LOG_WARN("net",
                        "EditorBridge %s: SetCamera body too short (%u bytes)",
                        client->peerAddr.c_str(),
                        static_cast<unsigned>(payloadLen));
                    break;
                }
                EditorBridgeCameraSet cs;
                cs.camera.fovDeg    = readF32LE(payload + 0);
                cs.camera.aspect    = readF32LE(payload + 4);
                cs.camera.nearPlane = readF32LE(payload + 8);
                cs.camera.farPlane  = readF32LE(payload + 12);
                {
                    std::lock_guard<std::mutex> lk(m_camerasMu);
                    m_pendingCameras.push_back(std::move(cs));
                }
                m_camerasReceived.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            case kBridgeMsgSetMaterial: {
                // 16-byte MaterialComponent body —
                //   [f32 baseColorR][f32 baseColorG][f32 baseColorB]
                //   [f32 overrideStrength]
                if (payloadLen < kBridgeSetMaterialBodyBytes) {
                    SV_LOG_WARN("net",
                        "EditorBridge %s: SetMaterial body too short (%u bytes)",
                        client->peerAddr.c_str(),
                        static_cast<unsigned>(payloadLen));
                    break;
                }
                EditorBridgeMaterialSet ms;
                ms.material.baseColorR       = readF32LE(payload + 0);
                ms.material.baseColorG       = readF32LE(payload + 4);
                ms.material.baseColorB       = readF32LE(payload + 8);
                ms.material.overrideStrength = readF32LE(payload + 12);
                {
                    std::lock_guard<std::mutex> lk(m_materialsMu);
                    m_pendingMaterials.push_back(std::move(ms));
                }
                m_materialsReceived.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            default:
                SV_LOG_WARN("net",
                            "EditorBridge %s: unknown msgType=0x%02X (len=%u)",
                            client->peerAddr.c_str(),
                            static_cast<unsigned>(msgType),
                            static_cast<unsigned>(len));
                break;
        }
    }

    client->alive.store(false);
    // NOTE: we don't close the socket here — the reap path owns the
    // close to avoid a double-close race with stop().
}

} // namespace net
} // namespace sv
