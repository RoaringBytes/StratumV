# StratumV — Vision

**Maintainer:** RoaringBytes
**Engine:** StratumV 1.3.10
**License:** Apache-2.0
**Tagline:** A layered Vulkan engine. Multiplayer-first. Live systems. Collaborative by default.

---

## What StratumV Is

StratumV is a game-agnostic, open-source Vulkan 1.3 engine you can build any game
on. It pairs a high-end renderer with a networking-first data model and a
hot-reloadable plugin system, so gameplay iterates without recompile-restart cycles.

- **High-end Vulkan 1.3 rendering** — a render graph with RT shadows, ReSTIR DI, and post-processing, plus optional NVIDIA DLSS / SHARC / neural texture compression.
- **Hot-reloadable DLL game systems** — gameplay lives in plugins implementing `IModularSystem`; no recompile-restart cycle during development.
- **Multiplayer as substrate, not an add-on** — an authoritative server plus reflection-based replication over QUIC (MsQuic). Games and editor clients run through the same transport with permission scopes.
- **Live collaborative development** — two or more developers editing the same running world in real time: scene transforms, materials, entities, asset pushes, and a shared undo log. See `docs/COLLAB_EDITING.md`.
- **Developer ergonomics** — a live admin panel, an optional loopback debug server, and JSON scene-state persistence.

The engine compiles directly into your game binary. You provide your game's
`SceneUBO`, a `GameSystemContext`, and your game systems; StratumV provides the
infrastructure. A project generator (`tools/scaffold_game.py`) creates a starter
project already wired to the engine.

## Design Philosophy

**Frozen core (Layers 1–3).** The Vulkan abstraction, render graph, and built-in render passes lock after initial authoring — no new render passes in Layers 1–3 without a StratumV version bump. Stability over flexibility. Layer 4 services (ECS, animation, physics, networking, replication) continue to evolve. See `ARCHITECTURE.md`.

**Multiplayer-first.** Networking is the substrate the engine is built on, not a bolt-on. `SV_REPLICATE`-marked components flow through an authoritative server over QUIC. Single-player = one local server + one client on the same binary, same code path. See `docs/NETWORK_DESIGN.md` and `docs/REPLICATION_CONTRACT.md`.

**Collaborative by default.** Editor, player, spectator, and admin are permission scopes on the same connection type, not separate application modes. Two developers joining the same running world and editing it together is the same feature as two players playing together. See `docs/COLLAB_EDITING.md`.

**Modular everything.** If it changes during development, it is a DLL plugin. The engine never recompiles for gameplay changes.

**Game contract over engine inheritance.** Games provide their own `SceneUBO` and `GameSystemContext`. The engine does not dictate game content — it provides the infrastructure.

**Stability by design.** Semantic versioning, ABI version checks at DLL load, and a structured test harness exist to guarantee that one game can build on StratumV without breaking another. CI gates, validation layers, and headless render-regression tests back the frozen-core promise with automated proof.

## Live Collaborative Development

Live collaborative development is the differentiating feature that shaped the
networking architecture. The basic promise: two or more developers joining the
same running game world, editing it together in real time, and seeing each
other's changes as they happen — no separate editor application, no file-level
merges, no restart cycle.

**How it works.** StratumV uses an authoritative-server model with reflection-based replication (`SV_REPLICATE` macros on selected components). Every connected client holds a permission scope — `Spectator`, `Player`, `Editor`, or `Admin`. Editor-scope clients mutate `Authority::Editor`-marked components via edit transactions: discrete, authored, undoable units of world change. Transactions replicate on the reliable QUIC stream; per-frame state deltas replicate on the unreliable datagram stream. Both flow through the same transport, the same reflection registry, the same rules.

**Who it's for.** Small teams iterating on a world together — one developer tuning a terrain heightmap while another places lights and a third adjusts a character, all on a single running server. The dedicated `stratumv_server` binary hosts the session; every developer is a client. Single-player = one local server + one client, same code path.

**What it looks like.** The in-game `AdminPanel` gains editor-scope tabs when the connected client holds the scope. Scene edits (move entity, place asset, tune material) are recorded in a per-session undo log on the server. Changed asset files stream over QUIC reliable streams and are cached content-addressably; thumbnails baked by one client are replicated to the others without re-bake work. Nothing about the workflow requires a separate editor process.

**Precedent.** Unreal's Multi-User Editing is the closest prior art and proves the authoritative-transaction model works at production scale. Resonite is the ideological precedent for treating the data model as the replication model. StratumV adapts both to a game-engine use case at small-team scale.

**Scope boundaries.** Collaborative editing covers scene state, asset pushes, material parameters, and world tuning. It does not cover source code (developers write code in their own IDE and rebuild via DLL hot-reload), visual scripting (not supported), long-term history (source control handles that), or chat/voice (out of band). See `docs/COLLAB_EDITING.md` for the full treatment.

---

## What StratumV Is Not

- Not a drop-in replacement for Unreal or Unity — it is a smaller, source-available engine you compile into your own game.
- Not a service. It compiles directly into the game binary; there are no runtime fees.
- Not a physics engine. Physics (Jolt) is optional, enabled per-game.
- Not a separate editor application. The editor is a permission scope on the runtime.
- Not tied to a cloud provider. Self-hosted dedicated server, MsQuic transport.
