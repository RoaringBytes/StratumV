# ASSET_PIPELINE — Rationale

> Decision-layer companion to `ARCHITECTURE.md`. This doc explains **why** the
> asset pipeline chose FBX direct loading alongside glTF, how CC5's JSON
> sidecar solves the material gap, and how the axis/unit conversion problem
> was eventually nailed down. For the what — loader API, file map — see
> `ARCHITECTURE.md` and `src/engine/vk/VkMesh.h`.

The pipeline evolved in two stages:

1. **glTF-only** — via tinygltf, sufficient for testing the skinned-mesh
   pipeline with a single CC5-via-Blender-via-GLB character.
2. **Direct FBX** — loading via ufbx, eliminating the Blender round-trip,
   with full CC5 JSON sidecar parsing to recover the material data FBX's
   standard properties can't express.

A later refactor then split the ~1400-LOC `VkMesh.cpp` into four focused
modules (`MeshImportData` / `GltfLoader` / `FbxLoader` / `CC5Sidecar`). This
document records why each decision landed where it did.

---

## 1. Why Two Loaders (glTF AND FBX)

A single-loader pipeline would be simpler. We chose to support both because
the answer to "can we use only glTF" turned out to be "yes but at unacceptable
cost":

### 1.1 glTF is the clean standard

tinygltf, JSON-based, well-specified, widely supported by DCC exporters,
natural fit for web and open-source workflows. Scene files (`.scene.json`)
build on top of it. Static meshes, skinned meshes with skin definitions,
morph targets, KHR_materials_pbrSpecularGlossiness, embedded or external
textures — all Just Work.

The lab harness used glTF exclusively in the early character work because the
plumbing was fastest to stand up and the output was easy to diff against
reference renders.

### 1.2 FBX is what the content pipeline produces

Character Creator 5 exports **FBX natively**, not glTF. The path from CC5
to an engine-ready mesh via glTF required:

1. Export from CC5 → FBX (native).
2. Batch-convert FBX → GLB in Blender (headless `blender --background
   --python`), with `cc_blender_tools` addon configuring PBR materials.
3. Load GLB in the engine.

Step 2 is where everything went wrong:

- **Constraints get stripped.** CC5 exports spring bones, Aim constraints,
  and rotation limits via FBX's constraint system. glTF has no equivalent.
  Blender bakes them into keyframes, which inflates animation file size and
  loses runtime control.
- **Animation curves get baked.** CC5 NLA tracks come in as separate
  animations; Blender concatenates them unless you use `cc_blender_tools`
  batch export settings, which are subtly different per-CC5-version.
- **Deform channels vanish.** Non-deforming bones (CC_Base_FacialBone,
  IK controls, helper nulls) are dropped or renamed during GLB export,
  breaking the skeleton hierarchy in ways ozz didn't tolerate (a later FBX
  pass had to add intermediate-bone chaining to recover this).
- **Iteration latency.** Every art change meant a CC5 export + a
  Blender headless run + an engine reload. 2–3 minutes minimum,
  cc_blender_tools misconfigurations were invisible until reload failed.

Direct FBX loading via ufbx cut the pipeline to a single step and made the
character pipeline iterate in under 10 seconds. That was the justification for
moving to a native FBX loader.

### 1.3 Why ufbx (and not FBX SDK)

Autodesk FBX SDK is the "official" option. We rejected it because:

- **Closed source, proprietary license.** Autodesk's FBX SDK is a
  per-developer license redistribution concern even for commercial games.
- **Heavy runtime.** The SDK ships as a 25 MB+ DLL with its own CRT.
- **C++ API with implicit state.** Threading model is unclear, error
  handling is exception-driven in a codebase that otherwise doesn't use
  exceptions.

ufbx is MIT-licensed, single C file (`ufbx.c` + `ufbx.h`), ~600 KB parsed,
no exceptions, fully documented source. v0.21.3 is pinned via FetchContent
in `CMakeLists.txt`. It parses mesh + skeleton + skin weights + materials +
blend shapes + animation curves, which is exactly what the pipeline needs.

---

## 2. Axis + Unit Conversion — The Hard Part

The single biggest pipeline bug was the axis/unit conversion path. It took
three iterations to get right. The final approach is worth documenting because
it is NOT the obvious first attempt.

### 2.1 The problem

- **CC5 FBX is Z-up centimeters.** Vertices are in centimeters, the up axis
  is Z, the forward axis is -Y.
- **glTF is Y-up meters.** Vertices are in meters, the up axis is Y, the
  forward axis is +Z.
- **StratumV uses glTF conventions internally** — Y-up meters — because the
  glTF loader was the first path and we want a single coordinate system
  for the whole runtime (ECS, physics, camera, lighting).

So every CC5 FBX character needs three conversions applied consistently:

- Vertices (positions, normals, morph deltas)
- Skeleton (rest pose, inverse bind matrices)
- Animation (keyframes — translation, rotation, scale)
- UV flip (FBX V-up vs GPU V-down)

Getting any one of those inconsistent with the others produces a character
that's **nearly right but subtly wrong** — flipped hand, wrong foot, head
upside down, teeth floating an inch from the jaw, IK targeting the wrong leg.

### 2.2 Attempt 1: MODIFY_GEOMETRY + manual swizzle

The first attempt used `UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY` plus a manual
vertex swizzle `(x, y, z) → (x, z, -y)` in the vertex loader. Axis conversion
was split:

- ufbx handled the **skeleton** (bone transforms got Y-up via
  MODIFY_GEOMETRY's scale-only mode).
- Engine handled the **vertex data** (via manual swizzle).

The result was flipped / mirrored characters. The split meant the inverse
bind matrices and rest poses were in one space (converted by ufbx) while
the vertex data was in a slightly different space (converted by engine).
Bind math (`inverse_bind * world_transform * vertex`) didn't cancel
correctly.

The decision that came out of this: the hybrid approach (MODIFY_GEOMETRY +
manual vertex swizzle) is abandoned.

### 2.3 Attempt 2: ADJUST_TRANSFORMS — ufbx owns everything

The fix was to delegate **all** coordinate conversion to ufbx via
`UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS`. In this mode:

- Vertex data stays **raw** — still in Z-up centimeters in the source buffer.
- ufbx adjusts every node transform (`node_to_world`, `geometry_to_world`,
  `bind_to_world`, and `ufbx_evaluate_transform` output) so that applying
  them to raw vertices produces Y-up meters in world space.
- Inverse bind matrices are computed as `inverse(bind_to_world)`, which
  maps adjusted-world → raw-vertex-space. The math cancels correctly because
  both sides of the skinning transform use the same adjusted space.
- Animation sampling via `ufbx_evaluate_transform()` returns adjusted
  transforms automatically.
- Morph deltas stay raw (same space as vertices, added before skinning).

The per-mesh wrinkle: a CC5 character has 11 meshes (body, eyes, teeth,
tongue, hair × 4, lashes × 3), each with its own skin deformer and its own
`geometry_to_world`. We bake each mesh's vertices with its
`geometry_to_world` at load time, normalizing all 11 meshes to a common
adjusted world space before passing them to `inverse(bind_to_world)`.

**The manual swizzle block is deleted entirely.** Zero lines of engine code
do axis conversion now. ufbx is the single source of truth.

### 2.4 UV V-flip

FBX UVs use bottom-left origin (V up), GPU textures use top-left origin
(V down). `v.uv.y = 1.0f - v.uv.y` in the loader. This is independent of
axis conversion — glTF loaders already do this and it was missed in the
first FBX loader pass.

### 2.5 Intermediate bone chaining

CC5 skeletons include non-deforming bones (`CC_Base_FacialBone`, several
helper nulls) between rig hierarchy joints. Our skeleton is **cluster-based**
— each joint needs an inverse bind matrix pulled from the skin deformer's
cluster list. Non-deforming bones don't have clusters, so they can't be
skeleton joints, but removing them breaks the parent chain.

The fix: `AnimationSystem::loadFbxAnimations()` detects intermediate non-
deforming bones and chains their transforms during animation evaluation.
Key result: jaw and teeth align correctly post-animation.

The CC-Blender-Tools addon handles this differently — it keeps all bones
including non-deforming ones. We can't copy CC-Blender-Tools directly (GPL
licensing) and our cluster-based architecture doesn't have a natural place
for non-deforming joints. Chaining during evaluation is the workaround.

### 2.6 Why the final table reads the way it does

`ARCHITECTURE.md` records the final state:

| Property | Value |
|----------|-------|
| ufbx mode | `UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS` |
| Target axes | right=+X, up=+Y, front=+Z (Y-up, matches glTF) |
| Target units | `target_unit_meters = 1.0` |
| Vertex data | Raw — untouched by ufbx (Z-up, centimeters) |
| Node transforms | Fully adjusted by ufbx |
| IBMs | Adjusted — `inverse(bind_to_world)` |
| Animations | `ufbx_evaluate_transform()` returns adjusted |
| Morph deltas | Raw — same space as vertices |

The "raw vertices are Z-up cm" row is the only footgun that remains. CPU-side
bounding boxes or collision computed from raw vertex data must account for
it. In practice this isn't a concern because the runtime doesn't compute AABBs
from raw CPU vertices — it derives them post-skinning from GPU skinning
matrices or from scene-graph transforms.

---

## 3. CC5 Material Data — The JSON Sidecar

### 3.1 Why FBX materials alone are not enough

CC5's advanced materials (Digital Human Shader / Digital Cornea / Hair /
Eyelash) are defined in CC5's proprietary shader system. FBX's standard
material properties can't express:

- `Node Type` — "Hair", "Eyelash", "Brow" (drives alpha blending)
- `Two Side` — two-sided rendering flag
- Root / tip colors for hair (RootColor, TipColor)
- Dye colors, melanin, pheomelanin
- Shader type identifiers
- Hair root map, hair ID map, hair flow map texture paths

When CC5 exports an FBX, it also writes a sibling `.json` file:

```
HQ-Main_FM_CCExport.fbx
HQ-Main_FM_CCExport.json    ← the sidecar
```

The sidecar contains every CC5-specific material parameter keyed by material
name. Parsing it recovers the data FBX alone can't provide.

### 3.2 Three waves of sidecar integration

**First wave:** Name-based BlendMode detection as a stopgap.
Materials whose name contained "eyelash", "hair", "scalp" got `BlendMode::
AlphaBlend`. This caught 6 of 10 CC5 transparent materials and was enough
to validate the alpha-blend pipeline end-to-end, but fragile — any CC5
character with non-standard material names would miss them.

**Second wave:** Opacity texture loading from FBX PBR
properties (`pbr.opacity.texture`, `fbx.transparency_color.texture`,
`fbx.transparency_factor.texture`). Also parsed the JSON sidecar for
`Custom Shader.Variable.RootColor` to recover hair root colors for
materials without diffuse textures. Opacity was restricted to `AlphaBlend`
materials to prevent false discards on cornea/eyes.

**Third wave:** Full sidecar parser replacing all the
piecemeal hacks. `parseCC5Sidecar(fbxPath)` returns a `CC5MatMap` keyed by
material name, each entry containing Node Type, Two Side, hasOpacity,
hasRootColor + rootColor, and texture paths. The FBX loader consumes this
map once, in two phases:

1. Before texture upload — determines BlendMode from `Node Type` (primary)
   or `(twoSide && hasOpacity)` (secondary for edge cases like
   `Ga_Eyelash` which has no Node Type but does have an opacity texture
   and TwoSide=true).
2. After texture upload — applies RootColor to materials without a diffuse
   texture, so bald-CC5 characters render with the right skin tone.

Name-based matching survives only as the fallback when the sidecar is
missing (non-CC5 FBX files from other DCCs).

### 3.3 Why not bake everything into the FBX

In theory CC5 could export a "material override FBX extension" (FBX has a
custom properties system that technically supports this). In practice:

- CC5 doesn't support custom FBX properties for shader graphs.
- Even if it did, other DCC tools (Maya, Blender, MotionBuilder) wouldn't
  know about our custom namespace.
- The JSON sidecar is readable, grep-able, and versionable. Artists can
  even hand-edit it when CC5 exports something wrong.

The sidecar approach is ugly but correct. The file lives next to the FBX
and travels with it through asset management.

### 3.4 What the sidecar doesn't do (yet)

Deferred to future work:

- **TipColor + root map blending.** Hair rendering uses a root-to-tip
  gradient driven by a hair root map texture. The sidecar has TipColor
  and the root map path; the shader path to consume them doesn't exist.
- **Custom Shader Image textures.** Hair Root Map, Hair ID Map, Hair Flow
  Map, Hair Specular Mask Map are referenced in the sidecar but not loaded
  by the engine.
- **Dye colors, melanin, pheomelanin.** Parsed but ignored.

Hair and skin will get a real shader upgrade in a later character-quality
pass.

---

## 4. The VkMesh Split

After the CC5 sidecar work landed, `VkMesh.cpp` was 1381 LOC and contained:

- Mesh VBO/IBO upload to the GPU.
- Texture upload to GPU.
- glTF parsing via tinygltf (with `TINYGLTF_IMPLEMENTATION` in this file).
- FBX parsing via ufbx (including multi-mesh bone remap, morph targets,
  blend shapes, CC5 material auto-detect).
- CC5 JSON sidecar parsing.
- Channel-packing for metal/rough textures.
- Rest-pose extraction.
- Material property extraction.

The refactor split it into four modules along a hard CPU-vs-GPU boundary:

```
                        ┌──────────────────────┐
                        │   MeshImportData     │  (header-only)
                        │                      │
                        │ vertices, indices,   │
                        │ submeshes, materials,│
                        │ TextureImportData[], │
                        │ skeleton, morph      │
                        │ deltas + names +     │
                        │ default weights      │
                        └──────────┬───────────┘
                                   │ populated by
         ┌─────────────────────────┼─────────────────────────┐
         │                         │                         │
 ┌───────▼───────┐      ┌──────────▼─────────┐    ┌──────────▼─────────┐
 │  GltfLoader   │      │     FbxLoader      │    │     CC5Sidecar     │
 │  (443 LOC)    │      │     (680 LOC)      │    │     (94 LOC)       │
 │               │      │                    │    │                    │
 │ tinygltf      │      │ ufbx               │    │ parseCC5Sidecar    │
 │ stb_image     │      │ ADJUST_TRANSFORMS  │    │ (free function)    │
 │ loadGltf(...) │      │ multi-mesh bones   │    │                    │
 │               │      │ morph targets      │    │ returns CC5MatMap  │
 │               │      │ sidecar integration│◄───┤                    │
 └───────────────┘      └────────────────────┘    └────────────────────┘
         │                         │
         │                         │
         └───────────┬─────────────┘
                     │ consumed by
               ┌─────▼─────────────┐
               │    VkMesh.cpp     │  (132 LOC — 10× reduction)
               │                   │
               │ loadFromFile()    │
               │  • route by ext   │
               │  • call loader    │
               │  • upload VBO/IBO │
               │  • upload textures│
               │  • upload morphs  │
               └───────────────────┘
```

### 4.1 The MeshImportData boundary

`MeshImportData` is a plain struct with vectors of POD data. Loaders take
`(path, loadTextures, MeshImportData& out)` and populate `out`. No VkCtx,
no GPU calls, no VMA allocations. They can run on any thread and be
unit-tested without Vulkan initialized.

`VkMesh::loadFromFile()` is the only place that touches `VkCtx`. It calls
the appropriate loader, moves the parsed data onto its own members, and
issues GPU upload commands. This separation was the precondition for the
Catch2 test harness — the CC5Sidecar parser now has a fixture-based
test suite, and the MeshImportData default state is testable.

### 4.2 Texture handling uniformity

Both loaders decode textures into `TextureImportData { pixels, width,
height, type, srgb }` — plain RGBA8 byte buffers plus metadata. VkMesh.cpp
iterates them and calls `VkTex::loadFromMemory()`. Failed uploads push a
placeholder `MeshTexture` to preserve material-index alignment (a missing
texture at index 5 becomes a 1×1 fallback, and materials referencing
index 5 still resolve correctly). The previous hand-split implementation
had subtly different failure paths between glTF and FBX; the uniform
`TextureImportData` boundary eliminates that class of bug.

### 4.3 Morph target packing — centralized

Both loaders store morph deltas in CPU vectors (`morphPosDeltas[target]
[vertex]`, `morphNormDeltas[target][vertex]`) plus parallel `morphNames`
and `morphDefaultWeights` vectors. VkMesh.cpp does the SSBO packing (vec4
pairs per target per vertex) and the `"target_N"` fallback naming. This
deduplicates ~40 lines that were nearly identical between the glTF and
FBX paths.

### 4.4 `TINYGLTF_IMPLEMENTATION` moved once

tinygltf requires exactly one `.cpp` file to `#define
TINYGLTF_IMPLEMENTATION` before including the header. VkMesh.cpp held that
responsibility before the split; after, it lives in GltfLoader.cpp.
Consumers (game projects and the lab harness) must not also define it.

### 4.5 The namespace rename to `sv`

The refactor also performed a mechanical namespace rename to `sv` across all
engine sources. The previous namespace was a legacy one carried over from
before StratumV was extracted into a standalone engine. Once the extraction
was complete and multiple separate consumers existed, the legacy namespace was
actively misleading.

The rename was purely mechanical — no semantic changes, zero behavior change —
and scoped to the engine tree. Consumer repos get an equivalent rename on
their next pull.

---

## 5. Scene Loading — `.scene.json` on top of glTF/FBX

`SceneLoader` reads a Blender-authored `.scene.json` file that references
mesh assets by path. Each scene entry has transform (TRS), parent name,
mesh path, and optional `SceneMarker` entries (empties with a marker type
for spawn points, navigation waypoints, etc.).

**Why a JSON wrapper and not raw glTF scene graphs:**

- glTF's scene graph format is great for meshes but awkward for engine-
  specific metadata. Encoding a "spawn point with a team tag" would require
  custom extensions that Blender doesn't export natively.
- JSON is hand-editable, diffable, and version-control-friendly. Artists
  can edit the scene file in any text editor if they need to nudge a
  position.
- `SceneLoader::cullVisible(viewProj)` returns visible node indices using
  `FrustumCuller` with Arvo world-space AABB transform. This
  means culling lives in the engine and doesn't pollute the JSON with
  bounding box data.

Meshes are deduplicated by path via `shared_ptr<VkMesh>` — loading a scene
with 50 barrel instances creates one VkMesh and 50 scene nodes pointing
at it. Material descriptor sets are built per-node via
`MaterialPipeline::buildMaterials()`, allowing per-instance material
overrides later (not yet used in practice).

The JSON schema is documented at `docs/scene_json_schema.json` (JSON
Schema draft-07) and validated by the Blender exporter
(`tools/blender/stratumv_exporter/`).

---

## 6. AssetManifest — Preload List vs AssetBrowser

Two related-but-distinct systems:

- **`AssetManifest`** — parses `assets.json`, returns a preload
  list of named asset entries. `BaseSystemContext::world.assetManifest`
  exposes it to plugins. The engine doesn't upload anything; game code
  iterates the entries and uploads VkMesh / VkTex as needed. Essentially
  a shared lookup table for "what assets should the game have ready".
- **`AssetBrowser`** — recursive filesystem scanner for the
  AdminPanel Assets tab. Walks `assets/` with a
  `std::filesystem::recursive_directory_iterator`, classifies each file
  by extension (compound-suffix-aware: `.scene.json` → Scene,
  `.meta.json` → Other+ignored), and caches per-asset `ImportSettings`
  (scale, up-axis, material mapping, preload) in memory. Provides the
  UI-facing filter/search/selection state for the Assets tab.

The AssetBrowser can **populate** an AssetManifest via `populateManifest()`,
bridging from scan-results to preload-list. That's how the AdminPanel
"save manifest" flow will work in future.

**Why the split:** AssetManifest is a runtime data structure that plugins
read through a stable pointer; AssetBrowser is a UI+cache that lives in
the engine's main-thread ImGui tab and doesn't need to cross the DLL
boundary. Smushing them together would have meant plugins could poke at
ImGui state, which is not allowed.

---

## 7. Format Support Matrix

The fully-supported import format table from `ARCHITECTURE.md`:

| Format | Library | Mesh | Skeleton | Animations | Morph Targets | Textures |
|--------|---------|------|----------|------------|---------------|----------|
| glTF / GLB | tinygltf | Yes | Yes (skin) | Yes | Yes (glTF morph targets) | Embedded or external |
| FBX | ufbx v0.21.3 | Yes | Yes (multi-mesh) | Yes (multi-clip, baked) | Yes (facial blend shapes) | Embedded or external |

Notable asymmetries:

- **glTF morph targets vs FBX blend shapes.** Same concept, different
  spelling. Both loaders populate `MeshImportData::morphPosDeltas` the
  same way; the SSBO pipeline doesn't care which format the data came
  from.
- **FBX animation baking at 30 FPS.** `loadFbxAnimations()` samples via
  `ufbx_evaluate_transform()` at 30 FPS and builds an ozz `RawAnimation`.
  The 30 FPS rate is a compromise — Euler/quaternion conversion, pre-
  rotation, and inherit modes are all handled by ufbx automatically, so
  sampling is the robust way to avoid losing data. Higher rates make
  clips bigger; 30 FPS is the standard CC5 / iClone export rate.
- **CC5 body morphs bake into the mesh.** CC5's body-shape morphs
  (weight, muscle) are applied at export time and frozen into the vertex
  data. Only facial blend shapes (262 HD / 63 ExpressionPlus) survive as
  animatable morph targets.

---

## 8. Things the Pipeline Deliberately Doesn't Do

- **No USD import.** USD is the right answer for large-team pipelines but
  adds ~100 MB of library weight for a feature the target games don't use.
  Revisit if we ever need to share assets with an external Maya/Houdini
  pipeline.
- **No `.obj` / `.3ds` / `.dae` (Collada) importers.** Legacy formats with
  no advantage over glTF for static meshes. Artists export via Blender or
  CC5; both output the formats we support.
- **No runtime DCC cache (`.abc` Alembic, `.ass` Arnold).** Renderer-side
  concerns; not a runtime format.
- **No Assimp.** We evaluated Assimp early and rejected it: too many
  formats (increases maintenance surface), implicit conversion behavior
  (harder to debug than our explicit axis conversion), and heavier build
  cost than tinygltf + ufbx combined.
- **No runtime mesh simplification / LOD generation.** LODs come from the
  DCC tool as separate meshes. The engine doesn't generate them at load
  time.
- **No procedural mesh generation.** Procedural content is authored as a
  Blender geometry-nodes addon that runs in Blender and exports the result
  via `.scene.json`. The engine doesn't run geometry nodes itself.

---

## 9. Related Files

- `src/engine/vk/VkMesh.h` — public mesh API + types (`MeshVertex`,
  `SubMesh`, `MeshMaterial`, `MeshTexture`, `SkeletonData`, `BlendMode`)
- `src/engine/vk/VkMesh.cpp` — 132 LOC thin GPU-upload dispatcher
- `src/engine/vk/MeshImportData.h` — CPU-side intermediate
- `src/engine/vk/GltfLoader.h/.cpp` — tinygltf path (`loadGltf()`)
- `src/engine/vk/FbxLoader.h/.cpp` — ufbx path (`loadFbx()`) with
  ADJUST_TRANSFORMS + multi-mesh bone remap
- `src/engine/vk/CC5Sidecar.h/.cpp` — `parseCC5Sidecar()` free function
- `src/engine/AnimationSystem.cpp` — `loadFbxAnimations()` with
  intermediate bone chaining
- `src/engine/SceneLoader.h/.cpp` — `.scene.json` parser, mesh dedup,
  frustum culling
- `src/engine/MaterialPipeline.h/.cpp` — 7-binding descriptor sets (UBO +
  6 textures incl. opacity)
- `src/engine/AssetManifest.h/.cpp` — preload list parser
- `src/engine/AssetBrowser.h/.cpp` — filesystem scanner + import settings
  cache
- `docs/scene_json_schema.json` — JSON Schema for `.scene.json`
- `tools/blender/stratumv_exporter/` — Blender addon for `.scene.json`
  export with auto-fix operators
- `ARCHITECTURE.md` → "Asset Import Formats" and "FBX Axis + Unit
  Conversion Strategy" — the structural summary this doc complements
