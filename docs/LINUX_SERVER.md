# StratumV — Headless Linux Server

This document describes how to build and run `stratumv_server` on a
Linux host (tested target: Debian 12 or Ubuntu 22.04 on a Proxmox VM)
using the `stratumv_core` carve-out.

`stratumv_core` is the Layer 4 networking + replication subset of
StratumV. It contains only the translation units a headless dedicated
server actually needs (`EngineLog`, `INetworkContext`,
`ReplicationRegistry`, `NetTransform`, `ReplicationProtocol`,
`MsQuicTransport`). No Vulkan, glslang, glm, ozz-animation, ImGui,
miniaudio, tinygltf, meshoptimizer, or Jolt is pulled in. The full
engine library (`stratumv`) remains Windows-only for now — Linux
graphics support is future work.

> **Status:** first-drop Linux bring-up (2026-04-10). The CMake wiring
> is exercised in CI via `.github/workflows/build.yml`'s `linux-core`
> matrix but has NOT been interactively smoke-tested on a real Proxmox
> VM yet. If you hit a pothole during first manual run, please report it.

---

## 1. Prerequisites

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config \
    libssl-dev
```

Minimum versions:

| Tool    | Min    | Why                                              |
|---------|--------|--------------------------------------------------|
| cmake   | 3.24   | `FetchContent_MakeAvailable` + generator-expression helpers |
| g++     | 11     | C++20 baseline (concepts, `std::filesystem`)    |
| OpenSSL | 3.0    | Self-signed cert generator + MsQuic TLS backend  |

Debian 12 bookworm ships cmake 3.25, g++ 12, OpenSSL 3.0 — all
adequate. Ubuntu 22.04 ships cmake 3.22 which is too old; install a
newer cmake from `apt.kitware.com` or use the snap.

## 2. Download + extract the MsQuic prebuilt

StratumV pins MsQuic 2.5.6. On Linux the build consumes a prebuilt
tarball with the OpenSSL3 TLS backend. Download from the official
[microsoft/msquic](https://github.com/microsoft/msquic/releases/tag/v2.5.6)
release page:

```sh
cd /opt
sudo curl -L -o msquic-2.5.6.tar.gz \
    https://github.com/microsoft/msquic/releases/download/v2.5.6/msquic_linux_x64_Release_openssl3.tar.gz
sudo mkdir -p msquic-2.5.6
sudo tar -xf msquic-2.5.6.tar.gz -C msquic-2.5.6 --strip-components=1
ls /opt/msquic-2.5.6
# expected layout:
#   bin/libmsquic.so          (or .so.2 / .so.2.5.6)
#   include/msquic.h
#   include/msquic_posix.h
```

If the OpenSSL3 tarball is unavailable for a given release, the
OpenSSL1 flavor (`msquic_linux_x64_Release_openssl.tar.gz`) is wire-
compatible; it just uses OpenSSL 1.1 for TLS instead of 3.0. Debian 12
has both available so either works.

## 3. Configure the build

The CMake option matrix for the Linux dedicated-server flavor:

```sh
git clone https://github.com/RoaringBytes/StratumV.git
cd StratumV
cmake -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSTRATUMV_CORE_ONLY=ON \
    -DSTRATUMV_BUILD_TESTS=ON \
    -DSTRATUMV_LINUX_MSQUIC_ROOT=/opt/msquic-2.5.6
```

Required flags:
- `STRATUMV_CORE_ONLY=ON` — omits all graphics dependencies. Without
  it, the configure will try to fetch Vulkan-Headers, glslang, glm,
  ozz-animation, ImGui, etc., which have no business on a headless
  Linux dedicated server.
- `STRATUMV_LINUX_MSQUIC_ROOT=/path/to/msquic` — the directory you
  extracted in step 2. Must contain `include/msquic.h` and
  `bin/libmsquic.so` (or `lib/libmsquic.so`).

Optional:
- `STRATUMV_BUILD_TESTS=ON` — builds the core ctest subset
  (test_ReplicationRegistry, test_ReplicationWire, test_MsQuicTransport,
  test_StratumVCore). Skip for a faster dedicated-server-only build.

## 4. Build

```sh
cmake --build build --parallel
```

Outputs (all under `build/`):
- `libstratumv_core.a` — the Layer 4 static library
- `src/stratumv_server/stratumv_server` — the headless dedicated server exe
- `tests/sv_tests` — the core ctest subset (if tests are enabled)

Because the target links MsQuic from a non-standard system path, the
executable's RPATH is set at build time to the MsQuic `bin/` directory
inside `STRATUMV_LINUX_MSQUIC_ROOT`. No `LD_LIBRARY_PATH` juggling is
needed to run it from the build tree.

If you move `stratumv_server` to a system path (e.g.
`/usr/local/bin`), you must either:

- Rebuild with a matching `STRATUMV_LINUX_MSQUIC_ROOT`, OR
- Set `LD_LIBRARY_PATH=/opt/msquic-2.5.6/bin` before invoking, OR
- Copy `libmsquic.so*` into `/usr/local/lib` and run `sudo ldconfig`

## 5. Run the dedicated server

```sh
./build/src/stratumv_server/stratumv_server --port 9101 --tick-hz 30
```

Expected startup log:

```
[Server][info] StratumV dedicated server starting (StratumV 1.3.2, msquic 2.5.6, tick 30 Hz)
[Server][info] Built schema handshake preamble (13 bytes, 1 types)
[Server][info] Listening on 127.0.0.1:9101 (idle timeout 60000 ms)
```

`Ctrl-C` cleanly shuts down the server (SIGINT handler flips the
`g_running` atomic, the listener accept loop drains, RegistrationClose
runs).

### Binding to a non-loopback address

`stratumv_server` currently binds to `127.0.0.1` unconditionally (see
`src/engine/net/MsQuicTransport.cpp::Transport::startListener`). To
serve Windows clients from a Proxmox VM across the LAN, run with the
Linux host's routable IP in the same NAT/bridge domain as the client
machines — the loopback bind is a Layer 4 implementation detail that
will be generalized when interest management lands.

## 6. Run the ctest subset

```sh
cd build
ctest --output-on-failure
```

In `STRATUMV_CORE_ONLY=ON` builds, `sv_tests` includes only the Layer
4 subset (`test_ReplicationRegistry`, `test_ReplicationWire`,
`test_MsQuicTransport`, `test_StratumVCore`). The graphics tests
(FrustumCuller, AnimationStateMachine, AssetBrowser, NetworkContext
shape, etc.) are not built because their TUs depend on headers that
aren't in the core carve-out's include path.

## 7. Deployment notes

### systemd unit

A minimal unit file for running `stratumv_server` as a service:

```ini
# /etc/systemd/system/stratumv-server.service
[Unit]
Description=StratumV Dedicated Server
After=network.target

[Service]
Type=simple
ExecStart=/opt/stratumv/bin/stratumv_server --port 9101 --tick-hz 30
WorkingDirectory=/opt/stratumv
Restart=on-failure
RestartSec=3
User=stratumv
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

Enable with:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now stratumv-server
sudo systemctl status stratumv-server
```

### Firewall (ufw)

```sh
sudo ufw allow 9101/udp comment "StratumV dedicated server"
```

QUIC rides over UDP, not TCP.

## 8. Current limitations

- Self-signed loopback certificate only. Real cert provisioning
  (Let's Encrypt, custom CA, SNI vhost routing) is future work.
- Binds to `127.0.0.1` only — LAN deployment needs a source tweak
  until the bind address is generalized.
- No in-engine gameplay state beyond a demo orbiting cube. Real
  gameplay state lives in the consumer game built on StratumV.
- stratumv_server accept log's `alpn` field is still blank at first
  accept because stats are read before `QUIC_CONNECTION_EVENT_CONNECTED`
  has fired — minor cosmetic issue.
