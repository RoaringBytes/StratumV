# StratumV

**A layered Vulkan 1.3 game engine. Multiplayer-first. Live systems. Collaborative by default.**

StratumV is a game-agnostic, open-source engine you can build any game on. It
pairs a high-end Vulkan renderer with a networking-first data model and a
hot-reloadable plugin system, so gameplay iterates without recompile-restart
cycles. The engine compiles directly into your game binary — you provide the
game content, StratumV provides the infrastructure.

- **License:** Apache-2.0
- **Language:** C++20
- **Platforms:** Windows (full engine) · Linux (headless dedicated server)
- **Status:** 1.3.10

---

## Features

- **Vulkan 1.3 renderer** — a render graph with RT shadows, ReSTIR DI, and post-processing. Optional NVIDIA DLSS / SHARC / neural texture compression (off by default; proprietary).
- **Hot-reloadable DLL game systems** — gameplay lives in plugins implementing `IModularSystem`. No recompile-restart cycle during development.
- **Networking as substrate** — an authoritative `stratumv_server`, reflection-based replication (`SV_REPLICATE`) over QUIC (MsQuic), and permission scopes. Single-player is one local server + one client on the same binary.
- **Live collaborative editing** — multiple developers editing the same running world: scene transforms, materials, entities, asset pushes, and a shared undo log.
- **Batteries** — ECS (EnTT), skeletal animation (ozz-animation), optional physics (Jolt), audio (miniaudio), glTF/FBX import, a live admin panel, and a Blender add-on for scene export + live link.

## Requirements

- Windows 10/11 with a Vulkan 1.3-capable GPU and current drivers
- Visual Studio 2022 (MSVC v143)
- CMake 3.24+

Most dependencies (GLFW, GLM, Vulkan-Headers, volk, VMA, EnTT, glslang, ImGui,
tinygltf, ufbx, meshoptimizer, miniaudio, ozz-animation, MsQuic) are fetched
automatically at configure time via CMake `FetchContent`. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Build

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Run the test suite:

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DSTRATUMV_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### Useful options

| Option | Default | Notes |
|--------|---------|-------|
| `STRATUMV_BUILD_TESTS` | `OFF` | Build and register the Catch2 test suite |
| `STRATUMV_ENABLE_MSQUIC` | `ON` | QUIC transport for networking/replication |
| `STRATUMV_ENABLE_JOLT` | `OFF` | Jolt physics backend |
| `STRATUMV_ENABLE_DLSS` / `_SHARC` / `_NTC` | `OFF` | Optional **proprietary** NVIDIA SDKs (you supply / accept NVIDIA's terms) |
| `STRATUMV_CORE_ONLY` | `OFF` | Headless build of the networking core + dedicated server (Linux-capable) |

## Building a game on StratumV

A game provides three things — a `SceneUBO` (your std140 uniform layout), a
`GameSystemContext` (extends the engine's `BaseSystemContext`), and a
`GameConfig` — plus its own game systems as DLL plugins.

The fastest way to start is the project generator, which scaffolds a complete
starter project (CMake wiring, the three required files, and an example plugin)
already pointed at the engine:

```sh
python tools/scaffold_game.py MyGame
```

Then consume the engine from your game's `CMakeLists.txt`:

```cmake
include(FetchContent)
add_subdirectory(../StratumV ${CMAKE_BINARY_DIR}/stratumv)
# ... (the scaffold wires ImGui sources for you)
target_link_libraries(MyGame PRIVATE stratumv)
```

See the generated `GAME_CONTRACT.md`, plus [`docs/PLUGIN_CONTRACT.md`](docs/PLUGIN_CONTRACT.md)
and the [`IModularSystem`](src/engine/IModularSystem.h) interface, for the full
plugin contract.

## Documentation

| Doc | Contents |
|-----|----------|
| [`VISION.md`](VISION.md) | Project goals and design philosophy |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Engine layer diagram and module registry |
| [`docs/NETWORK_DESIGN.md`](docs/NETWORK_DESIGN.md) | Networking model and transport |
| [`docs/REPLICATION_CONTRACT.md`](docs/REPLICATION_CONTRACT.md) | `SV_REPLICATE` + authority model |
| [`docs/COLLAB_EDITING.md`](docs/COLLAB_EDITING.md) | Live collaborative editing |
| [`docs/LINUX_SERVER.md`](docs/LINUX_SERVER.md) | Building/running the dedicated server on Linux |

## Project status

StratumV is Windows-first for graphical games; the dedicated server also builds
headless on Linux. Cross-platform graphical support (Linux/macOS) is on the
roadmap, not yet available.

## License

Licensed under the Apache License, Version 2.0 — see [`LICENSE`](LICENSE). Optional
NVIDIA components (DLSS / SHARC / RTXNTC) are proprietary, disabled by default,
and governed by NVIDIA's own terms; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
