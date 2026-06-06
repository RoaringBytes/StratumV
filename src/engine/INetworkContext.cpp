// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "INetworkContext.h"

namespace sv {

class NoOpNetworkContext : public INetworkContext {
public:
    bool     connect(const char*, uint16_t) override { return false; }
    void     disconnect()                   override {}
    bool     isConnected() const            override { return false; }
    void     tick()                         override {}
    float    getRttMs()        const        override { return 0.f; }
    uint64_t getBytesSent()    const        override { return 0; }
    uint64_t getBytesReceived() const       override { return 0; }
};

std::unique_ptr<INetworkContext> createNoOpNetworkContext() {
    return std::make_unique<NoOpNetworkContext>();
}

} // namespace sv
