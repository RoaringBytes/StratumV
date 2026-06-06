# ANIMATION_DESIGN — Rationale

> Decision-layer companion to `ARCHITECTURE.md`. This doc explains **why** the
> animation stack looks the way it does. For the what — module list, field
> counts, shader bindings — see `ARCHITECTURE.md` and `src/engine/Animation*`.

The character pipeline was built incrementally, including supporting work on
morph targets, FBX import, and the material sidecar. This document records the
trade-offs made at each branching point so they don't get relitigated.

---

## 1. Why ozz-animation

Six animation libraries were evaluated up front. ozz-animation 0.16.0 was
chosen for:

- **MIT license.** Compatible with commercial use, and
  carries no GPL / copyleft baggage.
- **SoA + SIMD from the ground up.** ozz stores local transforms as
  `SimdFloat4`-packed structure-of-arrays, operates via SSE2/NEON intrinsics
  with scalar fallback, and runs blending/L2M/skinning as explicit "jobs"
  whose memory access is predictable. Benchmarks at 250 joints / 4 layers
  come in under 30 µs on a single thread.
- **FetchContent-friendly.** ozz builds as a plain CMake library, so
  `FetchContent_Declare` + `FetchContent_MakeAvailable` is the entire
  integration story. No submodules, no manual vendoring.
- **Stateless by design.** ozz doesn't own a state machine, doesn't own a
  render object, doesn't own a skeleton registry. It gives you sampling,
  blending, and L2M jobs and leaves control flow to the engine. That
  matches how StratumV wants to own the state machine and ECS glue (see §4).
- **Root motion support.** Offline `AdditiveAnimationBuilder` and
  `MotionBlendingJob` handle the tricky cases (additive clips, root delta
  extraction) without forcing us to roll our own.

Rejected alternatives:

| Library | Rejection reason |
|---------|------------------|
| Ragdoll / Harfang | Closed source or engine-tied |
| Acclaim / BVH direct parsers | No retargeting, no SIMD, no tooling |
| Custom | "We can do that ourselves" always costs two months |

See `ARCHITECTURE.md` for the full decision table.

### 1.1 Version lock at 0.16.0

`CMakeLists.txt` pins ozz at tag `0.16.0`. Newer 0.x releases introduce
breaking changes to the `RawAnimation` / `RawSkeleton` offline builder API,
which would force us to rewrite `buildSkeleton()` and the FBX/glTF animation
conversion paths. Any upgrade will be a dedicated, isolated change.

### 1.2 Static CRT mismatch

`ozz_build_msvc_rt_dll` defaults OFF, which builds ozz against the static
CRT (`/MT`). The engine and lab harness both use the dynamic CRT (`/MD`).
Linking ozz to anything downstream produced CRT mismatch errors. The fix
was to set `ozz_build_msvc_rt_dll ON` globally in the root CMakeLists. Every
consumer now gets the dynamic-CRT build automatically.

---

## 2. GPU Bone Palette — SSBO, not UBO

Bones upload to GPU as a single SSBO with std430 layout (mat4 bones[]).
Each draw call pushes its own `boneOffset` via push constant, indexing into
the shared buffer.

**Why SSBO and not UBO:**

- **UBOs cap out around 64 KB on Intel / 16 KB guaranteed.** A single
  character can have 128+ bones → 128 × 64 = 8 KB, so three characters
  already blow a guaranteed UBO budget. SSBOs have no such limit in
  practice — the bone palette buffer is currently sized at 4096 bones
  (256 KB), enough for ~30 simultaneous characters before we need to
  upgrade.
- **Single descriptor set binding across all skinned draws.** With UBOs,
  every character would need its own descriptor set update or a dynamic
  offset. With an SSBO plus a push-constant `boneOffset`, one descriptor
  set covers every draw in the frame. Descriptor binding churn at 60+ draws
  per frame is measurable; eliminating it was free performance.
- **std430 packing is nicer.** std140 aligns mat4 rows to 16 bytes with
  padding rules that make bone data awkward to upload. std430 matches the
  C++ `glm::mat4` layout byte-for-byte, so `memcpy` from ozz `Float4x4`
  to mapped SSBO memory just works.

**Per-frame SSBO lifecycle:**

```cpp
ctx.animation.resetBonePalette();            // begin frame — reset write cursor
// for each skinned character:
uint32_t boneOffset = ctx.animation.uploadBones(inst);  // append to SSBO
ctx.animation.flushBonePalette();             // end frame — barrier before draws
// record skinned draws using the saved boneOffset per character
```

**Mapped-host-visible vs staging:** The bone palette buffer is persistently
mapped `CPU_TO_GPU` (`VMA_MEMORY_USAGE_CPU_TO_GPU`). The buffer is small
(<256 KB), writes are sequential, and the buffer is consumed on the same
frame it's written, so a staging buffer would just be two copies where one
suffices. If profiling ever shows CPU_TO_GPU as a bottleneck the buffer can
be upgraded to device-local + staging without any API change. Single-buffered
(not double-buffered) because the read happens inside the same frame's
submission — no cross-frame hazard.

### 2.1 Why per-draw `boneOffset` over per-character descriptor sets

The alternative design is "one descriptor set per character, each pointing
at a slice of a giant buffer". We rejected it because:

- Descriptor set allocations would track entity lifetimes, adding an
  allocator coupling between animation and ECS.
- Descriptor set updates are more expensive than push constant writes on
  most GPUs (validation + driver-side pipeline cache invalidation).
- Push constants are free per draw anyway — we already spend 72 bytes on
  `mat4 model + uint cascadeIndex + uint boneOffset` for shadows, and 128
  bytes on `SkinnedPushConstants` for the main pass.

---

## 3. Vertex Shader Skinning, 4 Weights

`shaders/skinned.vert` performs 4-weight linear blend skinning in the vertex
shader:

```glsl
vec4 pos = vec4(0.0);
for (int i = 0; i < 4; ++i) {
    mat4 bone = bones[pushConstants.boneOffset + int(vJoints[i])];
    pos += vWeights[i] * (bone * vec4(vPosition, 1.0));
}
```

**Why vertex shader and not compute:**

- **Simpler pipeline.** A compute pre-pass would need a transient skinned-
  vertex buffer, a compute→graphics barrier, and a descriptor set for the
  compute stage. Vertex shader skinning uses the same pipeline the static
  mesh renderer uses, minus the VBO attribute layout.
- **Sufficient for typical character LoD.** CC5 characters cap at ~108k
  vertices. At 9 characters in a scene, that's ~1M skinned vertices per
  frame. A vertex shader handles this at >500 FPS on mid-range GPUs.
- **Compute skinning wins at 4+ passes.** Games that re-read the skinned
  mesh (shadow, GBuffer, velocity, reflection) benefit from computing the
  skinned vertex once and sharing it. StratumV currently does one geometry
  pass + one shadow pass, which is two vertex shader evaluations per
  character — acceptable.

**Why exactly 4 weights:** glTF spec mandates JOINTS_0 / WEIGHTS_0 as 4-wide
vectors. Going to 8 weights would require JOINTS_1 / WEIGHTS_1 which CC5
doesn't export. Going to 2 weights would visibly degrade neck and shoulder
deformation. CC5 rigs bind with at most 4 influences per vertex, so 4 is
the natural fit.

**Normal transform is approximate:** The skinned vertex shader uses
`mat3(model) * mat3(skin) * normal` instead of the full inverse-transpose.
This is correct for uniform scale and close enough for game characters
with subtle non-uniformity. If visible artifacts ever appear on non-
uniformly-scaled meshes the fix is a one-line change in the shader.

---

## 4. Engine-Owned State Machine

ozz is stateless. The engine owns the state graph in
`src/engine/AnimationStateMachine.h/.cpp`. A state machine holds:

- A `std::vector<AnimState>` where each state carries a non-owning
  `const AnimationClip*` plus a `bool looping`.
- A `std::vector<AnimTransition>` with `fromState`, `toState`, `duration`,
  `TransitionMode`, and a fixed-size `char trigger[32]`.
- The current state index, the optional next state (during crossfade), the
  current trigger flag, and the current time within the active state.

`getBlendLayers()` returns a vector of `BlendLayer` with sampled locals for
the active and (during a transition) the outgoing state, with weights
interpolated by smoothstep:

```cpp
float t = progress;
float smooth = t * t * (3.0f - 2.0f * t);
// ... weight active and outgoing states
```

**Why smoothstep instead of linear:** Linear crossfades have a visible
velocity discontinuity at the start and end of the transition — the pose
pops onto the new clip. Smoothstep (`t²(3−2t)`) has zero first derivative
at both endpoints, producing a transition that visually eases in and out.
The cost is three multiplies per frame during a transition — free.

**Why DLL-safe string triggers:**

```cpp
char trigger[32];  // not std::string
```

Fixed-size char arrays cross the DLL boundary without any ABI concern.
`std::string` crosses fine when both sides use the same STL, but a trigger
passed from a DLL plugin's `IAnimationController` through the state machine
would cross two boundaries (plugin → engine → state machine) and the char
array is simply the cleanest way to make that zero-ambiguity. Matches the
pattern Unreal (`FName`) and Unity (string hashes) use.

### 4.1 `IAnimationController` is per-entity, not a system

`IAnimationController` is explicitly **not** an `IModularSystem`. Controllers
are per-entity (one per animated character) and managed by game code. The
engine only calls them via `AnimatorComponent::controller`. Games write
their own controller plugins for game-specific logic (patrol behaviors,
combat states, dialogue poses) and wire them onto ECS entities.

This avoids a class of bug where one `IModularSystem` controller would
have to iterate the ECS looking for animated entities and dispatch per-
entity logic itself — that's ECS work, and it belongs in game code, not
the plugin interface.

### 4.2 `AnimatorComponent` holds the state machine

`AnimatorComponent` is an ECS component with three playback modes, each
mutually exclusive at any given frame:

1. **Legacy direct playback** — `activeClipIndex` + `timeCursor` sample a
   single clip. Used by simple characters (background agents with one
   idle clip) and the lab harness's baseline test.
2. **State machine** — `unique_ptr<AnimationStateMachine> stateMachine`
   drives sampling. The system calls `stateMachine->update(dt)`, reads
   `getBlendLayers()`, passes them to `AnimationSystem::blend()`.
3. **Blend tree** — game code constructs an `AnimBodyLayer` tree and calls
   `AnimationSystem::blendBodyLayers()` directly. The state machine may be
   a leaf of this tree or replaced wholesale.

The state machine is a `unique_ptr` rather than a value type so that
`AnimatorComponent` stays lightweight for the many characters that don't
need a state machine.

---

## 5. Blend Tree — Modular Node Abstraction

The blend tree layer sits on top of the state machine. The core types are in
`src/engine/BlendTree.h/.cpp`:

- `IBlendNode` — abstract base with `evaluate(dt, output)`.
- `ClipNode` — samples a single `AnimationClip` with looping + playback
  speed.
- `RestPoseNode` — outputs the skeleton's rest pose (T-pose). Useful as
  the "zero" input for partial body blending.
- `BlendSpace1D` — parametric 1D blend across N sorted threshold entries.
  Used for locomotion: (speed=0, idle), (speed=1.5, walk), (speed=5, run).

### 5.1 `evaluate(dt, output)` combines time + pose

Each node owns its own ozz `SamplingJob::Context` for cache coherency
across frames. An earlier design split time advancement from pose
evaluation into two calls, but that made node ownership awkward (who owns
the time cursor? the tree or the leaf?). Combining them in `evaluate` means
the leaf owns its own clock, and the parent just forwards `dt` down the
tree. Simpler, fewer bugs.

### 5.2 Sorted-threshold `BlendSpace1D`

`BlendSpace1D` entries are sorted by threshold at construction. Evaluation
binary-searches for the two adjacent entries bracketing the current
parameter, samples both, and blends with an ozz `BlendingJob`. Rest pose
entries (with a null clip) output the skeleton rest pose, enabling
"parameter = 0 → rest pose, parameter = 1 → walk clip" style fades.

This beats the alternative of hashing into a grid (requires uniform
spacing) and is simpler than a k-d tree (premature for 1D).

### 5.3 Partial body blending via SoA joint masks

ozz's blending job accepts a per-joint weight buffer. A weight of 0
excludes a joint from a layer; 1.0 lets it pass through. We wrap this in
`buildJointMask(skeleton, rootIdx)`:

```cpp
// Walk depth-first from rootIdx forward, marking descendants.
// ozz stores joints depth-first, so parents always precede children —
// a simple linear scan is O(jointCount).
```

The result is a `ozz::vector<SimdFloat4>` with 4 joint weights per element
(SoA packing matches ozz's internal layout). Games pick a "split joint"
(spine_02 is the canonical upper/lower body split for bipeds) and build
two masks: one with descendants of spine_02 set to 1 (upper body), one
with everything else set to 1 (lower body). The upper body layer can then
play an aim clip while the lower body plays a walk clip.

**Why depth-first propagation and not recursive marking:** Recursive
marking would need an explicit stack or function call overhead per joint.
ozz joints are depth-first, so a single linear pass with boolean
propagation produces the same mask in O(n).

### 5.4 Additive blending (wired, untested)

`AnimBodyLayer::additive` is a boolean flag routing the layer to
`BlendingJob::additive_layers` instead of `layers`. Additive clips must be
pre-processed with ozz `AdditiveAnimationBuilder` (offline conversion),
which we have not integrated because no content pipeline currently
exercises it. The runtime plumbing is ready; when a game needs additive
poses (weapon recoil added on top of base motion), the missing piece is
the offline clip conversion step.

---

## 6. IK — Two-Bone + Aim, Post-Blend

IK runs as a post-processing pass. Two-bone IK handles feet (hip→knee→ankle)
and arms (shoulder→elbow→wrist); aim IK handles head look-at and spine
pointing. Both use ozz's `IKTwoBoneJob` and `IKAimJob`.

```cpp
animationSystem.computeSkinningMatrices(inst, bindPose);
animationSystem.applyIK(inst, bindPose,
                         animator.twoBoneIK, animator.twoBoneIKCount,
                         animator.aimIK,     animator.aimIKCount);
// applyIK writes corrections back into locals and re-runs L2M
```

### 6.1 SoA quaternion correction via scalar extraction

ozz's IK job produces a scalar (`ozz::math::Quaternion`) correction. The
skeleton's local rotations are SoA `SimdFloat4`, so the correction has to
be re-packed into the right lane. `applySoACorrection()` extracts the
affected 4-joint pack to scalar arrays, Hamilton-products the correction
at the target lane, and stores back.

This is not SIMD-optimal — it's four scalar multiplies and a store. But
IK chains are short (≤6 joints affected per frame across all characters
in the scene), so this is not a hot path. The simple implementation costs
a few microseconds; an SIMD version would save single-digit microseconds
and be much harder to audit.

### 6.2 Full L2M re-run after IK

After applying local corrections, `computeSkinningMatrices()` runs a full
LocalToModel pass over the whole skeleton. In theory you can run a partial
L2M starting from the affected joint subtree, which ozz supports via the
`from` parameter. We didn't bother because:

- The full L2M is ~30 µs for a 128-joint skeleton.
- Partial L2M would require tracking affected subtrees per IK slot.
- IK slots are 4 (feet) + 2 (aim) max per character.

If profiling ever shows L2M as a bottleneck, the `from` parameter is the
first optimization.

### 6.3 Fixed-size IK slot arrays

`AnimatorComponent` holds `TwoBoneIKSlot twoBoneIK[4]` and
`AimIKSlot aimIK[2]` as fixed arrays, not `std::vector`. Why:

- Most characters use 0 or 2 IK slots (two feet). Pre-allocating 4 slots
  per character costs ~400 bytes — negligible.
- Vectors would heap-allocate on first use, which matters for ECS
  initialization performance.
- Fixed arrays cross the DLL boundary cleanly.

---

## 7. Root Motion

`AnimationSystem::extractRootMotion(inst, prevRootPos, prevRootRot)` reads
the root joint's current local translation and rotation, computes the
delta against the previous frame, and zeros the XZ translation in locals
so the mesh doesn't drift in world space.

```
Frame N:   root.pos = (0, 1.0, 0.3)   ← clip pushes forward
Frame N+1: root.pos = (0, 1.0, 0.4)
delta = (0, 0, 0.1)                    ← applied to entity world transform
locals[root].pos.xz = 0                ← mesh stays at entity origin
```

### 7.1 XZ-only zeroing

Vertical translation (Y) is preserved in the local transform. Games that
want "jump up" or "crouch down" motion in their clips get it via the root
bone's Y position. XZ is the ground plane — that's what games apply to
the entity transform to drive locomotion.

### 7.2 Rotation delta computed, not applied

`RootMotionDelta` contains both `glm::vec3 position` and `glm::quat
rotation`. The rotation delta is reported but NOT zeroed in locals. Games
that want turn-in-place root rotation can zero the Y-rotation component
themselves; games that don't (most) just let the clip rotate the mesh
directly. Deferring the choice to game code was the least-committal design.

### 7.3 Why extraction happens before L2M

`extractRootMotion()` reads the root joint's SoA local translation directly,
before `computeSkinningMatrices()` converts locals to model-space matrices.
This avoids the round-trip through world space, and is robust against IK
corrections (IK runs after L2M and doesn't touch the root).

---

## 8. ECS as the Glue, Not a Parallel System

Skinned characters are ECS entities. An entity with a skinned mesh has:

- `TransformComponent` (world position / rotation / scale).
- A mesh pointer or handle.
- `AnimatorComponent` (skeleton, clips, state machine, IK slots, root
  motion state).

No component registry lives in the animation system. No parallel "scene
graph for animated things" exists. The renderer iterates ECS entities
with a view on `AnimatorComponent` + mesh + transform, and that's the
skinned draw list.

This is the model described in `ARCHITECTURE.md`:

> ECS integration — All scene/animation data lives on EnTT entities.
> SceneLoader populates entities (TransformComponent + mesh ref);
> AnimatorComponent carries skeleton, clips, and blend weights; game plugins
> mutate ECS; renderer iterates entities by component. ECS is the glue,
> not a parallel system.

**Why this matters:** an earlier system maintained a
separate "CharacterPool" parallel to its ECS. It was a constant source of
desync bugs — entity deleted in ECS, character handle still valid in the
pool. StratumV unified on ECS from the start and has not had a desync bug in
the skinned path since.

---

## 9. What the Animation System Does NOT Own

- **Morph targets.** Skeletal animation in ozz operates on joints only;
  morph targets are per-vertex blend shapes (facial expressions,
  lip-sync, body shape). StratumV has a separate `MorphTargetTypes.h/.cpp`
  system that uploads delta SSBOs, blends up to 8 targets per
  draw in the vertex shader, and composes with the 4-weight skinning
  transform. ozz has no opinion on any of this.
- **Retargeting.** If you need to play a Mixamo clip on a CC5 skeleton,
  you bake the clip to the CC5 rig offline (Blender + cc_blender_tools)
  and load the result. Runtime retargeting would require a name-based
  joint mapping table and is deferred indefinitely — artists can iterate
  on retargets in Blender much faster than any runtime approximation.
- **Physics ragdoll.** Rigid-body ragdolls live in `IPhysicsContext` via
  Jolt. The interaction with animation is: animation drives kinematic
  bodies for live characters; switching to ragdoll means the physics
  sim drives the bones. The hand-off is game-side; the animation system
  is just turned off on that entity.
- **Cloth, hair, fur simulation.** Not yet wired. When these arrive they
  go into their own subsystem that samples the bone palette and outputs
  its own vertex deformation pass.

---

## 10. Lab Harness — `lab/skinned_test`

`lab/skinned_test/` is a standalone executable extending `EngineBase` that
loads a single CC5 character, runs the animation pipeline, and renders to
a window. It's the primary testbed for animation features — smaller than
a minimal scene: no DLL plugins, no post-process, single character, minimal
UI.

Each animation feature was brought up in the harness as a visible milestone:

- T-pose only, proving the skinning pipeline works end-to-end.
- PBR textures, proving per-material descriptor sets work.
- State machine + clip playback.
- Blend tree with upper/lower body split.
- Foot IK with ImGui target sliders.

The harness has a framebuffer capture path that writes a PPM file after
a specified frame count. This was the reproducibility tool for the FBX axis
conversion work — confirm the character renders upright in the captured
frame, then ship.

---

## Related Files

- `src/engine/AnimationTypes.h` — `SkeletonHandle`, `AnimationClip`,
  `buildSkeleton()`, `loadGltfAnimations()`, `loadFbxAnimations()`
- `src/engine/AnimationSystem.h/.cpp` — sample / blend / L2M / skinning /
  SSBO / IK / root motion
- `src/engine/AnimationStateMachine.h/.cpp` — state graph + smoothstep
  crossfade
- `src/engine/BlendTree.h/.cpp` — `IBlendNode`, `BlendSpace1D`,
  `AnimBodyLayer`, `buildJointMask`
- `src/engine/IAnimationController.h` — per-entity DLL plugin interface
- `src/engine/AnimatorComponent.h` — ECS component (legacy / state machine /
  blend tree / IK / root motion state)
- `src/engine/passes/SkinnedMeshPass.h/.cpp` — 4-set pipeline layout,
  opaque + alpha-blend pipelines, 128 B `SkinnedPushConstants`
- `shaders/skinned.vert` — morph target blending + 4-weight skinning
- `shaders/skinnedPBR.frag` — full PBR fragment
- `lab/skinned_test/main.cpp` — visual test harness
- `ARCHITECTURE.md` — structural view + decision table
