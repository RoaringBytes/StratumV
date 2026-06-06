// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// INetworkContext — abstract networking interface for StratumV games.
//
// Lives at Layer 4 (Engine Services). Games wire a concrete implementation
// into BaseSystemContext::network. Use createNoOpNetworkContext() when
// networking is not yet implemented.
//
// The authoritative game server is a separate binary — this interface
// covers client-side connection and message transport only.

#include <cstdint>
#include <memory>

namespace sv {

class INetworkContext {
public:
    virtual ~INetworkContext() = default;

    // ── Connection lifecycle ────────────────────────────────────────
    virtual bool connect(const char* host, uint16_t port) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // ── Per-frame update ────────────────────────────────────────────
    // Drains the receive queue, flushes outgoing messages.
    virtual void tick() = 0;

    // ── Diagnostics ─────────────────────────────────────────────────
    virtual float    getRttMs()        const = 0;  // round-trip time in ms
    virtual uint64_t getBytesSent()    const = 0;
    virtual uint64_t getBytesReceived() const = 0;
};

// No-op implementation — connect() always returns false, all stats zero.
std::unique_ptr<INetworkContext> createNoOpNetworkContext();

} // namespace sv
