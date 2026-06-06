# StratumV — Engine Architecture

## Overview

StratumV is a layered Vulkan 1.3 game engine. The render core is frozen after initial extraction. All gameplay and game-specific features live in hot-reloadable DLL plugins. Games consume the engine via `add_subdirectory` and provide their own `SceneUBO`, `GameSystemContext`, and domain engine systems.

---

## Layer Model

```
┌─────────────────────────────────────────────────────────────────┐
│                        GAME (consumer)                          │
│                                                                 │
│  SceneUBO (game-defined)   GameSystemContext (game-defined)     │
│  Game domain systems (Terrain/Ocean/Buildings/etc.)             │
│  DLL plugins  ──  src/systems/ in each game repo               │
└──────────────────────────┬──────────────────────────────────────┘
                           │ links / includes
┌──────────────────────────▼──────────────────────────────────────┐
│                   LAYER 6 — Game Interface                      │
│  EngineBase  |  BaseSystemContext  |  IModularSystem             │
│  EngineSystem  |  SystemRegistry  |  DLLLoader  |  AssetWatcher │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│                   LAYER 5 — Content Pipeline / Dev               │
│  DevServer (TCP :9999)  |  SceneLoader  |  MaterialPipeline      │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│                   LAYER 4 — Engine Services                     │
│  Input  |  InputAction  |  InputBindings                        │
│  Camera  |  ICameraMode  |  FreeFlyCamera  |  FollowCamera       │
│  OrbitCamera  |  Audio (miniaudio)  |  Config (JSON)             │
│  ECS (EnTT)  |  Events (dispatcher)                             │
│  WorldStateIO  |  SceneStatePersistence  |  SceneStateVersioning│
│  EngineLog (ring buffer, SV_LOG_* macros)                       │
│  INetworkContext (no-op stub)                                    │
│  PhysicsTypes | IPhysicsContext | JoltPhysicsContext             │
│  AnimationTypes  |  AnimationSystem (ozz 0.16.0, IK, root motion)│
│  AnimationStateMachine (state graph + crossfade)                 │
│  BlendTree (IBlendNode + BlendSpace1D + joint masks)             │
│  IAnimationController (per-entity DLL plugin interface)          │
│  AnimatorComponent (ECS + stateMachine + IK slots)              │
│  FrustumCuller (6-plane extraction + AABB test)                  │
│  MorphTargetTypes (SSBO delta storage + descriptor set 3)        │
│  AssetManifest (JSON preload list, AssetKind enum)              │
│  AssetBrowser (filesystem scan + ImportSettings cache)           │
│  ThumbnailCache (LRU disk-backed cache)                          │
│  WorldBounds (POD in Config.h)                                   │
│  --- networking substrate ---                                    │
│  ReplicationRegistry (SV_REPLICATE + DirtyMask)                  │
│    + Authority enum + SnapshotWriter/Reader + encode/decode      │
│    (scalar path, runtime walker)                                 │
│  net/MsQuicTransport (Transport/Listener/Connection)             │
│  NetTransform / ParentLink / LightComponent /                    │
│    CameraComponent / MaterialComponent                           │
│    (5 shipped replicated components; authorities:                │
│     Server / Owner / Editor / Editor / Editor)                   │
│  net/ReplicationProtocol (wire framing — snapshot +              │
│    schema handshake + welcome + edit transaction +               │
│    asset announce/chunk/ack)                                     │
│  PermissionScope (Spectator/Player/Editor/Admin)                 │
│  EditTransaction + UndoLog (edit tx + undo log,                  │
│    generic payload dispatch via ReplicationRegistry)             │
│  WorldPersistence (SVWLD001 binary format)                       │
│  Sha256 (pure C++ FIPS 180-4)                                    │
│  AssetPersistence (content-addressable store)                    │
│  AssetUploadClient (sender+receiver state machine)               │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│                   LAYER 3 — Render Graph                        │
│  RenderGraph  |  BuiltinPasses  |  RenderPass                   │
│  PostProcess  |  ImGuiLayer  |  AdminPanel base                 │
│  SkinnedMeshPass (skinned PBR rendering)                        │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│                   LAYER 2 — Vulkan Abstraction                  │
│  VkContext  |  VkSwapchain  |  VkShader  |  VkBuffer            │
│  VkTexture  |  VkDescriptors  |  VkPipeline  |  VkMesh          │
│  VkAccelStructure  |  VkRTPipeline  |  VkClusterAS              │
│  VkComputePipeline  |  PipelineRegistry  |  DlssWrapper         │
│  glslang runtime compiler                                       │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│                   LAYER 1 — Platform                            │
│  Window (GLFW)  |  Types (glm, VkTypes)  |  QualityPresets      │
└─────────────────────────────────────────────────────────────────┘
```

---

## Module Registry

| Module | Layer | Status |
|--------|-------|--------|
| `vk/VkContext` | 2 | Extracted |
| `vk/VkSwapchain` | 2 | Extracted |
| `vk/VkShader` | 2 | Extracted |
| `vk/VkBuffer` | 2 | Extracted |
| `vk/VkTexture` | 2 | Extracted |
| `vk/VkDescriptors` | 2 | Extracted |
| `vk/VkPipeline` | 2 | Extracted |
| `vk/VkMesh` | 2 | Extracted (refactored — thin GPU dispatcher) |
| `vk/MeshImportData` | 2 | New (CPU intermediate, header-only) |
| `vk/GltfLoader` | 2 | New (extracted from VkMesh) |
| `vk/FbxLoader` | 2 | New (extracted from VkMesh) |
| `vk/CC5Sidecar` | 2 | New (extracted from VkMesh) |
| `vk/VkAccelStructure` | 2 | Extracted |
| `vk/VkRTPipeline` | 2 | Extracted |
| `vk/VkComputePipeline` | 2 | Extracted |
| `vk/VkClusterAS` | 2 | Extracted |
| `vk/PipelineRegistry` | 2 | Extracted |
| `DlssWrapper` | 2 | Extracted |
| `Window` | 1 | Extracted |
| `Types` | 1 | Extracted |
| `QualityPresets` | 1 | Extracted |
| `Camera` | 4 | Extracted (refactored) |
| `ICameraMode` | 4 | New |
| `FreeFlyCamera` | 4 | New |
| `FollowCamera` | 4 | New |
| `OrbitCamera` | 4 | New |
| `Input` / `InputAction` / `InputBindings` | 4 | Extracted |
| `Audio` / `AudioTypes` | 4 | Extracted |
| `Config` | 4 | Extracted |
| `Events` | 4 | Extracted |
| `Components` | 4 | Extracted |
| `graph/RenderGraph` | 3 | Extracted |
| `graph/GraphResources` | 3 | Extracted |
| `graph/BuiltinPasses` | 3 | Extracted |
| `graph/GpuProfiler` | 3 | Extracted |
| `RenderPass` | 3 | Extracted |
| `PostProcess` | 3 | Extracted |
| `passes/ShadowPass` | 3 | Extracted (modified) |
| `ui/ImGuiLayer` | 3 | Extracted |
| `ui/AdminPanel` | 3 | Extracted (modified) |
| `ui/AdminPanelDecorations` | 3 | Extracted |
| `ui/UiStyle` | 3 | Extracted (renamed) |
| `WorldStateTypes` | 4 | Extracted |
| `DevServer` | 5 | Extracted (modified) |
| `SceneLoader` | 5 | New |
| `MaterialPipeline` | 5 | New |
| `DLLLoader` | 6 | Extracted (modified) |
| `AssetWatcher` | 6 | Extracted (gained `watchDirectoryRecursive` + case-insensitive ext match) |
| `IModularSystem` | 6 | Extracted (modified) |
| `EngineSystem` | 6 | Extracted |
| `SystemRegistry` | 6 | Extracted (modified) |
| `WorldStateIO` | 4 | Extracted |
| `SceneStatePersistence` | 4 | Extracted (redesigned) |
| `SceneStateVersioning` | 4 | Extracted (redesigned) |
| `BaseSystemContext` | 6 | Extracted |
| `INetworkContext` | 4 | Extracted |
| `EngineLog` | 4 | Extracted |
| `AnimationTypes` | 4 | New |
| `AnimationSystem` | 4 | New |
| `AnimationStateMachine` | 4 | New |
| `BlendTree` | 4 | New |
| `IAnimationController` | 4 | New |
| `AnimatorComponent` | 4 | New |
| `SkinnedMeshPass` | 3 | New |
| `PhysicsTypes` | 4 | Extracted |
| `IPhysicsContext` | 4 | Extracted |
| `JoltPhysicsContext` | 4 | Extracted |
| `FrustumCuller` | 4 | New |
| `MorphTargetTypes` | 4 | New |
| `AssetManifest` | 4 | New |
| `AssetBrowser` | 4 | New (extended with `.meta.json` persistence + drag source) |
| `ThumbnailCache` | 4 | New (disk-backed + LRU + byte-budget) |
| `ReplicationRegistry` | 4 | New (SV_REPLICATE reflection + Authority + snapshot encoder) |
| `NetTransform` | 4 | New (first SV_REPLICATE'd component — 7 floats, Authority::Server) |
| `net/MsQuicTransport` | 4 | New (Transport/Listener/Connection RAII over MsQuic 2.5.6) |
| `net/ReplicationProtocol` | 4 | New (datagram snapshot + reliable schema/welcome/edit tx/asset messages) |
| `PermissionScope` | 4 | New (Spectator/Player/Editor/Admin ladder) |
| `EditTransaction` | 4 | New (edit tx wire format + generic payload dispatch) |
| `UndoLog` | 4 | New (per-client LIFO walk-back, header-only) |
| `WorldPersistence` | 4 | New (SVWLD001 binary format for server state) |
| `Sha256` | 4 | New (pure C++ FIPS 180-4) |
| `AssetPersistence` | 4 | New (content-addressable store, in-memory + optional disk) |
| `AssetUploadClient` | 4 | New (sender+receiver state machine for chunked asset sync) |
| `EngineBase` | 6 | Extracted (refactored for WorldBounds + AssetManifest) |

---

## Game Contract

Every game consuming StratumV must provide these in their own `src/game/`:

### `SceneUBO.h`
Defines the `SceneUBO` struct — the uniform buffer object layout for all shaders.
Must be a valid std140 layout. Games version this independently.

### `GameSystemContext.h`
Extends `BaseSystemContext` (from StratumV) with game-specific function pointers and mutable state.
DLL plugins `#include` this to access game-specific engine features.
Must not include any StratumV internal headers — only the public interface.

### `GameConfig.h`
Game-specific `Config` keys, defaults, and validation.

### `GameEngine.h / .cpp`
Game-specific engine subclass extending `EngineBase`. Overrides `onInit()` (window, Vulkan, context wiring), `onShutdown()` (teardown), `onFrame()` (per-frame logic + rendering). Contains `main()` entry point. Generated by `scaffold_game.py`.

---

## DLL Plugin System

DLL plugins implement `IModularSystem` and are loaded at runtime by `DLLLoader`.
Communication across the DLL boundary uses `GameSystemContext`, which extends
`BaseSystemContext` via public single-inheritance. `BaseSystemContext` (1.2.0+)
holds 135 leaf fields: 14 flat hot fields (`device`, `allocator`, `ecs`,
`events`, window + shared flags + services) plus 9 nested POD sub-structs
(`rendering` / `ui` / `input` / `buffers` / `vkfn` / `meshRegistry` / `audio`
/ `animation` / `world`). Each sub-struct is a POD with default member
initializers only — no constructors, no virtuals, so the outer struct remains
DLL-safe with the base subobject at offset 0.

```
game_systems.dll  ──►  IModularSystem interface
                  ←──  GameSystemContext (fn ptrs, ECS, device, allocator)
```

- `PreDraw` / `PostDraw` hooks for per-frame rendering
- `adminTab()` for admin panel integration
- `serialize()` / `deserialize()` for JSON persistence (required)
- `AssetWatcher` detects DLL rebuild → shadow-copy → `LoadLibrary` / `FreeLibrary` cycle with JSON state preservation

---

## Frozen Core Policy

The "frozen core" scopes to **Layers 1–3 only** — the Vulkan abstraction,
render graph, and built-in render passes. Layer 4 services (ECS,
animation, physics, networking, replication) continue to evolve as the
engine matures. The policy is deliberately scoped this way because
treating replication and live-collab editing as substrate rather than
add-ons requires room to grow: a future replication module cannot land
under a whole-engine freeze; a frozen Layer 1–3 can coexist with an
actively-evolving Layer 4.

**Layers 1–3 (frozen after initial authoring):**

- No new render passes without a StratumV version bump (semver minor).
- No breaking changes to `VkContext`, `VkSwapchain`, `VkShader`,
  `VkBuffer`, `VkTexture`, `VkDescriptors`, `VkPipeline`, `VkMesh`,
  `VkAccelStructure`, `VkRTPipeline`, `VkComputePipeline`,
  `PipelineRegistry`, `DlssWrapper`, `RenderGraph`, `BuiltinPasses`,
  `PostProcess`, `ShadowPass`, `ImGuiLayer`, `AdminPanel`, or
  `SkinnedMeshPass` without a version bump.
- Internal bug fixes and non-breaking additions (new overloads, new
  fields added to the end of structs) are allowed.

**Layers 4–6 (actively evolving):**

- `BaseSystemContext` shape changes are allowed but require a semver
  bump and a migration table in `CHANGELOG.md`. These refactors follow
  this pattern (see the CHANGELOG.md migration tables).
- New Layer 4 modules can be added freely.
- `IModularSystem` interface is stable in shape but may gain new
  non-breaking methods with an interface version bump.
- `SceneUBO` is per-game — the engine does not define it.

**DLL plugin ABI (frozen after initial authoring):** DLL plugins depend on public
engine headers only. DLL hot-reload semantics are preserved. Any
layout change that would break a compiled DLL requires a major version
bump.

---

## Physics

`IPhysicsContext` in Layer 4 — pure virtual interface (21 methods) with `createNoOpPhysicsContext()` factory.
`JoltPhysicsContext` — concrete Jolt 5.2.0 implementation behind `STRATUMV_ENABLE_JOLT` CMake flag.
`PhysicsTypes.h` — POD types shared by interface and consumers.

API surface: rigid bodies (dynamic/static box, compound), terrain heightfield, character controller (capsule, slope, step, resize), constraints (fixed/point/hinge/distance with cascade break), raycast, live params, diagnostics.

A game switches from its own physics integration to the engine JoltPhysicsContext when it wires physics.
Other games wire the no-op context until they need physics.

---

## Asset Import Formats

The engine loads meshes, skeletons, animations, and morph targets from standard DCC export formats. `VkMesh::loadFromFile()` routes by file extension.

| Format | Library | Mesh | Skeleton | Animations | Morph Targets | Textures |
|--------|---------|------|----------|------------|---------------|----------|
| glTF/GLB | tinygltf | Yes | Yes | Yes | Yes (glTF morph targets) | Embedded or external |
| FBX | ufbx | Yes | Yes | Yes (multi-clip) | Yes (facial blend shapes) | Embedded or external |

**CC5 character pipeline:** CC5 exports FBX natively. FBX preserves skeleton, skin weights, PBR textures, all animation clips, and facial expression blend shapes (262 HD / 63 ExpressionPlus with MotionPlus export). Body morphs bake into the mesh at export — only facial shapes survive as animatable targets. CC5 exports in centimeters with Z-up axes.

### FBX Axis + Unit Conversion Strategy

| Property | Value |
|----------|-------|
| ufbx mode | `UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS` |
| Target axes | right=+X, up=+Y, front=+Z (Y-up, matches glTF) |
| Target units | `target_unit_meters = 1.0` |
| Vertex data | **Raw** — untouched by ufbx (Z-up, centimeters) |
| Node transforms | Fully adjusted by ufbx for both axis rotation and unit scaling |
| IBMs / rest poses | Adjusted by ufbx — `inverse(bind_to_world)` maps adjusted-world → raw-vertex-space |
| Animations | `ufbx_evaluate_transform()` returns adjusted transforms |
| Morph deltas | Raw — same space as vertices, added before skinning |

**Why:** The previous approach (`MODIFY_GEOMETRY` + manual vertex swizzle) split axis conversion between ufbx (skeleton) and engine (vertices), causing flipped/mirrored characters. `ADJUST_TRANSFORMS` makes ufbx the single authority — bind pose math cancels out the coordinate difference automatically. No manual swizzle code needed.

**CPU-side note:** Raw vertex positions are in Z-up centimeters until skinning transforms them on GPU. CPU bounding boxes or collision from raw vertex data must account for this (standard concern for any skinned mesh).

**Procedural scene pipeline:** a Blender geometry-nodes addon can generate procedural scenes, exporting glTF/GLB with instancing, loaded via SceneLoader (`.scene.json`) or VkMesh directly.

**Blender pipeline:** Blender can batch-convert CC5 FBX → GLB headlessly (`blender --background --python`). cc_blender_tools addon auto-configures CC5 PBR materials. NLA tracks map to separate glTF animations.

---

## Developer Workflow Architecture

The engine is a runtime, not an editor. Scene authoring and spatial editing happen in external tools. The workflow splits responsibilities:

```
┌──────────────┐     ┌──────────────┐     ┌──────────────────────┐
│  CC5/iClone  │     │   Blender    │     │   Engine (runtime)   │
│              │     │              │     │                      │
│ Create chars │────►│ Build world  │────►│ Play & test          │
│ Animate      │ FBX │ Place assets │ TCP │ Tune characters      │
│ Mocap        │     │ Procedural   │:9999│ Adjust gameplay      │
│              │     │ scenes       │◄───►│ Save scene state     │
│              │     │ Position ents│     │                      │
└──────────────┘     └──────────────┘     └──────────────────────┘
                      (spatial editor)     (gameplay tuning)
```

| Role | Tool | How |
|------|------|-----|
| Character creation | CC5 | Artist exports FBX + JSON sidecar, engine loads both via ufbx + nlohmann JSON |
| Character animation | iClone 8 / Blender | Stock locomotion library, mocap, or manual animation; export FBX |
| World building | Blender | Procedural scene generation, prop placement, lighting |
| Spatial editing | Blender (via live link) | Move/rotate/scale objects, see changes in engine instantly |
| Asset browsing | AdminPanel AssetBrowser | Browse assets/ folder, click to load, import settings |
| Character tuning | AdminPanel EntityInspector | Morph sliders, animation selector, gameplay parameters |
| Scene persistence | AdminPanel save-back | Save current scene state to disk, reload cleanly |

**Key principle:** No config file editing. Artists use CC5 + Blender GUIs. Developers use AdminPanel for gameplay tuning. The engine hot-reloads on file changes (AssetWatcher) and accepts live edits (DevServer TCP :9999).

**CC5 material data flow:** CC5 exports a `.json` sidecar alongside each `.fbx` containing `Custom Shader` variables (RootColor, TipColor, dye colors, shader type, Node Type). Standard FBX material properties are insufficient for CC5's hair/lash materials. The engine parses the JSON sidecar at FBX load time to extract these values.

**Future: CC5 Live Link (backlog):** Bidirectional CC5/iClone ↔ DevServer TCP sync for real-time character pose/material/morph editing. Requires CC5 plugin SDK investigation. Long-term vision.

---

## Networking (Substrate)

StratumV treats multiplayer and live collaborative development as the
engine substrate, not a bolt-on feature. The `INetworkContext` stub is a
placeholder that proves the DLL boundary can carry a networking
interface; the real networking stack builds on top of it, starting with
the reflection registry and ending with asset sync replication.

**Design rationale:** See `docs/NETWORK_DESIGN.md` for the transport
choice (MsQuic), authoritative server model, scale target (256–500
players per shard), determinism policy (fixed-timestep Jolt + ozz),
dedicated-server binary, and why the data model IS the replication
model (Resonite-inspired inversion).

**Per-field replication contract:** See `docs/REPLICATION_CONTRACT.md`
for the `SV_REPLICATE` macro surface, the `Authority::Server/Owner/Editor/None`
enum, dirty-bit mechanics, snapshot/delta encoding, and why codegen was
deferred in favor of macros for v1.

**Collaborative development:** See `docs/COLLAB_EDITING.md` for the
four permission scopes (`Spectator`/`Player`/`Editor`/`Admin`), edit
transactions, the undo log, and asset sync. Live collab is not a
separate feature on top of networking — it's a permission scope on the
existing replication substrate.

### Layer placement

The networking stack lives across two layers:

- **Layer 2** — `NetworkTransport` (MsQuic binding). Platform-adjacent
  module that wraps the MsQuic C API, exposes reliable streams +
  unreliable datagrams + TLS 1.3 + connection migration. Same conceptual
  tier as `vk/VkContext` — infrastructure that the rest of the engine
  builds on.
- **Layer 4** — `ReplicationRegistry`, `TransactionLog`, `AssetSyncEngine`.
  Service modules that depend on the Layer 2 transport and expose
  per-component replication, edit transactions, and content-addressable
  asset sync to game DLL plugins via the nested `network` sub-struct on
  `BaseSystemContext`.

### Dedicated server

A separate CMake target, `stratumv_server`, is a headless binary that
shares Layer 4 services with the client but excludes `vk/`, `graph/`,
`passes/`, `ui/`, and the rendering phases of `EngineBase`. Single-player
games built on StratumV run as one local `stratumv_server` process plus
one client connected over loopback — same code path, same authority
rules, same replication. Dedicated servers are self-hosted (no cloud
provider coupling); see `NETWORK_DESIGN.md §5.3` for the rationale.

---

## Architecture Decisions

### Animation

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Animation library | ozz-animation 0.16.0 | MIT, SIMD SoA, FetchContent, root motion support, battle-tested |
| GPU bone upload | SSBO std430 (not UBO) | No bone count limit, supports multi-character in single buffer |
| Skinning method | Vertex shader 4-weight | Standard, sufficient for game characters, avoids compute pass complexity |
| Bone palette layout | Single SSBO, per-draw boneOffset | One buffer for all characters, minimal descriptor set switching |
| glTF → ozz conversion | Runtime via offline API | No CLI tool in build step, works with hot-reload pipeline |
| State machine | Engine-owned, not ozz | ozz is stateless by design; engine builds state graph on top |
| Morph targets | Separate system (not ozz) | ozz is skeletal only; morph blending in vertex shader or compute |
| Max bones per character | 128 soft limit | Covers CC5 exports (137 deform bones after pruning controls) |
| Threading | Per-character job parallelism | ozz jobs are thread-safe with separate Contexts |
| ECS integration | All scene/animation data lives on EnTT entities | SceneLoader populates entities (TransformComponent + mesh ref); the AnimatorComponent adds skeleton, clips, blend weights; game plugins mutate ECS; renderer iterates entities by component (static mesh vs skinned mesh) — ECS is the glue, not a parallel system |

### Physics

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Physics library | Jolt 5.2.0 | MIT, mature, SIMD, character controller, production-proven |
| Integration pattern | IPhysicsContext (interface + no-op) | Same DLL-safe pattern as INetworkContext; games opt in |
| Engine module | JoltPhysicsContext.h/.cpp (pimpl) | Wraps Jolt behind a pimpl; hides Jolt headers |
| CMake flag | STRATUMV_ENABLE_JOLT | Per-game opt-in; no Jolt compile cost for games that don't use it |
| BaseSystemContext slot | IPhysicsContext* physics | DLL plugins query physics via interface, not raw Jolt |
| Character controller | Jolt CharacterVirtual | Capsule-based, supports slope, step, crouch resize |
| Terrain collision | Jolt HeightFieldShape | Usable for terrain or a ground plane |

### Pathfinding

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Engine interface | IPathfindingContext (interface + no-op) | DLL-safe; same pattern as INetworkContext/IPhysicsContext |
| Implementation | Game-side (e.g. Recast/Detour) | Nav generation params are world-specific |
