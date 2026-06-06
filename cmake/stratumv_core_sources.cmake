# ── StratumV core library sources ──────────────────────────
#
# Shared between the full Windows engine library (`stratumv`) and the
# Linux-or-Windows headless-server carve-out (`stratumv_core`). Keeping
# the list in one file guarantees the two library builds never drift in
# what they consider "core".
#
# The core subset is the Layer 4 networking + replication pieces plus
# their only non-graphics dependency, `EngineLog`. Every source in this
# list MUST compile with only the following include roots visible:
#
#   src/                        (for "net/X.h" #includes)
#   src/engine/                 (for direct header includes)
#   generated/                  (StratumVVersion.h)
#   nlohmann/json.hpp transitive via EngineLog — actually unused here,
#                               but kept as an allowed future dep
#   msquic.h                    (Windows Schannel or Linux OpenSSL)
#
# Explicitly NOT allowed in the core subset: vulkan-headers, volk,
# vk_mem_alloc, glslang, glm, entt, ozz-animation, ufbx, tinygltf,
# meshoptimizer, imgui, miniaudio. `tests/test_StratumVCore.cpp` has a
# compile-only probe that fails the build if any of those creep in.
#
# MsQuic availability is an outer gate — callers should only include
# this file after verifying `STRATUMV_ENABLE_MSQUIC`. `MsQuicTransport.cpp`
# still compiles with MsQuic off because of its internal stub fallback,
# but there is no reason to carve it out of `stratumv.lib` in that
# configuration so the outer gate keeps things simple.

set(STRATUMV_CORE_SOURCES
    # ── Platform-neutral logging ring buffer ──
    # Pure std:: — chrono, mutex, vector, printf. Zero graphics deps.
    src/engine/EngineLog.cpp

    # ── INetworkContext NoOp implementation ──
    # Layer 4 no-op factory; pure std::.
    src/engine/INetworkContext.cpp

    # ── Replication reflection registry ──
    # SV_REPLICATE macros, Authority enum, SnapshotWriter/Reader,
    # encodeSnapshot / decodeSnapshot. Depends only on EngineLog.
    src/engine/ReplicationRegistry.cpp

    # ── First replicated component ──
    # 7-float NetTransform + SV_REPLICATE + SV_COMPONENT_AUTHORITY
    # + ensureNetTransformRegistered anchor.
    src/engine/NetTransform.cpp

    # ── Second replicated component ──
    # Single-u32 ParentLink with Authority::Owner. SetField-only
    # on the wire — never part of Spawn payloads. Shipped in the
    # core subset so the Linux headless server can validate
    # parent sync transactions without a graphics build.
    src/engine/ParentLink.cpp

    # ── Third replicated component ──
    # 8-field LightComponent with Authority::Editor — the first
    # component to actually exercise the Editor authority class.
    # Type enum + RGB color + intensity + range + cone angles; the
    # entity's NetTransform supplies position + direction. Pure
    # logic (no Vulkan), shipped in the core subset so the Linux
    # headless server can validate light sync transactions.
    src/engine/LightComponent.cpp

    # ── Fourth replicated component ──
    # 4-field CameraComponent with Authority::Editor. Per-entity
    # camera projection sidecar (fovDeg + aspect + nearPlane +
    # farPlane). The lab harness picks the first entity with a
    # non-default sidecar each frame and applies it to its proj
    # matrix. Pure logic, ships in core so the Linux headless
    # server can validate camera sync transactions.
    src/engine/CameraComponent.cpp

    # ── Fifth replicated component ──
    # 4-field MaterialComponent with Authority::Editor. Single-demo
    # basecolor override + strength gate, applied at the end of the
    # fragment shader as a multiplier. Does NOT replace the
    # SkinnedMeshPass material dispatch path. Pure logic, ships in
    # core so the Linux headless server can validate material sync
    # transactions.
    src/engine/MaterialComponent.cpp

    # ── Collaborative edit transaction wire format ──
    # EditKind enum + 33-byte header + NetTransform payload codec +
    # generic payload dispatch via ReplicationRegistry.
    # Pure logic, no MsQuic, no Vulkan. UndoLog.h is header-only.
    src/engine/EditTransaction.cpp

    # ── World persistence binary format ──
    # Disk-backed snapshot of the dedicated server's entity map,
    # serialised via the same encodeSnapshot walker the wire layer
    # uses. Pure logic built on stdio + std::filesystem — no graphics.
    src/engine/WorldPersistence.cpp

    # ── SHA-256 helper ──
    # Pure C++ FIPS 180-4 implementation used by the asset sync path
    # for content-addressable hashing. Zero external dependencies —
    # ~200 lines of scalar arithmetic.
    src/engine/Sha256.cpp

    # ── Content-addressable asset store ──
    # Server-side CAS with in-memory cache + optional disk mirror at
    # <server-data>/assets/<2hex>/<62hex>.bin. Pure logic built on
    # stdio + std::filesystem.
    src/engine/AssetPersistence.cpp

    # ── Asset upload client + receiver assembler ──
    # Shared sender/receiver state machine for chunked asset sync.
    # Used by both stratumv_server and client engines.
    src/engine/AssetUploadClient.cpp

    # ── Wire framing layer ──
    # Datagram snapshot frame + reliable schema handshake frame +
    # Welcome message. Pure logic, no MsQuic.
    src/engine/net/ReplicationProtocol.cpp

    # ── MsQuic transport wrapper ──
    # Self-signed cert generation is per-platform: wincrypt/ncrypt on
    # Windows, OpenSSL on Linux. Cross-platform gating lives inside
    # the .cpp via `#if defined(_WIN32)`.
    src/engine/net/MsQuicTransport.cpp
)
