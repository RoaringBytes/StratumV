# Changelog

All notable changes to StratumV are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/) and the project adheres to
[Semantic Versioning](https://semver.org/).

## [1.3.10] - Initial public release

First open-source (Apache-2.0) release of StratumV, a game-agnostic Vulkan 1.3
game engine.

### Highlights
- Vulkan 1.3 renderer: render graph, RT shadows, ReSTIR DI, post-processing;
  optional NVIDIA DLSS / SHARC / neural texture compression (off by default).
- Hot-reloadable DLL game systems via the `IModularSystem` plugin interface.
- Networking-first data model: an authoritative `stratumv_server`, reflection-based
  replication (`SV_REPLICATE`) over QUIC (MsQuic), and permission scopes.
- Live collaborative editing: edit transactions, a shared undo log, and asset sync.
- ECS (EnTT), animation (ozz-animation), optional physics (Jolt), audio (miniaudio).
- Blender add-on for scene export and live link.

See `ARCHITECTURE.md` and the `docs/` directory for design and contract details.
