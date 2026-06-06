// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── MsQuicTransport unit/integration tests ──────────────────
// Covers the first StratumV session where bytes actually leave the
// process. Exercises:
//   1. The pure-lookup surfaces (transportStatusToString,
//      msquicVersionString, isMsquicAvailable) without any sockets.
//   2. Transport lifecycle (start, stop, double-start guard).
//   3. Listener on ephemeral port gets a real bound port.
//   4. Client -> server loopback TLS 1.3 QUIC handshake completes on
// both ends (this is THE test for — if this is green,
//      the session goal is met).
//   5. Peer addresses exchanged at CONNECTED event are loopback.
//   6. Graceful shutdown propagates to both ends.
//   7. connect() on a Transport that hasn't been started reports
//      NotStarted cleanly instead of crashing.
//
// Timeouts: we use generous 5s waits on loopback; MsQuic typically
// completes a handshake in a few ms locally, so 5s covers slow CI
// machines and cert generation warm-up without flaking on fast ones.

#include "net/MsQuicTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using sv::net::Connection;
using sv::net::ConnectionStats;
using sv::net::Listener;
using sv::net::Transport;
using sv::net::TransportStatus;
using sv::net::transportStatusToString;

namespace {

constexpr uint32_t kHandshakeTimeoutMs = 5000;
constexpr uint32_t kShutdownTimeoutMs  = 5000;
constexpr uint32_t kAcceptTimeoutMs    = 5000;

// Start a server Transport configured to accept self-signed loopback
// connections. Returns TransportStatus::Ok on success. Centralised so
// every test gets the same cfg.
TransportStatus startServerTransport(Transport& t) {
    Transport::Config cfg;
    cfg.alpn                      = "stratumv/1";
    cfg.idleTimeoutMs             = 30000;
    cfg.useSelfSignedLoopbackCert = true;
    cfg.appName                   = "stratumv-test-server";
    return t.start(cfg);
}

TransportStatus startClientTransport(Transport& t) {
    Transport::Config cfg;
    cfg.alpn                      = "stratumv/1";
    cfg.idleTimeoutMs             = 30000;
    cfg.clientInsecureNoVerify    = true;  // self-signed loopback cert
    cfg.appName                   = "stratumv-test-client";
    return t.start(cfg);
}

} // namespace (anonymous)

// ── 1. Pure-lookup tests (no MsQuic handles) ────────────────────────

TEST_CASE("MsQuicTransport: transportStatusToString covers every enum value", "[msquic][status]") {
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::Ok))                  == "Ok");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::InvalidArg))          == "InvalidArg");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::NotStarted))          == "NotStarted");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::AlreadyStarted))      == "AlreadyStarted");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::InitFailed))          == "InitFailed");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::RegistrationFailed))  == "RegistrationFailed");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::CredentialFailed))    == "CredentialFailed");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::ConfigurationFailed)) == "ConfigurationFailed");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::ListenerFailed))      == "ListenerFailed");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::ConnectFailed))       == "ConnectFailed");
    REQUIRE(std::string_view(transportStatusToString(TransportStatus::MsQuicMissing))       == "MsQuicMissing");
}

TEST_CASE("MsQuicTransport: isMsquicAvailable and version string are populated", "[msquic][status]") {
    REQUIRE(Transport::isMsquicAvailable() == true);
    const std::string version = Transport::msquicVersionString();
    REQUIRE_FALSE(version.empty());
    // Pinned to 2.5.6 in CMakeLists.txt (the FetchContent URL). When
    // that URL is bumped, both the pin and this assertion move in
    // lockstep.
    REQUIRE(version == "2.5.6");
}

// ── 2. Transport lifecycle ──────────────────────────────────────────

TEST_CASE("MsQuicTransport: server-role start/stop lifecycle", "[msquic][lifecycle]") {
    Transport t;
    REQUIRE_FALSE(t.started());

    REQUIRE(startServerTransport(t) == TransportStatus::Ok);
    REQUIRE(t.started());

    t.stop();
    REQUIRE_FALSE(t.started());

    // Re-start must succeed again (start/stop are a balanced pair, not
    // a one-shot).
    REQUIRE(startServerTransport(t) == TransportStatus::Ok);
    REQUIRE(t.started());
    t.stop();
}

TEST_CASE("MsQuicTransport: calling start twice reports AlreadyStarted", "[msquic][lifecycle]") {
    Transport t;
    REQUIRE(startServerTransport(t) == TransportStatus::Ok);
    REQUIRE(startServerTransport(t) == TransportStatus::AlreadyStarted);
    t.stop();
}

TEST_CASE("MsQuicTransport: connect on an unstarted Transport returns NotStarted", "[msquic][lifecycle]") {
    Transport t;
    Connection c;
    REQUIRE(t.connect("127.0.0.1", 5555, c) == TransportStatus::NotStarted);
    // Transport never acquired MsQuic state, so no stop() needed.
}

// ── 3. Listener binds + reports ephemeral port ──────────────────────

TEST_CASE("MsQuicTransport: listener on port 0 resolves to a non-zero ephemeral port", "[msquic][listener]") {
    Transport server;
    REQUIRE(startServerTransport(server) == TransportStatus::Ok);

    {
        Listener listener;
        REQUIRE(server.startListener(0, listener) == TransportStatus::Ok);
        REQUIRE(listener.valid());
        REQUIRE(listener.localPort() != 0);
    }  // listener destroyed before server.stop()

    server.stop();
}

// ── 4. Full loopback handshake (the headline test) ──────────

TEST_CASE("MsQuicTransport: loopback TLS 1.3 QUIC handshake completes on both ends", "[msquic][handshake]") {
    // Scoped so destruction order is serverConn -> clientConn ->
    // listener -> client -> server (reverse of creation). Transport
    // destructors block on any live connections/listeners inside the
    // same registration, so this order is load-bearing.
    Transport server;
    REQUIRE(startServerTransport(server) == TransportStatus::Ok);

    Transport client;
    REQUIRE(startClientTransport(client) == TransportStatus::Ok);

    {
        Listener listener;
        REQUIRE(server.startListener(0, listener) == TransportStatus::Ok);
        const uint16_t port = listener.localPort();
        REQUIRE(port != 0);

        {
            Connection clientConn;
            REQUIRE(client.connect("127.0.0.1", port, clientConn) == TransportStatus::Ok);

            // Wait for the server side to see the incoming connection.
            Connection serverConn = listener.acceptOne(kAcceptTimeoutMs);
            REQUIRE(serverConn.valid());

            // Both ends must observe QUIC_CONNECTION_EVENT_CONNECTED.
            // This is the assertion the whole session exists to land.
            REQUIRE(clientConn.waitForConnected(kHandshakeTimeoutMs));
            REQUIRE(serverConn.waitForConnected(kHandshakeTimeoutMs));

            const ConnectionStats cs = clientConn.stats();
            const ConnectionStats ss = serverConn.stats();
            REQUIRE(cs.connected);
            REQUIRE(ss.connected);

            // Peer addresses should be loopback strings. MsQuic may
            // report the server-visible client address as
            // "127.0.0.1:<ephemeral>", and the client-visible server
            // address as "127.0.0.1:<listener port>".
            REQUIRE_FALSE(cs.peerAddress.empty());
            REQUIRE_FALSE(ss.peerAddress.empty());
            REQUIRE(cs.peerAddress.rfind("127.0.0.1:", 0) == 0);
            REQUIRE(ss.peerAddress.rfind("127.0.0.1:", 0) == 0);

            // ALPN round-trip: negotiated ALPN reported on CONNECTED
            // should match what we configured on start().
            REQUIRE(cs.negotiatedAlpn == "stratumv/1");
            REQUIRE(ss.negotiatedAlpn == "stratumv/1");

            // Graceful shutdown from the client side. Both ends should
            // eventually observe QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE.
            clientConn.shutdown(0);
            REQUIRE(clientConn.waitForShutdownComplete(kShutdownTimeoutMs));
            REQUIRE(serverConn.waitForShutdownComplete(kShutdownTimeoutMs));

            const ConnectionStats cs2 = clientConn.stats();
            const ConnectionStats ss2 = serverConn.stats();
            REQUIRE(cs2.shutdownComplete);
            REQUIRE(ss2.shutdownComplete);
        }  // Connection destructors fire here
    }  // Listener destructor fires here

    client.stop();
    server.stop();
}

// ── 5. Reliable-message round-trip ───────────────────
// Opens a real loopback connection, installs a reliable-message
// handler on the client, sends one reliable message from the server,
// and verifies the handler fires with the exact bytes. The stream
// path is internal to MsQuicTransport.cpp — this is the ONLY test
// that exercises the unidirectional-stream callback plumbing with a
// real MsQuic handshake. The pure-logic preamble encode/parse cases
// live in tests/test_ReplicationWire.cpp.

TEST_CASE("MsQuicTransport: reliable-message loopback round-trip delivers the full payload",
          "[msquic][reliable]") {
    Transport server;
    REQUIRE(startServerTransport(server) == TransportStatus::Ok);

    Transport client;
    REQUIRE(startClientTransport(client) == TransportStatus::Ok);

    {
        Listener listener;
        REQUIRE(server.startListener(0, listener) == TransportStatus::Ok);
        const uint16_t port = listener.localPort();
        REQUIRE(port != 0);

        {
            Connection clientConn;
            REQUIRE(client.connect("127.0.0.1", port, clientConn) == TransportStatus::Ok);

            Connection serverConn = listener.acceptOne(kAcceptTimeoutMs);
            REQUIRE(serverConn.valid());
            REQUIRE(clientConn.waitForConnected(kHandshakeTimeoutMs));
            REQUIRE(serverConn.waitForConnected(kHandshakeTimeoutMs));

            // Shared inbox for the client's reliable handler. The
            // MsQuic worker thread writes under the lock; the test
            // thread polls a copy via a deadline wait.
            std::mutex                mu;
            std::condition_variable   cv;
            std::vector<uint8_t>      received;
            bool                      gotMessage = false;

            clientConn.setReliableMessageHandler(
                [&](const uint8_t* data, size_t size) {
                    std::lock_guard<std::mutex> lk(mu);
                    received.assign(data, data + size);
                    gotMessage = true;
                    cv.notify_all();
                });

            // Distinctive payload: the header byte 0x02 is the -
            // MIN-a schema-handshake marker, so a failing test still
            // tells a sensible story ("expected 0x02 wasn't delivered").
            const std::vector<uint8_t> payload = {
                0x02, 0xDE, 0xAD, 0xBE, 0xEF,
                0x00, 0x00, 0x01, 0x02,
                0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x00, 0x2A
            };
            REQUIRE(serverConn.sendReliableMessage(payload.data(), payload.size()));

            // Deadline wait: 5 s is generous for a ~17-byte message
            // on loopback, but it covers cold MsQuic worker spin-up.
            {
                std::unique_lock<std::mutex> lk(mu);
                const bool delivered = cv.wait_for(
                    lk,
                    std::chrono::milliseconds(kHandshakeTimeoutMs),
                    [&]() { return gotMessage; });
                REQUIRE(delivered);
                REQUIRE(received == payload);
            }

            // Counters should reflect the one send + one receive.
            const ConnectionStats serverStats = serverConn.stats();
            const ConnectionStats clientStats = clientConn.stats();
            REQUIRE(serverStats.reliableMessagesSent     == 1u);
            REQUIRE(serverStats.reliableBytesSent        == payload.size());
            REQUIRE(clientStats.reliableMessagesReceived == 1u);
            REQUIRE(clientStats.reliableBytesReceived    == payload.size());

            // Tear down cleanly so RegistrationClose is not blocked by
            // any lingering stream state.
            clientConn.shutdown(0);
            REQUIRE(clientConn.waitForShutdownComplete(kShutdownTimeoutMs));
            REQUIRE(serverConn.waitForShutdownComplete(kShutdownTimeoutMs));
        }  // Connection destructors fire here
    }  // Listener destructor fires here

    client.stop();
    server.stop();
}

// ── 6. acceptOne timeout returns an invalid Connection ──────────────
// Sanity check for Listener::acceptOne with no inbound traffic. The
// listener is scoped inside an inner block so it gets destroyed
// (ListenerClose) before server.stop() runs — otherwise
// RegistrationClose blocks waiting for the still-open listener, which
// looks exactly like a hang to CTest. Every test in this file that
// explicitly calls server.stop() must apply the same ordering rule.

TEST_CASE("MsQuicTransport: acceptOne with no inbound returns an invalid Connection", "[msquic][listener]") {
    Transport server;
    REQUIRE(startServerTransport(server) == TransportStatus::Ok);

    {
        Listener listener;
        REQUIRE(server.startListener(0, listener) == TransportStatus::Ok);

        // 200 ms is enough to prove the wait_for times out without
        // bloating overall test runtime. Anything shorter and Catch2
        // reports the test in micro time; anything longer is wasteful.
        Connection c = listener.acceptOne(200);
        REQUIRE_FALSE(c.valid());

        listener.stop();
    }  // Listener destructor runs before server.stop()

    server.stop();
}
