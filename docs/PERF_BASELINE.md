# PERF_BASELINE — StratumV Performance Baseline

> This file is the baseline performance reference for StratumV. Update
> it whenever the engine grows or shrinks observable per-frame work, so
> we have a fixed point for "did we regress?" and "do the network
> observability fields actually surface?"

## Why this exists

The networking substrate cannot be used in production until performance is
observable. Sync bugs in a 256-player authoritative server are
undiagnosable without bandwidth, packet rate, and tick budget counters
visible alongside frame time and draw counts. The performance baseline is
a prerequisite of the networking transport for this reason — see
`docs/NETWORK_DESIGN.md §7`.

The baseline lands the **plumbing**: the `PerformanceContext` sub-struct
on `BaseSystemContext`, the AdminPanel HUD that reads it, and the
lab harness wiring that populates it each frame. The networking layer
then fills in the six `NetworkStats` fields:
`tickMs`, `bytesPerSec`, `packetsPerSec`, `replicatedEntityCount`,
`ackLatencyMs`, `droppedDatagramPct`.

## What gets measured

```
sv::PerformanceContext {
  PerformanceBudget budget;           // game configures
  float    frameTimeMs;               // wall-clock per frame
  float    cpuFrameTimeMs;            // CPU work this frame
  float    gpuFrameTimeMs;            // GPU work (sum of profiler passes)
  float    avgFps;                    // running mean over the run
  uint32_t drawCallCount;             // submitted vkCmdDraw* / frame
  uint32_t triangleCount;             // sum of submesh indices/3
  float    vramUsedMB;                // device-local heap usage (VMA)
  float    vramBudgetMB;              // device-local heap budget (VMA)
  NetworkStats network;               // 6 networking placeholders
}
```

The lab harness `TestEngine` populates every field except
`gpuFrameTimeMs` and the `network.*` block. The `GpuProfiler`
already exists in the engine (`src/engine/graph/GpuProfiler.h`) but
is not enabled in the lab harness because of a pre-existing main
render pass bug — wiring the profiler is deferred until that is
resolved.

## Reference hardware

| Component | Value |
|-----------|------:|
| GPU       | RTX-class device, 24 GB device-local heap (`vramBudgetMB ≈ 23554`) |
| CPU       | Modern x64 desktop |
| OS        | Windows 11 |
| Build     | VS 17 2022, Release, SHARC + NTC off |
| Display   | High-refresh (≥120 Hz), vsync on |

## Lab harness baseline (skinned_test, --auto-exit)

The `--auto-exit` mode runs ~26 frames against the CC5 Main_Female
character (`HQ-Main_FM_CCExport.fbx`, 18 submeshes / 17 materials /
56 textures / 63 joints / 107 840 verts / 160 902 indices / 8
morph targets), captures one PPM screenshot at frame 15, then
prints the live `PerformanceContext` snapshot to stdout before
exiting.

### Three consecutive runs

```
[perf] frames=26  frameTime=8.370 ms  avgFps=245.3  cpu=4.079 ms  draws=18  tris=53634  vram=1336.8/23554.0 MB
[perf] frames=26  frameTime=8.302 ms  avgFps=268.0  cpu=4.125 ms  draws=18  tris=53634  vram=1336.8/23554.0 MB
[perf] frames=26  frameTime=8.332 ms  avgFps=267.9  cpu=4.151 ms  draws=18  tris=53634  vram=1336.8/23554.0 MB
```

### Steady-state numbers

| Metric | Value | Notes |
|--------|------:|-------|
| `frameTimeMs` (last frame) | **~8.33 ms** | vsync-bounded; 1000/8.33 ≈ 120 fps |
| `cpuFrameTimeMs` | **~4.1 ms** | one full lab-harness frame on the main thread (animation update + ImGui + record) |
| `gpuFrameTimeMs` | 0.0 | GPU profiler not enabled in the lab harness |
| `avgFps` (true mean) | **~260 fps** | skewed high by the pre-vsync warm-up frames; the LAST frame at 8.33 ms is the steady-state value |
| `drawCallCount` | **18** | one draw per CC5 submesh (7 opaque + 11 alpha-blended) |
| `triangleCount` | **53 634** | `160 902 / 3` indices |
| `vramUsedMB` | **~1 337 MB** | full CC5 mesh + 56 textures + 8 morph SSBO + ImGui font + Vulkan swapchain |
| `vramBudgetMB` | **23 554 MB** | device-local heap capacity reported by VMA |

### Reproducibility

`frameTimeMs` is reproducible to within ±0.07 ms across the three
runs (vsync provides the natural lower bound). `cpuFrameTimeMs`
varies by ±0.07 ms because the main thread also services GLFW
events and ImGui input. The CC5 mesh load is deterministic →
`drawCallCount`, `triangleCount`, and `vramUsedMB` are bit-stable
across runs.

The variability in `avgFps` (245 → 268) reflects how many of the
26 frames were captured before vsync had a chance to stabilise:
the very first frame of `EngineBase::run` has `dt ≈ 0` because
`prev = now` is taken just before the loop starts, and the next
few frames are dominated by Vulkan command-buffer initialisation
and ImGui font upload. The steady-state value lives in the LAST
frame's `frameTimeMs`, not the running mean.

## Default budget

```cpp
PerformanceBudget {
  targetFps    = 60.0f
  maxFrameMs   = 16.67f      // 1000 / targetFps
  maxDrawCalls = 10 000
  maxTriangles = 5 000 000
  maxGpuMemMB  = 8 192
}
```

These are intentionally conservative — the AdminPanel HUD turns the
frame-time bar yellow at 75 % of `maxFrameMs` and red at 100 %, so
the lab harness's 8.33 ms frame is solidly green against the 16.67
ms default budget. Games override the defaults by writing to
`ctx.perf.budget` in their game-specific `SystemContext` wiring.

### Budget-vs-actual against the default for the lab harness

| Metric | Budget | Actual | % | Status |
|--------|------:|------:|---:|:------:|
| frameTimeMs    | 16.67 ms      | 8.33 ms     | 50 %  | ✅ green |
| drawCallCount  | 10 000        | 18          | 0.18% | ✅ green |
| triangleCount  | 5 000 000     | 53 634      | 1.07% | ✅ green |
| vramUsedMB     | 8 192 MB      | 1 337 MB    | 16 %  | ✅ green |

The CC5 lab is well below every default budget — the budget is
calibrated for a real game scene with hundreds of
draws and millions of triangles, not a one-character animation
test.

## Network observability (placeholders)

The six fields specified in `docs/NETWORK_DESIGN.md §7` are wired
as zero-valued placeholders inside `PerformanceContext::network`.
The AdminPanel renders them in a "Network Stats (placeholders)"
section that reads `(no network session active)` until the actual
networking layer is live.

| Field | Type | Populated by | Unit |
|-------|------|--------------|------|
| `tickMs`                | float    | server loop                 | ms |
| `bytesPerSec`           | uint64_t | transport callback          | bytes/s |
| `packetsPerSec`         | uint32_t | transport callback          | pkts/s |
| `replicatedEntityCount` | uint32_t | snapshot generator          | entities |
| `ackLatencyMs`          | float    | reliable ack stream         | ms |
| `droppedDatagramPct`    | float    | QUIC loss telemetry         | 0..1 |

The baseline does NOT populate any of them. The contract is:
- Field added now → AdminPanel HUD knows where to read.
- DLL plugins compiled against this layout → can already reference
  `ctx.perf.network.*` without conditional compilation.
- Networking goes live → fields fill in → HUD automatically lights
  up.

A future semver bump (1.2.0 → 1.3.0) may move `NetworkStats` out
of `PerformanceContext` and into a new `NetworkContext` sub-struct
alongside `INetworkContext*`. That refactor is one-line-per-call
because the field path will change from `ctx.perf.network.tickMs`
to `ctx.network.stats.tickMs` — ackable in the migration table.

## How to take a fresh measurement

Lab harness, single auto-exit run:
```sh
cmake --build build --config Release --target skinned_test
cd build/lab/skinned_test/Release
./skinned_test.exe --auto-exit
```

Read the line beginning with `[perf]` from stdout. The captured
frame lives at `capture.ppm` (PPM, 1280×720) — convert to PNG with
PIL:
```sh
python -c "from PIL import Image; Image.open('capture.ppm').save('capture_perf1.png')"
```

Add the steady-state numbers to a new section here, dated, when
the engine adds or removes a meaningful chunk of per-frame work
(new render pass, new draw, new SSBO upload, etc).

## Out of scope

These were considered and deferred:

- **CI regression check.** A perf gate that fails the build if
  `frameTimeMs > 1.5 × baseline` would catch silent regressions
  but requires an NVIDIA T4 CI runner. Manual baseline updates
  work for now.
- **Heat-map / flame chart.** The existing `GpuProfiler` slot
  array is enough for the AdminPanel HUD; full flame-chart
  visualisation belongs in a follow-up.
- **Per-DLL plugin perf attribution.** Plugins can already write
  to their own slots in `gpuPassMs` via the existing graph
  registration; the per-plugin attribution UI is separate.
- **Lab harness GpuProfiler enable.** Would populate
  `gpuFrameTimeMs` but is blocked on the pre-existing main-pass
  render bug noted above.
- **Per-game baselines.** Each game owns its
  own perf sweep; this file is the engine baseline only. Games
  can extend with their own `docs/PERF_BASELINE_*.md` if useful.

## History

| Date | Scope | Key numbers |
|------|-------|-------------|
| 2026-04-10 | Initial baseline; lab harness CC5 character | 8.33 ms / 18 draws / 53 634 tris / 1 337 MB VRAM |
