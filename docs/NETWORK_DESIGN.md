# NETWORK_DESIGN — Networking Rationale

> Decision-layer companion to `ARCHITECTURE.md`. This doc explains **why** the
> StratumV networking stack is shaped the way it is. For the what — module
> layering, transport API, replication registry surface — see
> `ARCHITECTURE.md`, `REPLICATION_CONTRACT.md`, and the headers under
> `src/engine/`. For collaborative editing semantics on top of the transport,
> see `COLLAB_EDITING.md`.

> **Status** — headless `stratumv_server` exe + `NetworkContext` sub-struct
> refactor shipped. `BaseSystemContext.network` is now a `NetworkContext`
> sub-struct (not a flat `INetworkContext*`) — consumers access the interface
> via `ctx.network.context->...`. Semver bumped 1.2.1 → 1.3.0 with a migration
> table in `CHANGELOG.md §1.3.0`. `src/stratumv_server/main.cpp` wires the
> Transport+Listener into a minimal dedicated-server binary (--port,
> --idle-timeout-ms, SIGINT shutdown). `tests/test_NetworkContext.cpp` adds 6
> Catch2 cases covering the sub-struct shape, the NoOp wiring, the perf.network
> split, and a server process-equivalent smoke test. Decision noted in §7
> below: `PerformanceContext::network` (NetworkStats) stays under `perf`, does
> NOT migrate into the new `NetworkContext` sub-struct.
>
> **Status** — MsQuic transport integration shipped.
> `src/engine/net/MsQuicTransport.h` + `MsQuicTransport.cpp` provide the
> `sv::net::Transport` / `Listener` / `Connection` RAII types. MsQuic 2.5.6 is
> consumed as the `Microsoft.Native.Quic.MsQuic.Schannel` NuGet package via
> `FetchContent_Declare(URL ...)` — no source build, no OpenSSL, no Perl.
> Loopback TLS 1.3 QUIC handshake is proven by `tests/test_MsQuicTransport.cpp`
> (8 cases, 50 assertions). Server side uses a one-shot self-signed cert
> generated via CNG (`NCryptCreatePersistedKey` + `CertCreateSelfSignCertificate`
> with a process-unique key container that Transport::stop deletes on
> shutdown). Client side uses `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`
> to accept it.

StratumV is a multiplayer-first engine. The original `INetworkContext` stub
was a placeholder — a no-op slot on `BaseSystemContext` to prove the DLL
boundary could carry a networking interface without committing to an
implementation. The real networking stack is specified here.

Every decision in this document traces back to three hard constraints:

1. **The target games are multiplayer.** One reference workload is a 64-player
   authoritative survival game; another is a PvPvE extraction shooter, and
   neither is a
   single-player game that will bolt on networking later.
2. **Live collaborative dev editing is a first-class engine feature.** Two
   developers joining the same running world, editing terrain or entities or
   materials together, is on the same transport and data model as players
   playing together. See `COLLAB_EDITING.md` for the full treatment.
3. **Zero runtime cost, zero runtime dependencies.** No Fly.io, no AWS, no
   Cloudflare R2, no GameLift, no per-player fees, no royalties. Permissive
   licenses only (MIT / BSD / Apache 2 / ISC / Zlib / public domain). GPL and
   AGPL are out.

If any decision below reads like "we picked X because we're building a
single-player engine that will eventually support multiplayer," assume
the doc is wrong and fix it.

---

## 1. The Architectural Inversion

### 1.1 Why networking cannot be a late add-on

An earlier plan treated networking as a late add-on: "extract the render
core, get two games running single-player, then design networking on top."
That ordering is wrong for three reasons.

**First, it duplicates the data model.** An engine built single-player
stores gameplay state in whichever container is convenient — raw structs,
EnTT components, std::vector fields, std::unordered_map caches. None of
that has wire-format metadata. When networking arrives, every component
that needs to replicate has to be refactored to separate the wire-safe
fields from the cache/scratch fields, and every system that mutates those
components has to be audited for authority. That's not a feature, that's
a rewrite.

**Second, it duplicates the lifecycle.** A single-player engine assumes one
authoritative source of truth: the running process. Entity spawn, destroy,
mutate, save, load all run on the one client. A networked engine has to
distinguish "server owns this entity," "client predicts this entity,"
"editor just mutated this entity via a transaction," "this entity came from
a late-join snapshot." Adding those distinctions after the fact means every
`registry.emplace` call in the game needs an audit.

**Third, and most importantly for StratumV: it blocks live collaborative
editing from ever existing.** Collaborative editing requires the same
transport, the same authoritative data model, and the same authority rules
as multiplayer gameplay. If networking lands as a late add-on, live collab
becomes a wish item that never ships, the same way it never shipped
in Unity or Godot despite being technically possible in both. The only
engines that have shipped working live collab (Resonite, Unreal Multi-User
Editing) built the reflection layer first and grew everything else on top.

### 1.2 Resonite's insight

Resonite's architectural principle, quoted from
<https://wiki.resonite.com/Architecture_Overview>:

> programming primitives such as variables, references, arrays, and
> dictionaries that are designed to be automatically replicated over
> network or persisted.

Translated: the data model **is** the replication model. Not a layer on
top. The replication system isn't a feature the engine has; it's the
substrate the engine is built on. Everything — object spawning,
persistence, undo, network sync, hot reload, collaborative editing — is
an application of the same primitive.

StratumV takes the same stance, scoped down to a game engine rather than a
general-purpose VR metaverse platform. The reflection registry + replication
authority model is the primitive. Networking, collab editing, save games,
and DevServer live tuning are all consumers of it.

### 1.3 The order of operations

This is the order the networking stack is built in, and it is not negotiable:

1. **Reflection registry first.** No networking, no wire format, no
   threads. Just `SV_REPLICATE(field1, field2, ...)` macros on selected
   components plus a `ReplicationRegistry` module that records offsets,
   types, and dirty bits. Tests are pure math.
2. **Authority + snapshot/delta.** Still no wire. Pure serialization
   math with `Authority::Server/Owner/Editor/None` on each replicated
   component. Delta encoding with bit packing against the last acked state.
   Tests encode → decode → round-trip.
3. **Transport.** MsQuic integration. Raw connection + handshake.
   Still no game state flowing.
4. **Dedicated server binary + context sub-struct refactor.**
   Headless `stratumv_server` CMake target. `BaseSystemContext.network`
   promoted from flat `INetworkContext*` to nested `NetworkContext`
   sub-struct. Semver bump to 1.3.0.
5. **Interest management + interpolation.** Area-of-interest,
   fixed-tick snapshot generation, client-side interpolation buffer.
6. **Scale-target-B work.** Spatial hashing, Jolt physics islands,
   per-client bandwidth budgets.
7. **First real multiplayer.** Two `skinned_test` instances, one
   moves a cube, the other sees it with interpolation.
8. **Permission scopes + edit transactions.**
9. **Join-with-snapshot.**
10. **Asset sync + thumbnail replication.**

Steps 1–2 are pure logic with unit tests. Step 3 is infrastructure. Step 4
is a refactor. Only at step 7 does a pixel move across a wire. That delay
is intentional: getting the primitive right matters more than getting
something on screen fast.

---

## 2. Transport — MsQuic

### 2.0 How MsQuic is consumed

MsQuic ships as a prebuilt binary in the
**`Microsoft.Native.Quic.MsQuic.Schannel`** NuGet package (`.nupkg` file, which
is really just a ZIP). StratumV pulls it in via `FetchContent_Declare(URL ...)`
rather than building from source:

```cmake
FetchContent_Declare(msquic
    URL https://www.nuget.org/api/v2/package/Microsoft.Native.Quic.MsQuic.Schannel/2.5.6
    URL_HASH SHA256=07c812dc258a9fd386c767398ceabcf78ae892535e8ea0f71d7f0d8bf4a73dfe
    DOWNLOAD_NAME msquic-2.5.6.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
```

The extracted tree contains:

| Artifact | Path within extracted nupkg |
|----------|-----------------------------|
| Runtime DLL | `build/native/bin/x64/msquic.dll` (~536 KB) |
| Import lib | `build/native/lib/x64/msquic.lib` (~2 KB) |
| C API header | `build/native/include/msquic.h` (~81 KB) |
| C++ wrapper | `build/native/include/msquic.hpp` (~63 KB, not used — C API direct) |
| Debug symbols | `build/native/bin/x64/msquic.pdb` (~3.6 MB) |

CMake creates a `SHARED IMPORTED` target for msquic with `IMPORTED_LOCATION`
pointing at the DLL and `IMPORTED_IMPLIB` at the lib. The helper
`stratumv_copy_msquic_dll(target)` adds a POST_BUILD step that copies the DLL
next to any exe that links stratumv. sv_tests, sv_golden_tests, and
skinned_test all use it.

**Why NuGet, not the MsQuic source build:** MsQuic's own CMakeLists.txt
requires either the Schannel backend (Windows Server 2022 / Windows 11 24H2+)
or the OpenSSL backend (which pulls in Strawberry Perl and a multi-minute
OpenSSL compile). The NuGet Schannel package skips both — the binary is
already signed against Schannel and just works on any recent Windows. .NET
Core (required for MsQuic's logging target) is also not needed, since we are
not building the logging support.

**Upgrading MsQuic:** bump the FetchContent URL version, download the new
.nupkg, compute SHA256, update both in `CMakeLists.txt`, and bump the pinned
version string in `Transport::msquicVersionString` and the
test_MsQuicTransport `"2.5.6"` assertion.

### 2.1 What MsQuic is

MsQuic (<https://github.com/microsoft/msquic>) is Microsoft's production
QUIC implementation. MIT licensed. C API. Used by the Xbox Game Development
Kit for multiplayer title networking
(<https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/networking/game-mesh/msquic-intro-networking>).
Ships on Windows, Linux, macOS. One API surface for:

- Reliable ordered streams (for handshake, chunked asset uploads, edit
  transaction log, join-with-snapshot).
- Unreliable datagrams (for high-frequency state updates).
- TLS 1.3 encryption (not optional).
- Connection migration (a player switching Wi-Fi → cellular keeps the
  session).
- Multiplexed streams per connection (no head-of-line blocking between
  independent edit transactions).
- Per-connection flow control.

### 2.2 Alternatives considered

| Candidate | Rejected because |
|-----------|------------------|
| **ENet** | 2005-era UDP reliability. No encryption, no stream multiplexing, no connection migration. Would need a crypto layer (libsodium? Noise?) and a framing layer bolted on top. Adding MsQuic-equivalent features to ENet would be reinventing MsQuic. |
| **yojimbo** | libsodium dependency, narrow focus on fast-action FPS, limited maintenance momentum, no stream abstraction. |
| **GameNetworkingSockets (Valve)** | Steam-shaped API, Steam toolchain assumptions, Isetta team walked away from it citing documentation and build friction. |
| **enet-plus / enet6** | Same fundamental ENet design plus IPv6. Doesn't solve the missing-encryption or missing-stream problems. |
| **Hand-rolled UDP** | Zero leverage. A hand-rolled reliable-UDP layer with encryption, flow control, and multiplexing is a multi-year project. That's a library, not a subsystem inside a game engine. |
| **KCP** | Reliable UDP, but no encryption and no streams. Same problem as ENet. |
| **WebRTC data channels** | Built for browser P2P. Requires SFU or TURN for NAT traversal. Overkill for dedicated-server model. |

MsQuic is the only option on the table that gives us all the features we
actually need in one dependency, from a vendor that will still exist in
five years, with a permissive license.

### 2.3 WebTransport for browser clients

Browser-based tools (spectator views, admin dashboards, asset web UIs, the
"see the running game from any device" dev tool) can't speak MsQuic
directly — browsers don't expose raw UDP or QUIC sockets. The WebTransport
API does, and it runs over HTTP/3 which is QUIC under the hood. The
dedicated server will expose a WebTransport listener on the same MsQuic
stack via a thin adapter. This lets a browser-based observer join a running
session without pulling in a second transport library.

This is additive, not alternative — the primary transport for game clients
and editor clients is still raw MsQuic. WebTransport is an optional adapter
on the server side for web-only consumers.

---

## 3. Authoritative Server

### 3.1 The model

StratumV uses an authoritative server model: exactly one process is the
source of truth for gameplay state, and all other processes (player
clients, editor clients, spectator clients) are consumers that send
intents and receive snapshots.

This is the Source Engine / Quake / Overwatch / Unreal / every major FPS
model. It's also the Unreal Multi-User Editing model for live collab
(<https://dev.epicgames.com/documentation/en-us/unreal-engine/multi-user-editing-overview-for-unreal-engine>),
which is the closest prior art to what StratumV wants for dev collaboration.

### 3.2 Why not CRDTs

Automerge-rs (<https://github.com/automerge/automerge>) would let us treat
collaborative editing as a convergent merge problem — every client has a
local replica, edits are commutative, and the data structure guarantees
convergence without a central authority. Figma uses this model. Resonite
uses a CRDT-ish data model for world objects, though with server
authoritative semantics layered on top for game rules.

We explicitly rejected CRDTs for v1 for four reasons:

1. **Authoritative already solves the collab case.** Unreal Multi-User
   Editing proves a transaction-log-on-server model handles multi-developer
   live editing at real production scale. No CRDTs needed.
2. **Authoritative is required for gameplay anyway.** A 64-player PvPvE
   shooter needs an authoritative server to prevent cheating. The collab
   editing stack would sit next to an authoritative stack instead of
   reusing it — more code, not less.
3. **Toolchain cost.** Automerge-rs is Rust. Embedding it means adding a
   Rust toolchain to a C++ build, writing C bindings, and managing cross-
   compilation. Non-trivial.
4. **Conceptual cost.** CRDTs are a different mental model than
   "authoritative + delta sync." Mixing them in one engine means every
   future engineer has to learn both models. Picking one is cheaper.

CRDTs are revisitable later — specifically if a game needs offline
collaborative editing where two devs edit the same world without either of
them running a server, then merge on reconnect. No current RoaringBytes
game needs that. If one ever does, the reflection registry is the same
primitive a CRDT layer would need; the authority rules are the only thing
that would have to change.

### 3.3 Determinism policy

Not all systems need to be deterministic. The ones that do are:

- **Jolt physics in zones that need server authoritative collision.** Jolt
  provides deterministic simulation under fixed timestep (1/60s), same input
  sequence, same initial state. A game may run Jolt at a fixed
  1/60s timestep; another follows when it wires physics.
- **Animation state machines for hit-registration-relevant bones.** The
  animation stack (`AnimationSystem.cpp`, ozz 0.16.0) samples at fixed
  timesteps with no floating-point accumulation across frames. Consistent.

The ones that don't need to be deterministic are:

- **Particle systems.** Purely cosmetic. Client rolls its own seed.
- **Post-process effects.** Client-only.
- **UI animations.** Client-only.
- **Background ambient audio.** Client-only.

Deterministic physics + deterministic animation gives us enough to run
authoritative hit registration on the server without replaying every
client's visual effects. That's the right scope trade.

### 3.4 Fixed server tick

Server tick rate target: **30 Hz**. Jolt physics step: **60 Hz** (two
physics sub-steps per network tick).

30 Hz is the Overwatch / Apex Legends / Counter-Strike: Global Offensive
default. 60 Hz is possible and is nice for competitive FPS, but it doubles
bandwidth and doubles server CPU per session. For the scale target we're
aiming at (256-500 per shard), 30 Hz is the right ceiling. Individual
games can opt into 60 Hz later by setting a config field; the engine
supports both.

Fixed tick is non-negotiable. Variable tick rate destroys determinism and
makes delta encoding undiagnosable. If a server is running hot and can't
hit 30 Hz, the answer is spatial hashing and interest management, not
dropping to 20 Hz.

---

## 4. Scale Target

### 4.1 Targets A / B / C

| Target | Players per shard | Status |
|--------|-------------------|--------|
| **A** | 64–128 | Baseline reference target. Well within tested QUIC/UDP throughput. |
| **B** | 256–500 | **Target for StratumV.** Same fundamental architecture as A, with spatial hashing, physics islands, per-client bandwidth budgeting, compressed transform format. |
| **C** | 1000+ | Explicitly **deferred, probably never**. Would require CRDT layer or Star Citizen-style server meshing. |

### 4.2 Why B and not C

Star Citizen has been working on 1000-player shards since roughly 2015. As
of early 2026 they are at 700-800 players after a decade of server meshing
work (<https://soren.com/en/news/star-citizen/2026-01-02-server-meshing-success-defines-star-citizens-year>).
That's with a studio that has hundreds of engineers and hundreds of
millions of dollars. It is not a scope RoaringBytes should be aiming at.

The honest question is not "how many players per shard" — it's "how many
concurrent players in the game world total," and that's a function of
shards × players-per-shard. If shard spin-up is cheap (and with a
self-hosted CMake target binary it is), then 5 shards × 300 = 1500
concurrent players. Past a certain point you cap per-shard and solve the
rest with multi-shard.

### 4.3 Why B and not A

Scale target A (64-128) is the baseline reference target. That
would be the easy path. We target B anyway for two reasons:

1. **A game might grow.** Treating 64 as a hard ceiling bakes the
   assumption into the data structures (fixed-size arrays, coarse
   interest-management grids, etc.). Designing for 256-500 means the
   engine can host 64 without wasting capacity and can host 256 if a
   future game wants to.
2. **Spatial partitioning is free to add early and expensive to add
   late.** Jolt physics islands, BVH-based interest management, per-client
   bandwidth budget queues — these are substantially harder to retrofit
   into an already-shipping game than to design in from the start.

### 4.4 How B fits in the same code as A

Scale target B is not a different architecture from A. It's the same
architecture with bigger numbers and better data structures:

- **Interest management**: tile grid (A) → spatial hash / BVH (B).
- **Physics**: single world (A) → Jolt physics islands per spatial
  partition (B).
- **Bandwidth**: flat 64 KB/s per client (A) → priority-queue-driven
  per-client budget (B).
- **Transform format**: full float3+quat (A) → quantized delta (B).

An A-scale game doesn't pay for any of the B-specific machinery at
runtime; it just runs with the defaults. A B-scale game turns on spatial
hashing + physics islands in config and the same code paths handle it.

---

## 5. Dedicated Server

### 5.1 Separate binary

The dedicated server is a separate CMake target, `stratumv_server`. It is
not a runtime flag on the client. Reasons:

1. **No swapchain.** The server has no window, no swap images, no ImGui
   render pass, no DLSS. Compiling those in and then `if (!headless)`-ing
   them out is worse than just not compiling them.
2. **Dedicated server has different dependencies.** No GLFW, no Vulkan
   surface extensions, no font files, no shader compiler at runtime. The
   server binary should be small (ideally under 50 MB stripped) and
   startable without GPU drivers.
3. **Testability.** Server can be spun up in CI without a graphics stack.
4. **Deployability.** A self-hosted deployment might put the dedicated
   server on a Linux box (cheap), and the clients on Windows/macOS
   (where players live). Separate binaries keep that simple.

The shared code is everything in `src/engine/` that doesn't touch Vulkan or
ImGui: ECS, animation runtime, physics, networking, config, engine log,
world state, asset manifest, replication registry, scene loader, devserver.
The client-only code is `vk/`, `graph/`, `passes/`, `ui/`, parts of
`EngineBase`, and the main loop's rendering phase.

### 5.2 Single-player is one local server plus one client

Single-player games built on StratumV run as one local `stratumv_server`
process plus one client process, connected via loopback. Same code path
as multiplayer. Same transport. Same replication. Same authority.

This is the same model id Tech / Source / Unreal use. It has three big
wins:

1. **No second code path.** Single-player bugs surface in multiplayer. You
   don't ship and discover that the "single-player mode" skipped code
   that matters.
2. **Save games are snapshots.** A save game is the authoritative server's
   world state at a moment. The same format that handles join-with-
   snapshot handles save files.
3. **Dev workflow parity.** A dev working on single-player logic hits the
   same authority rules as a dev working on multiplayer. No mental
   context switch.

The cost is a second process at runtime, which for single-player
deployment is cheap: ~50 MB RAM for the server, loopback is free, and the
player doesn't care about the process count.

### 5.3 Self-hosted only — no cloud provider coupling

No Fly.io, no AWS GameLift, no Cloudflare R2, no Azure PlayFab, no Epic
Online Services. The dedicated server is a binary a user can run on
their own hardware. Asset storage is content-addressable hashes on local
disk. Player identity starts as a username claim without central auth
and can grow into a federated identity layer later.

Rationale:

1. **No recurring cost for RoaringBytes.** A cloud provider becomes a tax
   on every player forever.
2. **No recurring cost for players.** Servers can be self-hosted by
   communities the way Counter-Strike / Garry's Mod / Minecraft servers
   are.
3. **No platform risk.** If a provider changes pricing, changes terms, or
   goes away, the game still works.
4. **Clean separation.** Authentication, matchmaking, and server hosting
   are independent problems. Forcing them all through a single vendor
   makes every one of them harder to change later.

A future hosted-matchmaking service is possible later if RoaringBytes wants
to run official servers for a game. It would be an optional overlay, not a
dependency.

---

## 6. Physics + Networking

### 6.1 Jolt stays

Jolt 5.2.0 is already integrated via `IPhysicsContext` +
`JoltPhysicsContext`. It's the physics engine for the foreseeable future.
Networking is built on top of Jolt, not instead of it.

### 6.2 Fixed timestep is required

Jolt's determinism guarantees require fixed timestep with stable input
order. The server runs Jolt at 60 Hz with deterministic integration. The
server tick (30 Hz, see section 3.4) runs two Jolt sub-steps per tick.
Clients do not run authoritative Jolt — they either interpolate server-
authoritative transforms or, for player-owned bodies, run a predicted
Jolt simulation that is reconciled against server state.

Variable timestep breaks the whole model. Anyone tempted to tune the Jolt
timestep to match frame rate needs to read this section first and then
change their mind.

### 6.3 Physics islands as spatial partitions

Jolt has a built-in island system — it partitions the simulation into
disconnected groups of bodies that can be stepped independently. At scale
B (256-500 players), single-island simulation is expensive because every
character ends up sharing an island with at least one other character
most of the time.

The scale-target-B work splits the physics world into spatial-hash-aligned
partitions, each of which owns its own Jolt PhysicsSystem at its own
fixed timestep. Entities near a partition boundary hand off cleanly.
Interest management shares the same partitioning, so the networking
interest grid and the physics simulation grid are the same grid.

This is how scale B happens without the physics step pegging the server
CPU at 100%.

---

## 7. Server Observability

Networking bugs are invisible without metrics. The observability block
ships as `PerformanceContext::network` (a `NetworkStats` POD nested inside
`ctx.perf`), populated with zero-valued placeholders so the AdminPanel HUD
and DLL plugin surface are wired before the first replication byte flies.
Minimum set:

| Field | Unit | Purpose |
|-------|------|---------|
| `tickMs` | ms | Server tick budget. If this goes over 33.3 ms, server is falling behind. |
| `bytesPerSec` | bytes/sec | Per-client outbound bandwidth. |
| `packetsPerSec` | pkts/sec | Per-client outbound packet rate. |
| `replicatedEntityCount` | count | How many entities a given client is currently tracking. |
| `ackLatencyMs` | ms | Round-trip time as measured by the replication ack stream. |
| `droppedDatagramPct` | % | Fraction of unreliable datagrams that did not arrive (QUIC reports this). |

Access path: `ctx.perf.network.bytesPerSec`, `ctx.perf.network.tickMs`, etc.
The AdminPanel HUD reads all of `PerformanceContext` as a single slice
per frame and renders frame-time / draws / VRAM / network bars side
by side from that one struct.

### 7.1 Why NetworkStats stays under `perf`, not under `network`

The `NetworkContext` sub-struct on `BaseSystemContext` is the home for
**active networking services** — the `INetworkContext* context` slot that
replaced the flat 1.2.x `ctx.network` pointer, plus the future slots
(Transport handle, permission-scope accessor, snapshot apply hook, asset
sync channel). An obvious question arises: should
`PerformanceContext::network` (the `NetworkStats` observability block)
also migrate into `NetworkContext`, so all networking-related fields
live under `ctx.network.*`?

**Decision: NetworkStats stays under `perf`.** Reasoning:

1. **AdminPanel HUD locality.** `src/engine/ui/AdminPanel.cpp` reads
   `PerformanceContext` as a single struct-copy when drawing the Performance
   tab, and renders the frame-time / draws / VRAM / network bars side
   by side from that one slice. Splitting the network counters out
   would force the HUD to chase a second pointer into `ctx.network` on
   every frame for a rendering loop that already exists. Zero-cost
   refactors that add an extra indirection to the hot path are
   anti-patterns; this one would save nothing.

2. **Semantic split.** `ctx.network.*` holds *active services* — the
   interface you call into to do networking. `ctx.perf.network.*`
   holds *passive runtime counters* — the numbers you read out to see
   what the active services are doing. These are different things that
   happen to share a name. Services go next to other services (under
   the sub-struct named after their concern); metrics go next to other
   metrics (under `perf`). Collapsing them would mix two unrelated
   access patterns under one roof.

3. **AdminPanel HUD churn is measurable.** `lab/skinned_test/main.cpp`,
   `test_PerformanceContext.cpp`, and `test_MockContext.cpp` all read
   `pc.network.*` via `PerformanceContext`. Migrating would touch
   every one of them. The CHANGELOG migration table in 1.3.0 already
   covers the flat-to-sub-struct rename for the interface pointer;
   piling a second rename on top would raise the consumer cost of
   1.3.0 without a proportionate benefit.

4. **DLL plugins read both halves anyway.** A DLL plugin that cares
   about networking already reaches into two different sub-structs in
   the nested layout (`ctx.network.context->send(...)` for the service,
   `ctx.perf.network.bytesPerSec` for the stat). That's fine — the
   types are unrelated, the access patterns are unrelated, and mixing
   them under a single name would just hide the distinction.

The split rule going forward:

- **Active services** (transport handles, interfaces, hooks) live under
  `ctx.network.*` — one sub-struct named after the concern.
- **Passive runtime metrics** (bytes/sec, RTT, dropped datagrams) live
  under `ctx.perf.network.*` — one sub-struct named after their role
  in the observability HUD, grouped with frame-time / draws / VRAM.

Later work MAY add network fields to `PerformanceContext::network`
(e.g. per-client RTT histograms) without moving them, and MAY add
service slots to `NetworkContext` (e.g. `transport`, `permissionScope`)
without touching `perf`. The two sub-structs evolve independently.

The observability block was a deliberate prerequisite of the transport
work precisely because sync bugs are undiagnosable without these metrics.

The alternative — debugging a 256-player desync by reading logs — has been
tried elsewhere and doesn't work.

---

## 8. What Networking Does Not Include

Scope discipline. The networking stack covers:

- Transport.
- Reflection registry + replication authority.
- Snapshot + delta encoding.
- Interest management + fixed-tick sync.
- Join-with-snapshot lifecycle.
- Edit transactions for collaborative editing.
- Asset sync for editor clients.

It does **not** cover:

- **Matchmaking.** That's a separate service or a community server list.
  Games that need it build it on top. The engine exposes connect-by-IP
  and connect-by-hostname; that's enough.
- **Voice chat.** Out of scope. Players can use Discord. If a game
  actually needs in-engine voice, that's future work.
- **Text chat.** Out of scope. Games that want it build a chat DLL
  plugin.
- **Account systems.** Out of scope. Players authenticate to the server
  with a username claim. Federated identity is deferred.
- **Anti-cheat.** The authoritative server model makes most cheats
  ineffective by construction (client can't declare "I have 9999
  health"). Movement and aim validation live in per-game DLL plugins.
  Anti-tamper and client integrity are separate concerns that don't
  belong in the engine.
- **Dedicated server hosting / deployment automation.** Out of scope.
  RoaringBytes builds the binary; users run it.
- **P2P / NAT traversal.** Rejected in favor of dedicated server model.
  If two players want to play, one of them runs a server, or they join
  a community server.
- **WebSocket fallback.** Rejected. WebTransport over HTTP/3 covers the
  browser case; hand-rolling a WebSocket fallback adds a second transport
  path we'd have to maintain.

Every item on the "not included" list is there for a reason — either it's
genuinely not an engine concern, or it's a large feature that would derail
the core networking stack.

---

## 9. Related Docs

- **`REPLICATION_CONTRACT.md`** — The SV_REPLICATE macro, Authority enum,
  dirty bits, quantization hints, per-component opt-in discipline.
- **`COLLAB_EDITING.md`** — Permission scopes, edit transactions, undo
  log, asset sync, Unreal MUE comparison.
- **`ARCHITECTURE.md`** — Layer model. Networking is a Layer 4 service
  module (`ReplicationRegistry`) plus a Layer 2-adjacent platform module
  (`NetworkTransport` / MsQuic binding).
- **`PLUGIN_CONTRACT.md`** — The DLL boundary rules that the replication
  layer has to respect. Networking fields live in a nested
  `NetworkContext` sub-struct on `BaseSystemContext`.
- **`CHANGELOG.md`** — StratumV 1.3.0 ships with a migration table from
  `ctx.network->*` to `ctx.network.*`.

---

## 10. Research Sources

- Resonite architecture overview:
  <https://wiki.resonite.com/Architecture_Overview>
- Resonite networking overview:
  <https://wiki.resonite.com/Networking_information>
- Resonite data model synchronization:
  <https://wiki.resonite.com/index.php?title=Data_model_synchronization>
- Unreal Multi-User Editing (UE 5.7):
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/multi-user-editing-overview-for-unreal-engine>
- MsQuic on the Microsoft Game Development Kit:
  <https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/networking/game-mesh/msquic-intro-networking>
- "Interest Management in Massively Multiplayer Online Games" — Jean-
  Sébastien Boulanger thesis, McGill University:
  <https://www.cs.mcgill.ca/~jboula2/thesis.pdf>
- "QUIC for Game Networking" — Da Posto, Medium:
  <https://daposto.medium.com/quic-for-gamenetworking-46cf23936228>
- Star Citizen server meshing retrospective (cited as scale ceiling, not
  target): <https://soren.com/en/news/star-citizen/2026-01-02-server-meshing-success-defines-star-citizens-year>
