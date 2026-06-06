// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── MsQuicTransport ─────────────────────────────────────────────────
// Thin C++ wrapper around the MsQuic C API that hides raw handles
// (HQUIC), the event-callback machinery, and the MsQuic headers from
// public StratumV consumers. Lives at the platform-module tier just
// below the Layer 4 replication substrate (see ARCHITECTURE.md §6 and
// NETWORK_DESIGN.md §9).
//
// This layer is the "bytes leave the process" boundary. All it
// proves is that a TLS 1.3 QUIC loopback handshake completes between
// two Transport instances. No game state, no snapshot payload, no
// stream I/O beyond what the handshake negotiates. Higher layers add
// the headless stratumv_server target, interest management,
// interpolation, and snapshot exchange on top of this transport.
//
// Public guarantees:
//   1. Callers do not need <msquic.h> or <msquic.hpp>. This header
//      only pulls in <cstdint>/<memory>/<mutex>/<condition_variable>/
//      <string>/<functional>/<optional>.
//   2. Transport, Listener, and Connection are RAII, non-copyable, and
//      movable. Destructors tear everything down cleanly.
//   3. All state observed by tests is surfaced through Connection::
//      stats() / waitForConnected() / waitForDisconnected(). MsQuic's
//      worker threads never touch test code directly.
//   4. The server side supports only an in-process
//      self-signed certificate (useSelfSignedLoopbackCert). Real
//      certificate management is deferred (see
//      NETWORK_DESIGN.md §5.3).
//
// The implementation lives in MsQuicTransport.cpp, which is the only
// translation unit allowed to #include <msquic.h>/<msquic.hpp>.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sv::net {

// ── Error codes ─────────────────────────────────────────────────────
// Deliberately small + flat. The intent is for callers to branch on
// "ok or not" and log the string form. Finer-grained error reporting
// arrives with the Layer-4 replication error model.
enum class TransportStatus : int32_t {
    Ok                  = 0,
    InvalidArg          = -1,
    NotStarted          = -2,
    AlreadyStarted      = -3,
    InitFailed          = -4,      // MsQuicOpen2 / API table fetch failed
    RegistrationFailed  = -5,      // QUIC_API_TABLE::RegistrationOpen failed
    CredentialFailed    = -6,      // Self-signed cert build / cred load failed
    ConfigurationFailed = -7,      // QUIC_API_TABLE::ConfigurationOpen failed
    ListenerFailed      = -8,      // ListenerOpen / ListenerStart failed
    ConnectFailed       = -9,      // ConnectionOpen / ConnectionStart failed
    MsQuicMissing       = -10,     // Runtime DLL absent (compile-time guard off)
};

const char* transportStatusToString(TransportStatus s);

// ── Connection event state ──────────────────────────────────────────
// Tests observe this via Connection::stats(). Updates happen on
// MsQuic worker threads and are protected by Connection's internal
// mutex. stats() returns a copy, which is safe to read from the
// test thread without extra locking.
struct ConnectionStats {
    bool        connected       = false;    // QUIC_CONNECTION_EVENT_CONNECTED fired
    bool        shutdownStarted = false;    // SHUTDOWN_INITIATED_BY_* fired
    bool        shutdownComplete= false;    // SHUTDOWN_COMPLETE fired
    uint64_t    errorCode       = 0;        // Peer or transport code (post-shutdown)
    std::string peerAddress;                 // "127.0.0.1:54321" after Connected
    std::string negotiatedAlpn;              // e.g. "stratumv/1"

    // Datagram counters. Updated from MsQuic worker threads,
    // read via Connection::stats(). datagramsSent counts successful
    // DatagramSend() invocations (it does NOT wait for peer ack).
    // datagramsReceived counts DATAGRAM_RECEIVED events delivered.
    uint64_t    datagramsSent     = 0;
    uint64_t    datagramBytesSent = 0;
    uint64_t    datagramsReceived = 0;
    uint64_t    datagramBytesReceived = 0;

    // Reliable-message counters. reliableMessagesSent is
    // incremented when sendReliableMessage() hands a buffer off to
    // MsQuic (not on delivery). reliableMessagesReceived is incremented
    // once per peer-initiated unidirectional stream that reached its
    // PEER_SEND_SHUTDOWN (FIN) event — i.e. once per fully assembled
    // inbound reliable message.
    uint64_t    reliableMessagesSent     = 0;
    uint64_t    reliableBytesSent        = 0;
    uint64_t    reliableMessagesReceived = 0;
    uint64_t    reliableBytesReceived    = 0;
};

// ── Datagram handler ────────────────────────────────────────────────
// Callback signature for inbound unreliable datagrams.
// Invoked from a MsQuic worker thread, so the handler must be
// thread-safe. Convention: stuff received bytes into a locked queue
// and drain on the main thread during the game tick.
// The buffer is owned by MsQuic and valid only for the duration of
// the callback — handlers that need to keep the bytes must copy.
using DatagramHandler =
    std::function<void(const uint8_t* data, size_t size)>;

// ── Reliable message handler ────────────────────────────────────────
// Callback signature for fully assembled reliable messages. One
// message corresponds to one peer-initiated unidirectional QUIC
// stream that has received its FIN (QUIC_STREAM_EVENT_PEER_SEND_
// SHUTDOWN). The entire accumulated payload is delivered in a single
// call — Connection handles segment reassembly. Invoked from a
// MsQuic worker thread, so the handler must be thread-safe. The
// buffer is owned by Connection and valid only for the duration of
// the call. Handlers that need to keep the bytes must copy.
using ReliableMessageHandler =
    std::function<void(const uint8_t* data, size_t size)>;

// ── Connection (one QUIC session) ───────────────────────────────────
class Connection {
public:
    Connection();
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    // True once the underlying HQUIC has been allocated. Does NOT imply
    // handshake complete — check stats().connected for that.
    bool valid() const;

    // Thread-safe snapshot of current state.
    ConnectionStats stats() const;

    // Block until stats().connected is true, or timeout. Safe to call
    // from the test thread. Returns false on timeout.
    bool waitForConnected(uint32_t timeoutMs) const;

    // Block until stats().shutdownComplete is true, or timeout.
    bool waitForShutdownComplete(uint32_t timeoutMs) const;

    // Initiate a graceful application-level shutdown. Idempotent. The
    // Connection remains valid() until destruction; stats() reports
    // shutdownStarted immediately and shutdownComplete once the peer
    // has acknowledged.
    void shutdown(uint64_t errorCode = 0);

    // ── Datagram API ─────────────────────────────────────────────────
    // Send an unreliable datagram. The buffer is copied into an
    // internally-owned holder; callers may reuse / free their buffer
    // immediately after sendDatagram returns. Returns true if MsQuic
    // accepted the datagram for queueing; false on "not connected",
    // "datagrams disabled", over-MTU, or any MsQuic-level error.
    // Success does NOT mean the datagram reached the peer — QUIC
    // datagrams are fire-and-forget.
    bool sendDatagram(const uint8_t* data, size_t size);

    // Install a handler invoked when a datagram arrives on this
    // connection. The handler runs on a MsQuic worker thread and
    // must be thread-safe. The buffer is valid only for the duration
    // of the call — handlers that need the bytes later must copy.
    // Pass nullptr to clear. Safe to call before or after the
    // handshake completes.
    void setDatagramHandler(DatagramHandler handler);

    // ── Reliable message API ─────────────────────────────────────────
    // Send a reliable message to the peer on a fresh unidirectional
    // QUIC stream. The stream is opened, the bytes are sent with a
    // trailing FIN, and the stream is closed cooperatively. The peer
    // sees a single invocation of its ReliableMessageHandler once all
    // bytes have arrived and the FIN has been observed.
    //
    // The buffer is copied into an internally-owned holder; callers
    // may reuse / free their buffer immediately after the call
    // returns. Returns true on successful queue (not delivery):
    //   * MsQuic accepted the StreamOpen + StreamSend calls
    //   * The Connection is in `connected` state and not shutting down
    //
    // Returns false on "not connected", stream open failure, or any
    // MsQuic-level error. Unlike sendDatagram, reliable messages are
    // NOT dropped on the wire — QUIC's congestion control will pace
    // delivery as needed.
    bool sendReliableMessage(const uint8_t* data, size_t size);

    // Install a handler invoked once for each fully-received peer-
    // initiated unidirectional stream. The handler runs on a MsQuic
    // worker thread and must be thread-safe. The buffer is valid only
    // for the duration of the call — handlers that need the bytes
    // later must copy. Pass nullptr to clear. Safe to call before or
    // after the handshake completes; the Connection buffers any
    // in-flight stream state until the handler is installed and
    // resolved at PEER_SEND_SHUTDOWN time.
    void setReliableMessageHandler(ReliableMessageHandler handler);

    // Impl is forward-declared publicly so the MsQuic callback in the
    // .cpp can name it. The definition lives entirely in the .cpp so
    // nothing leaks.
    struct Impl;

private:
    friend class Transport;
    friend class Listener;
    std::unique_ptr<Impl> m_impl;
};

// ── Listener (accepts incoming connections) ─────────────────────────
class Listener {
public:
    Listener();
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) noexcept;
    Listener& operator=(Listener&&) noexcept;

    bool valid() const;

    // The actual locally-bound port. 0 if not yet started. When
    // Transport::startListener is called with port == 0, the kernel
    // picks an ephemeral port and this returns the picked value.
    uint16_t localPort() const;

    // Block up to `timeoutMs` for an incoming connection. On success
    // the returned Connection has its HQUIC installed and the MsQuic
    // configuration applied; the handshake then proceeds in the
    // background. Callers should follow with waitForConnected().
    //
    // On timeout or shutdown, returns a Connection whose valid() is
    // false.
    Connection acceptOne(uint32_t timeoutMs);

    // Stop accepting new connections. Already-accepted ones stay valid.
    void stop();

    // Publicly forward-declared so the MsQuic listener callback in the
    // .cpp can build a Connection::Impl and stash it in the accept queue.
    struct Impl;

private:
    friend class Transport;
    std::unique_ptr<Impl> m_impl;
};

// ── Transport (owns the MsQuic process-wide state) ──────────────────
class Transport {
public:
    struct Config {
        // Application layer protocol negotiation string. Both peers
        // must agree. "stratumv/1" is the default; consumers can
        // override for compatibility testing.
        std::string alpn = "stratumv/1";

        // Connection-level idle timeout in ms. 0 leaves MsQuic's
        // default in place (30000 ms as of 2.5.6).
        uint64_t    idleTimeoutMs = 30000;

        // Server-side: build a one-shot in-process self-signed cert
        // and use it as the listener's credential. Required for
        // startListener to succeed. Real cert support
        // (PEM file, Windows cert store lookup) is deferred.
        bool useSelfSignedLoopbackCert = false;

        // Client-side: skip certificate validation. REQUIRED for the
        // peer side of a self-signed loopback setup, since the test
        // cert chain doesn't anchor to a trusted root. DO NOT set
        // this flag in production code paths.
        bool clientInsecureNoVerify = false;

        // Registration name shown in MsQuic diagnostics. Cosmetic.
        std::string appName = "stratumv";
    };

    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    // Bring up MsQuic: load the API table, create a Registration,
    // build the credential configurations needed for the roles this
    // Transport will play. Idempotent via the AlreadyStarted error:
    // calling start twice is a programming bug, not silent noop.
    TransportStatus start(const Config& cfg);

    // Tear down every resource this Transport owns. Safe to call from
    // a destructor path even if start failed. Idempotent.
    void stop();

    // True between start() and stop().
    bool started() const;

    // Server side: bind a listener on 127.0.0.1 (or any local bind
    // address) at `port`. Passing port == 0 asks the kernel for an
    // ephemeral port, which Listener::localPort() then exposes.
    // Requires cfg.useSelfSignedLoopbackCert == true.
    TransportStatus startListener(uint16_t port, Listener& out);

    // Client side: initiate a connection to the remote peer. Returns
    // immediately once the QUIC handshake has been scheduled; the
    // handshake itself runs asynchronously and completes via the
    // event callback. Block for completion via
    // Connection::waitForConnected.
    TransportStatus connect(const std::string& host,
                            uint16_t port,
                            Connection& out);

    // Runtime-linked MsQuic version string (e.g. "2.5.6"). Empty if
    // STRATUMV_ENABLE_MSQUIC was off at compile time.
    static std::string msquicVersionString();

    // Compile-time guard: true if stratumv.lib was built with
    // STRATUMV_ENABLE_MSQUIC=ON. False means all of Transport's methods
    // return MsQuicMissing and the implementation is a stub.
    static bool isMsquicAvailable();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sv::net
