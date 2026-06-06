// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── NetworkContext sub-struct shape tests ───────────────────
// Pins the semantics of the BaseSystemContext refactor:
//
//   * The flat `INetworkContext* network` slot that lived on
// BaseSystemContext from (2025) through 1.2.x moved INTO
//     a new `NetworkContext` sub-struct as `network.context`.
//   * `ctx.network` is now a compound sub-struct, not a pointer.
//     `ctx.network->tick()` no longer compiles; consumers must write
//     `ctx.network.context->tick()` after the 1.3.0 bump.
//   * `ctx.perf.network` (NetworkStats observability block) stays
//     put — active services go under `ctx.network.*`, passive runtime
//     counters stay under `ctx.perf.network.*`. This split is load-
//     bearing for the AdminPanel HUD and documented in
//     docs/NETWORK_DESIGN.md §7.
//
// The tests also include a "server process-equivalent" smoke test that
// spins up the same MsQuic server-role Transport + Listener pair that
// src/stratumv_server/main.cpp uses, verifying it can start, bind an
// ephemeral port, and shut down cleanly from inside the test harness.
// No handshake is exercised here — that's test_MsQuicTransport.cpp's
// job. The point is to prove the dedicated-server code path can be
// reached without launching an actual OS process, which matters for
// future CI runs.

#include "BaseSystemContext.h"
#include "INetworkContext.h"
#include "net/MsQuicTransport.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <type_traits>

using sv::BaseSystemContext;
using sv::INetworkContext;
using sv::NetworkContext;

// ── 1. Shape + default state ────────────────────────────────────────

TEST_CASE("NetworkContext: default-constructed has null context pointer",
          "[network][shape]") {
    NetworkContext nc{};
    REQUIRE(nc.context == nullptr);
}

TEST_CASE("NetworkContext: default-constructible POD semantics",
          "[network][shape]") {
    // The sub-struct is a plain POD: default-constructible so the
    // scaffold `BaseSystemContext ctx{};` pattern keeps working, and
    // standard-layout so the DLL ABI boundary stays simple.
    static_assert(std::is_default_constructible_v<NetworkContext>,
                  "NetworkContext must be default-constructible");
    static_assert(std::is_standard_layout_v<NetworkContext>,
                  "NetworkContext must be standard layout (DLL boundary)");

    NetworkContext nc{};
    REQUIRE(nc.context == nullptr);

    // Writing the slot is the only legal mutation in 1.3.0.
    nc.context = reinterpret_cast<INetworkContext*>(std::uintptr_t{0xFEEDBEEF});
    REQUIRE(nc.context != nullptr);

    // Reset is trivial value-init — no destructor gymnastics.
    nc = NetworkContext{};
    REQUIRE(nc.context == nullptr);
}

// ── 2. BaseSystemContext.network is a sub-struct (not a raw ptr) ────

TEST_CASE("BaseSystemContext: network is a NetworkContext sub-struct",
          "[network][shape][refactor]") {
    // refactor invariant: the member is a compound value, not a
    // pointer. If someone reverts to `INetworkContext* network` this
    // static_assert is what catches it — the member type changes, and
    // std::is_same_v flips to false.
    static_assert(std::is_same_v<decltype(BaseSystemContext::network),
                                 NetworkContext>,
                  "BaseSystemContext::network must be a NetworkContext "
                  "sub-struct as of (1.3.0), not a raw pointer");

    BaseSystemContext ctx{};
    REQUIRE(ctx.network.context == nullptr);
}

// ── 3. NoOp implementation wires through the new slot ──────────────

TEST_CASE("BaseSystemContext: network.context accepts NoOpNetworkContext",
          "[network][noop]") {
    BaseSystemContext ctx{};

    auto noOp = sv::createNoOpNetworkContext();
    REQUIRE(noOp != nullptr);

    ctx.network.context = noOp.get();
    REQUIRE(ctx.network.context == noOp.get());

    // Calling the interface through the sub-struct path should behave
    // exactly as it did when the slot was flat. The NoOp impl returns
    // false/0 for everything — the point here is to prove the call
    // compiles and dispatches through the vtable without UB.
    REQUIRE_FALSE(ctx.network.context->isConnected());
    REQUIRE_FALSE(ctx.network.context->connect("127.0.0.1", 9001));
    REQUIRE(ctx.network.context->getRttMs() == 0.0f);
    REQUIRE(ctx.network.context->getBytesSent() == 0u);
    REQUIRE(ctx.network.context->getBytesReceived() == 0u);

    // tick() is a no-op; calling it should not mutate anything observable.
    ctx.network.context->tick();
    REQUIRE_FALSE(ctx.network.context->isConnected());

    // Disconnect is idempotent on a NoOp impl.
    ctx.network.context->disconnect();
    REQUIRE_FALSE(ctx.network.context->isConnected());
}

// ── 4. ctx.network.context is independent of ctx.perf.network.* ─────

TEST_CASE("BaseSystemContext: network.context does not alias perf.network.*",
          "[network][shape][perf]") {
    BaseSystemContext ctx{};

    // Both sub-structs happen to be named `network` at their parent
    // level. This test locks in the split: writing one must never
    // leak into the other. If a future refactor collapses them, this
    // test fires first.
    auto noOp = sv::createNoOpNetworkContext();
    ctx.network.context             = noOp.get();
    ctx.perf.network.bytesPerSec    = 4096;
    ctx.perf.network.tickMs         = 16.7f;
    ctx.perf.network.packetsPerSec  = 120;

    REQUIRE(ctx.network.context == noOp.get());
    REQUIRE(ctx.perf.network.bytesPerSec == 4096u);
    REQUIRE(ctx.perf.network.tickMs == 16.7f);
    REQUIRE(ctx.perf.network.packetsPerSec == 120u);

    // Reset network.context; perf.network.* must stay put.
    ctx.network.context = nullptr;
    REQUIRE(ctx.network.context == nullptr);
    REQUIRE(ctx.perf.network.bytesPerSec == 4096u);
    REQUIRE(ctx.perf.network.tickMs == 16.7f);

    // Reset perf.network.*; network.context must stay at nullptr (we
    // already cleared it, but the key is that clearing perf.network
    // doesn't accidentally fill network.context back in).
    ctx.perf.network = sv::NetworkStats{};
    REQUIRE(ctx.perf.network.bytesPerSec == 0u);
    REQUIRE(ctx.perf.network.tickMs == 0.0f);
    REQUIRE(ctx.network.context == nullptr);
}

// ── 5. Server process-equivalent: Transport + Listener lifecycle ────
//
// Proves that the exact MsQuic bring-up sequence used by
// src/stratumv_server/main.cpp works from inside the test harness.
// The server main does:
//   1. Transport::isMsquicAvailable() gate
//   2. Transport::start(serverCfg)
//   3. Transport::startListener(port, listener)
//   4. loop on listener.acceptOne(timeout)
//   5. Listener destructor before Transport::stop()
//
// We skip step 4 (no connection traffic) and prove 1-3 + 5 run cleanly.
// This lets future CI jobs reach the server path without spawning a
// child process.

#ifdef STRATUMV_MSQUIC_AVAILABLE

TEST_CASE("stratumv_server process-equivalent: Transport + Listener lifecycle",
          "[network][server][msquic]") {
    using sv::net::Connection;
    using sv::net::Listener;
    using sv::net::Transport;
    using sv::net::TransportStatus;

    REQUIRE(Transport::isMsquicAvailable());

    Transport server;
    Transport::Config cfg;
    cfg.alpn                      = "stratumv/1";
    cfg.idleTimeoutMs             = 10000;
    cfg.useSelfSignedLoopbackCert = true;
    cfg.appName                   = "stratumv_server_test";

    REQUIRE(server.start(cfg) == TransportStatus::Ok);
    REQUIRE(server.started());

    {
        // Inner scope: Listener must destruct before server.stop() or
        // RegistrationClose blocks forever on the live child handle.
        Listener listener;
        REQUIRE(server.startListener(0, listener) == TransportStatus::Ok);
        REQUIRE(listener.valid());

        // Ephemeral port resolves to a real kernel-assigned value.
        const uint16_t port = listener.localPort();
        REQUIRE(port != 0);

        // acceptOne on an empty queue returns a short-timeout invalid
        // Connection — the same state the real server sees between
        // incoming peers.
        Connection conn = listener.acceptOne(50);
        REQUIRE_FALSE(conn.valid());
    } // Listener destructor fires here

    server.stop();
    REQUIRE_FALSE(server.started());
}

#endif // STRATUMV_MSQUIC_AVAILABLE
