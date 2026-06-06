# COLLAB_EDITING — Live Collaborative Development

> Decision-layer companion to `ARCHITECTURE.md`, `NETWORK_DESIGN.md`, and
> `REPLICATION_CONTRACT.md`. This doc explains **why** StratumV treats
> live collaborative development as an engine primitive, and how the
> permission scopes, edit transactions, undo log, and asset sync slot
> onto the authoritative replication substrate. For the transport see
> `NETWORK_DESIGN.md`; for the per-field replication contract see
> `REPLICATION_CONTRACT.md`.

Live collaborative development is the feature that made the networking
reframe necessary. Two developers joining a running game world, editing
terrain, characters, materials, and scripts together, and seeing each
other's changes in real time — that is the wedge that differentiates
StratumV from Unity, Unreal, Godot, and every other engine a small team
could pick up in 2026. Everything in this document is in service of
making that feature boring to build because the substrate is right.

The canonical prior art is Unreal Multi-User Editing. The ideological
prior art is Resonite. This doc leans on both, with the specific scoping
down to "a game engine for two multiplayer games made by a small studio"
rather than "a general-purpose collaborative metaverse platform."

---

## 1. The Core Insight

### 1.1 Collab editing is not a feature — it's a permission scope

The conventional mental model says: players play games; developers edit
games in an editor; editors and runtimes are separate programs with
separate code paths. Every major engine works this way. Unity has Unity
Editor and the player runtime. Unreal has UnrealEditor.exe and
UnrealGame.exe. Godot has the editor mode and the export template. They
all draw a line between "the thing you make" and "the thing you run,"
and they all lose the ability to make the same thing collaboratively in
real time.

The StratumV mental model says: players and developers are both
clients. What separates them is which permission scopes they hold.
A player client holds `Player`. A dev client holds `Editor`. A spectator
holds `Spectator`. An admin holds `Admin`. Everything flows from that
distinction, and it flows through the same replication substrate
(`REPLICATION_CONTRACT.md`) and the same transport (`NETWORK_DESIGN.md`).

This reframe is what makes live collab affordable. If collab editing
required a separate editor process with its own replication stack, it
would never ship. Because it's a permission scope on the existing
replication stack, it's a tractable amount of work.

### 1.2 The two precedents

**Unreal Multi-User Editing** (UE 5.7, dev.epicgames.com docs). Multiple
developers connect to a shared editor session hosted by a
`UnrealMultiUserServer` process. Edits are transactional — a single edit
operation is a unit that can be played, undone, logged, and replicated.
The server is the authority; clients apply edits and see other clients'
edits with millisecond latency. This is the closest prior art to what
StratumV wants, and it is the one most of the architecture is lifted
from.

What MUE does that StratumV will do:

- Separate server binary hosts the collaborative session.
- Edits are discrete transactions, not continuous streams.
- Transactions are replicated to all connected clients.
- Undo log is a transaction replay (or reverse-replay).
- Permission scopes gate what a client is allowed to mutate.
- Join mid-session via a full state snapshot followed by delta sync.

What MUE does that StratumV will **not** do:

- Host a full Unreal Editor UI. StratumV's "editor" is the existing
  in-game AdminPanel with editor-scope buttons unlocked. No dedicated
  editor application.
- Provide source-control integration hooks in the editor. Source control
  happens outside the running session in the usual way.
- Run a separate "Multi-User Server" brand. StratumV's dedicated server
  hosts both gameplay and editing in the same process, distinguished by
  permission scope.

**Resonite** (wiki.resonite.com). Everyone in a Resonite session can
edit everything in real time — the data model is the replication model
(see `NETWORK_DESIGN.md` section 1.2). Permission is per-user and
per-object, not per-scope. This is more permissive than StratumV will
be, but the underlying insight — that the same replication system
handles gameplay state and editor state — is identical.

What StratumV takes from Resonite:

- The architectural principle that the data model **is** the replication
  model.
- The acceptance that an engine can ship without a separate editor
  binary.
- Proof that real-time multi-user editing of 3D worlds is technically
  possible on modest hardware with modest bandwidth.

What StratumV does not take from Resonite:

- Per-object permission ACLs. StratumV uses coarser scopes. Simpler to
  reason about for gameplay-engine use cases.
- A general-purpose visual scripting layer (ProtoFlux). StratumV games
  write gameplay in C++ DLL plugins.
- A VR-first interaction model. StratumV is a traditional mouse/keyboard
  engine with VR as a deferred concern.

### 1.3 Why we rejected CRDTs for collab

See `NETWORK_DESIGN.md` section 3.2 for the full argument. Summarized:
authoritative server + transaction log handles the collab case at the
scale we care about (2-4 developers editing a world together) without
pulling in a CRDT runtime, and the authoritative model is required
anyway for gameplay cheating prevention. Adding CRDTs on top of an
authoritative server would be two code paths for one problem.

---

## 2. Permission Scopes

### 2.1 The four scopes

```cpp
namespace sv {
enum class PermissionScope : uint8_t {
    Spectator = 0,  // read-only view of the world, no input
    Player    = 1,  // normal gameplay input + owned state
    Editor    = 2,  // scene mutation + asset modification
    Admin     = 3   // Editor + permission management + server ops
};
}
```

Four values, one byte, packed into the handshake. Scopes are
hierarchical — `Admin` ⊇ `Editor` ⊇ `Player` ⊇ `Spectator` — but the
checks compare by exact value, not by ≥, to keep capability checks
explicit.

### 2.2 What each scope allows

**`Spectator`** — read-only. Can connect, receive world state, move a
camera. Cannot move gameplay entities, cannot send input, cannot mutate
anything. Used for streamers, debug observers, coaches, and the
browser-based monitoring tool that will live on top of WebTransport.

**`Player`** — normal gameplay. Can send input intents for owned
entities (`Authority::Owner`). Cannot mutate scene data, cannot edit
assets, cannot place or remove entities. Cannot see spectator-hidden
data (debug overlays, server-side variables, admin panels). This is
what 99% of clients connect with.

**`Editor`** — `Player` plus scene mutation. Can mutate
`Authority::Editor`-marked components (transforms of placed assets,
material parameters, light settings, etc.). Can place entities via
editor transactions. Can load and save scene files via DevServer
commands. Can see the AdminPanel editor tabs. In short: everything
needed to build and tune a world inside a running session.

**`Admin`** — `Editor` plus server-level ops. Can grant / revoke
permission scopes for other clients. Can kick clients. Can shut down
the server. Can clear the undo log. Can force-resync a client. This
is a small number of people per session — typically the session host.

### 2.3 What these scopes do NOT include

- **Per-object ACLs.** No "only client X can edit entity 42." Too much
  bookkeeping, too easy to get wrong, and the gameplay engine use case
  doesn't need it. If two devs edit the same entity simultaneously, the
  transaction log serializes the edits and both devs see the result.
  Last writer wins.
- **Moderation tooling.** No chat mute, no shadow-ban, no report. Not
  because these are bad ideas but because they belong in game logic,
  not the engine.
- **Role-based access control over arbitrary resources.** A scope is a
  coarse privilege level, not a set of ACL capabilities. If a game wants
  richer access control, it lives in a DLL plugin.

### 2.4 Why not just a boolean "isDev"

Earlier draft had `bool isDev`. Two problems:

1. **Spectator is a real use case.** Browser-based observers, streamers,
   and debug clients exist. Giving them `Player` means they can shoot
   other players; giving them `Editor` means they can break the session.
   A third scope is the right level.
2. **Admin is a real use case.** The host needs powers the other devs
   don't have (kicking, permission management). A fourth scope makes
   this clean.

4 scopes, 2 bits of discrimination, fits the problem exactly.

### 2.5 Scope assignment

Scopes are assigned at client connect time, carried in the QUIC handshake
along with a simple username claim. The server decides whether to accept
the requested scope based on game-specific logic (held in a DLL plugin)
or a simple config file on the server. The engine's default behavior is:

1. First client to connect becomes `Admin`.
2. Subsequent clients default to `Player`.
3. `Admin` can promote any client to `Editor` or back down to `Player`.
4. `Spectator` has to be requested explicitly at connect time.

Games override the defaults with their own connection-handler logic.
One game might require a passphrase for `Editor`; another might
restrict `Admin` to local-network connections. Engine doesn't care — it
just enforces whatever scope the handler returns.

---

## 3. Edit Transactions

### 3.1 What a transaction is

An **edit transaction** is a single logical unit of editing work. "Move
entity 42 to position (1, 2, 3)" is a transaction. "Place 17 trees on
the terrain" is a transaction. "Change the color of every street light
in district A to warm yellow" is a transaction. The unit is author-
chosen — the runtime doesn't second-guess.

A transaction has:

- An **id** (monotonic per session).
- An **author** (the client ID that issued it).
- A **kind** (a string tag: `"move_entity"`, `"place_asset"`,
  `"set_material_param"`, etc. — games define their own set).
- A **forward op** (bytes that, applied to the world, perform the edit).
- An **inverse op** (bytes that, applied to the world, undo the edit).
- A **timestamp** (server-assigned at commit time).

Transactions are serialized with the same primitives as the replication
snapshot format (`REPLICATION_CONTRACT.md` section 4.3). The forward and
inverse ops are per-component field patches, so the same encoder that
handles tick deltas handles edit commits.

### 3.2 Commit flow

1. **Client constructs a transaction** locally — "I want to move entity
   42 to (1, 2, 3)." The client captures the current state of the
   affected fields as the inverse op, the desired state as the forward
   op, and sends the transaction to the server on the reliable stream.
2. **Server validates** the requesting client has the required scope
   for the affected components. `Editor` components require `Editor`
   scope, etc.
3. **Server applies the forward op** to its authoritative state.
4. **Server assigns an id + timestamp** and appends to the undo log.
5. **Server broadcasts the transaction** (with the assigned id) to all
   other connected clients on the reliable stream.
6. **Other clients apply the forward op** to their local replica.
7. **The next tick** emits a normal delta that carries the same
   mutation. This is redundant for clients that already applied the
   transaction, but it's cheap (deltas are almost empty) and it keeps
   the replication stream internally consistent.

### 3.3 Why transactions and ticks coexist

Couldn't the transaction just be a tick delta? Technically yes — the
bytes would be identical. But three things separate them in practice:

1. **Ticks are unreliable datagrams; transactions are reliable streams.**
   Losing a single-frame position tick is fine (next tick fixes it).
   Losing a placement transaction is session-breaking (the entity would
   exist on some clients and not others).
2. **Transactions have structure.** They carry an id, an author, and an
   inverse op for undo. Ticks carry none of that — they're raw field
   updates. Preserving the structure is what makes undo and audit
   possible.
3. **Transactions are ACID-ish.** A transaction either commits fully
   or not at all. Ticks are last-write-wins at the field level.

The right mental model is: the tick stream is how the simulation stays
synchronized moment-to-moment, and the transaction log is how the world
is shaped over time. They are different channels for different purposes,
even though they carry similar bytes.

### 3.4 Concurrent edits

Two editors editing the same entity at the same time: the server sees
two transactions within the same tick window. They are assigned
sequential ids, the server applies them in id order, and the second
transaction wins (last-writer-wins at the entity level). Both editors
see the final state because both transactions broadcast to both of
them.

This is simpler than conflict resolution and good enough for the small-
team scale we're targeting. If a game needs smarter conflict handling
(e.g. three-way merge for terrain heightmap edits), it can register a
per-kind merge function in the DLL plugin. Default is last-writer-wins.

### 3.5 Transaction batching

A single user action can produce many low-level edits. Dragging a slider
that controls a light's intensity emits dozens of transactions per
second if each drag frame becomes its own transaction. That's bandwidth
waste and it bloats the undo log.

The engine provides transaction batching:

```cpp
sv::EditBatch batch(ctx.network.transactionLog, "adjust_intensity");
// ... many mutations ...
batch.commit();
```

While the batch is open, transactions are coalesced client-side — the
most recent value wins. On `commit`, a single transaction is sent
representing the cumulative change from start to end. Cancel (don't
commit, just let the batch go out of scope) discards the pending
changes.

Games use batches for continuous-input edits (sliders, drag handles,
brush strokes). Discrete edits (single button clicks, placement, delete)
skip the batch and commit immediately.

---

## 4. Undo Log

### 4.1 Structure

The undo log is a ring buffer of transactions held by the server. Each
entry is a transaction record (id, author, kind, forward op, inverse op,
timestamp). Buffer size is configurable per game; default is 256
entries.

The log is per-session, not persisted across restarts. Saving and
loading a scene is a separate feature (see section 6) — the undo log is
a live-session construct.

### 4.2 Undo

When a client issues an `undo` command, the server:

1. Walks back from the tail of the undo log to find the most recent
   transaction authored by that client. (Default: per-author undo.
   Per-session undo is an option games can opt into.)
2. Applies the transaction's inverse op.
3. Moves the transaction to a redo stack.
4. Broadcasts a new transaction carrying the inverse op's effect to all
   other clients. Yes, this means undo is itself a transaction — with
   a `kind: "undo"` tag and the inverse of the inverse as its own
   inverse (so redo works).

### 4.3 Redo

Redo pops the redo stack, applies the transaction again, and pushes it
back onto the undo log. If any new transaction was committed since the
undo, the redo stack is cleared (standard word-processor behavior).

### 4.4 Why per-author instead of global

Global undo is the right model for a single-user editor. In a multi-
user session, global undo means "whoever hits Ctrl+Z last wins," which
is confusing (you might undo someone else's edit you didn't know about).
Per-author undo scopes the stack to each editor's own actions, which is
the mental model most users already have.

Games can opt into global undo if they want (e.g. a solo-developer mode
where per-author is pointless). The engine provides the mechanism; the
game picks the policy.

### 4.5 Undo vs destructive operations

Some edits cannot be cleanly inverted:

- Deleting an asset file from disk.
- Permanent resource reservation (e.g. allocating a large GPU buffer).
- External-system side effects (sending an email, posting to chat).

These are out of scope for the transaction system. The engine provides
undo for gameplay state and scene data only. Operations that touch
things the engine can't unwind are not transactions — they're direct
commands that the author is responsible for.

A useful rule: if an operation can't be expressed as a field delta on a
replicated component, it's not a transaction candidate.

---

## 5. Asset Sync

### 5.1 The problem

When a developer changes an asset file (a mesh, a texture, a shader, a
scene file) in their local working copy, other developers in the same
session need to see the change. The naive answer is "everyone pulls
from source control and restarts the session" — which is the workflow
every other engine has, and which makes live collab not-actually-live.

The engine can do better. Asset changes flow through the same transport
as replication, with a different shape.

### 5.2 Content-addressable storage

Every asset the engine knows about is identified by its content hash, a
fixed-width BLAKE3 digest of the file bytes. The `AssetBrowser` module
already walks the asset tree and tracks last-modified times; the asset
sync layer extends it to compute and cache the content hash. The cache
lives on disk next to the thumbnail cache.

Content-addressable means:

- Two files with identical bytes have the same identifier.
- A file referenced from multiple places in the session appears exactly
  once in the wire stream.
- Clients don't have to trust filenames — they trust hashes.
- Deduplication is automatic.

### 5.3 Upload flow

When an editor-scope client wants to push a changed asset to the server:

1. Client computes the asset's new content hash locally.
2. Client sends an `ASSET_ANNOUNCE` record on the reliable stream:
   `(filename, oldHash, newHash, byteSize)`.
3. Server checks if it already has `newHash` (because another client
   uploaded it, or it was present before the session started). If yes,
   server skips the upload and broadcasts the announce to other
   clients. If no, server requests the bytes.
4. Client streams the asset bytes on a dedicated reliable stream. Large
   assets are chunked; each chunk is acked via QUIC's stream flow
   control.
5. Server stores the bytes in its content-addressable store (keyed by
   `newHash`), updates its filename→hash mapping, and broadcasts the
   `ASSET_ANNOUNCE` record to other clients.
6. Other clients receive the announce and, if they don't already have
   `newHash`, request the bytes from the server on a reliable stream.
7. Once a client has the new bytes, it hot-reloads the asset through
   the existing recursive `AssetWatcher` machinery. All gameplay state
   that references the filename picks up the new content on the next
   access.

### 5.4 Thumbnail sync

The `ThumbnailCache` module already generates 256×256 PNG
thumbnails for mesh and texture assets via GPU bake. In a collab
session, when one client bakes a thumbnail, the others don't need to
re-bake — they can receive the baked bytes directly. The thumbnail
cache extends to:

1. Emit a `THUMBNAIL_AVAILABLE` record when it completes a bake, carrying
   the source asset hash and the thumbnail content hash.
2. Accept incoming `THUMBNAIL_AVAILABLE` records from other clients and
   populate the local cache with the received thumbnail bytes (if the
   source hash matches the local copy).
3. Invalidate on source-hash change — if a client's local copy of an
   asset differs from the baked thumbnail's source hash, the thumbnail
   is marked stale until a local re-bake.

Net effect: in a 4-developer session, a new asset is baked exactly once
across the whole session. The other three clients receive and cache the
PNG bytes without running Vulkan-side bake work.

### 5.5 Binary asset size limits

Asset sync is reliable-stream-based, which means QUIC flow control
bounds it. The engine imposes an additional per-asset cap (default:
256 MB) to prevent accidental uploads of enormous files choking the
session. Games can raise or lower the limit via config.

Sessions with very large asset changes (e.g. a 1 GB world heightmap)
fall outside live-collab scope. The workflow in that case is: dev
checks in the heightmap via source control, all devs restart the
session. That's not a failure of the engine — it's the right tool for
the size of the change.

---

## 6. Scene Save / Load

### 6.1 The snapshot-on-disk idea

A save file is a snapshot of the authoritative world state at a moment
in time. The replication runtime already knows how to serialize that
state — it does it every tick for join-with-snapshot. Saving
is just "run the snapshot encoder, write the bytes to a file instead of
a wire stream."

This means:

- Save and load are server-side operations, not client-side.
- The format is the same format used for join-with-snapshot, versioned
  the same way.
- A saved file is portable between sessions running the same game
  version.
- Loading a save replaces the authoritative state and triggers a
  join-with-snapshot broadcast to all connected clients, which is
  already the right mechanism for getting everyone back in sync.

### 6.2 What goes in a save

- All replicated components on all entities.
- The full undo log (optional — per-game config decides).
- The server tick counter.
- The asset filename→hash mapping (so the save knows which asset
  versions to reference).
- Server-side game state registered via the existing
  `SceneStatePersistence` machinery.

### 6.3 What does not go in a save

- Client-side prediction buffers (they're per-client, not world state).
- Transient effects (particles, sound instances).
- AdminPanel UI state (per-client).
- Asset bytes themselves (those are in the content-addressable store;
  the save references them by hash).

### 6.4 Save files vs live collab

Save files are snapshot checkpoints — they don't replace live collab,
they complement it. A team might save at the start of each dev session,
work live for 4 hours, save again at the end. The intermediate live
work is the actual collab; the saves are safety nets.

This is different from Unreal's model where the save file **is** the
persistent format and live collab is a sync layer on top. Ours is more
like "the running session is authoritative; saves are snapshots of
what's authoritative right now." Closer to the database transaction-log
model, which is how the undo log already works.

---

## 7. Interaction with Existing StratumV Subsystems

### 7.1 AdminPanel

The `AdminPanel` already has tabs for Render, Water, Terrain,
Weather, Player, and Assets. Editor mode adds more tabs
gated on `Editor` scope:

- **Scene** — entity list, placement tools, transform editing. Hooks
  into `SceneLoader` and `SceneNode` data model.
- **Asset Inspector** — the existing Assets tab gains editor-only
  import settings and push-to-server actions.
- **Edit Log** — the transaction log visible as a scrollable list, with
  undo/redo buttons at the top.
- **Permissions** (Admin scope only) — list of connected clients, their
  scopes, and buttons to promote/demote/kick.

None of these need new engine infrastructure — they're ImGui code in
`AdminPanel.cpp` guarded by scope checks. The scope check is a single
call to `ctx.network.scope() >= PermissionScope::Editor`.

### 7.2 DevServer

The `DevServer` is a TCP :9999 debug socket for the engine. It's
the way devs inspect and mutate state from CLI tools and scripts.
It remains the right place for **out-of-band commands** — things
you issue as a developer from a terminal, not things you do inside the
game UI.

DevServer commands that a live-collab session might want:

- `list_clients` — show connected clients and their scopes.
- `save_snapshot filename.sav` — write a save file to disk.
- `load_snapshot filename.sav` — restore from a save file.
- `grant_scope clientId Editor` — elevate a client.
- `list_undo 20` — show the last 20 transactions in the log.

All of these are implemented as DevServer command handlers on the
server side, registered via the existing `registerCommand` mechanism.
Zero engine API changes; just new handlers.

### 7.3 BaseSystemContext

Live collab adds fields to the nested `network` sub-struct on
`BaseSystemContext`. Candidates:

```cpp
struct NetworkContext {
    INetworkContext*      network             = nullptr;
    PermissionScope       localScope          = PermissionScope::Spectator;
    TransactionLog*       transactionLog      = nullptr;
    AssetSyncEngine*      assetSync           = nullptr;
    // ... transport and replication fields ...
};
```

Games read these through `ctx.network.*` (same pattern as
`ctx.input.*`, `ctx.buffers.*`, etc.). DLL plugins can initiate edit
transactions via `ctx.network.transactionLog->begin(...)`, check their
scope, and subscribe to asset-change events.

The exact field list is given in section 9.1. This is a preview to show
the plug points.

### 7.4 The lab harness

The `lab/skinned_test` harness already has a Vulkan window,
the ImGui panels, and the asset browser wired up. The minimal validation
is two `skinned_test` instances on loopback, one in `Editor`
scope and one in `Player` scope, with the editor client able to move
the camera or move a placed entity and the player client seeing the
change via transaction replication.

This is a substantially lower bar than launching a full consumer game and
getting developers collaborating in a finished world — which is the right
bar for validating the primitive without dragging a full game in.

---

## 8. Scope Exclusions

What collaborative editing explicitly does not try to be.

- **Not a full IDE.** No code editing, no debugger, no autocomplete.
  Developers write code in their own IDE and rebuild the game DLL; the
  existing hot-reload (`AssetWatcher` + `DLLLoader`) picks up the new
  binary. Collab is about **world and data** editing, not code editing.
- **Not a visual scripting system.** No ProtoFlux, no Blueprint, no
  node graph. Gameplay logic is C++ in DLL plugins. Collab editing can
  touch the data those plugins read (parameters, tuning curves, asset
  references) but not the plugin code itself.
- **Not a file-level merge tool.** When two devs both edit an asset file
  outside the session (via Blender, CC5, Photoshop), the resolution is
  source control. Collab only covers edits made through the running
  session.
- **Not a commit history.** The undo log is session-scoped. Long-term
  history, branches, and code review are source control concerns.
- **Not a chat or voice channel.** Communication happens out-of-band
  (Discord, Slack, in-person). If a game wants in-engine chat it's a
  DLL plugin.
- **Not an access control system.** Four coarse scopes, no ACLs, no
  per-resource grants. Games that need finer controls build them.
- **Not a crash-safe autosave.** The undo log and transaction stream
  give some of the benefit but periodic save-to-disk is a game-level
  policy. Games can schedule a `save_snapshot` via DevServer on a
  timer; the engine doesn't force it.

Every item on this list is deferrable without compromising the core
collab feature. The core feature is: two or more devs, same running
world, scene + asset edits flowing in real time, undo available. That's
the minimum viable collab primitive, and it's what the collab substrate
is aimed at.

---

## 9. Implementation History

The collab substrate landed incrementally across several engine
releases. Each subsection records what shipped in a given version bump.

- **1.3.3 → 1.3.4** (§9.1) — the wire format + authoritative server loop
  + scope gating with the existing NetTransform.
- **1.3.4 → 1.3.5** (§9.2) — generic per-type payload dispatch via
  `ReplicationRegistry::findByHash` + `encodeSnapshot`, join-with-snapshot
  replays at CURRENT state, `--server-data` CLI flag plus periodic
  autosave + SIGINT-flush world persistence.
- **1.3.5 → 1.3.6** (§9.3) — asset sync + thumbnail replication: an editor
  client drops a new texture into the asset browser, the texture bytes
  stream to the server, other clients receive the bytes and see the new
  asset without touching disk.
- **1.3.6 → 1.3.8** (§9.4, §9.5) — Blender live link as a first-class
  editor client, asset push from Blender, and scene-hierarchy sync.

After asset sync lands, the core collab primitive is complete. Per-game
collab integration (e.g. a world editor or level-tuning tools) lives in
the consumer repos.

### 9.1 Wire format + authoritative loop (1.3.3 → 1.3.4)

The first slice of the collab substrate. Scope was
deliberately kept to "prove the wire format + authoritative
server loop + scope gating with the existing NetTransform", without
touching the
replication registry's type-dispatch path.

**New core modules** (all under `stratumv_core.lib`, pure logic, no
MsQuic or Vulkan):

- `src/engine/PermissionScope.h` — header-only `enum class
  PermissionScope { Spectator, Player, Editor, Admin }` + ladder
  comparison operators + `permissionScopeFromByte` safe narrowing
  (out-of-range → Spectator, never elevates).
- `src/engine/EditTransaction.h` + `.cpp` — `EditKind` enum (SetField
  / Undo / Redo / Spawn / Despawn), `EditTransaction` struct, 33-byte
  wire header (`msgType`/`kind`/`txId`/`originClientId`/
  `requiredScope`/`entityId`/`typeNameHash`/`timestampMs`/
  `payloadLen`), `encodeEditTransaction` + `parseEditTransaction`,
  plus NetTransform-specific payload helpers
  (`writeNetTransformLE` / `readNetTransformLE` /
  `writeSpawnPayload` / `readSpawnPayload`). This slice shipped
  NetTransform-only on the wire; per-type dispatch via
  `ReplicationRegistry` meta is a follow-up (§9.2).
- `src/engine/UndoLog.h` — header-only `UndoEntry` + `UndoLog` class.
  LIFO per-client walk-back (`findLatestUndoable(clientId)` +
  `findLatestRedoable(clientId)`). Shared storage across all clients
  so late joiners can see a coherent picture once join-with-snapshot
  is added (§9.2). This slice does NOT prune redo state when a new
  edit is appended — a documented quirk; per-client branch cursors
  are future work.
- `src/engine/net/ReplicationProtocol.h` + `.cpp` — added
  `kFrameEditTransaction = 3` and `kFrameWelcome = 4` to the wire
  message type enum, plus the `WelcomeMessage` struct (10 bytes:
  msgType + clientId + scope + avatarEntityId) with
  `encodeWelcomeMessage` / `parseWelcomeMessage`. Reserved
  `kErrScopeDenied = 1002` for a future scope-denied QUIC close
  code (server currently logs + drops).

**BaseSystemContext additions** (1.3.0 NetworkContext shape → 1.3.4
extension, still additive so no ABI break):

```cpp
struct NetworkContext {
    INetworkContext* context        = nullptr;   // since 1.3.0
    net::Transport*  transport      = nullptr;   // since 1.3.1
    PermissionScope  scope          = PermissionScope::Spectator;  // 1.3.4
    uint32_t         clientId       = 0;          // 1.3.4
    uint32_t         avatarEntityId = 0;          // 1.3.4
};
```

The client fills these from the Welcome message after connecting.
Games wire their own `NetworkContext` pointer into
`AdminBindings::networkContext` so the AdminPanel Edit tab can read
the scope directly.

**Server (`src/stratumv_server/main.cpp`) — big rewrite:**

- Introduces `ServerWorld` with an entity map
  (`std::unordered_map<uint32_t, ReplicatedEntity>`) + monotonic id
  counters + the undo log. Entity 1 is the server-owned
  orbiting cube; entities 100+ are per-client
  avatars allocated on accept.
- `ClientState` struct now carries per-connection progress markers
  (`preambleSent` / `welcomeSent` / `worldSynced` / `dead`) and a
  mutex-protected inbox for the reliable-stream messages the
  MsQuic worker thread delivers. `std::shared_ptr<ClientState>` +
  `std::weak_ptr` in the callback lambda keeps the worker thread
  from dangling on reap.
- `tickClientHandshake()` runs a per-client state machine every
  tick: send preamble → send welcome → replay Spawn transactions
  for every live entity → broadcast this client's avatar spawn to
  the other already-welcomed clients → mark worldSynced.
- `applyClientTransaction()` handles SetField / Undo / Redo. The
  server OVERWRITES origin/txId/timestamp fields on every inbound
  transaction so clients cannot forge identity. Owner-authority
  check: only the owning client can SetField an
  `Authority::Owner` entity. Server-authoritative entities are
  server-only (clients get denied).
- `broadcastAllEntities()` walks the entity map once per tick and
  emits one datagram per entity per welcomed client (N*M, fine
  for ≤8 clients + ≤10 entities at this stage; interest
  management is future work).
- Reap path on disconnect: erase from entity map + broadcast a
  Despawn transaction to every remaining welcomed client.

**Client (`lab/skinned_test/main.cpp`) — extended:**

- Multi-entity state: `std::unordered_map<uint32_t, ClientEntity>`
  replaces the single-cube `m_netCurrent` tracking (kept as a
  legacy mirror for the text readout). Each entity carries its own
  prev/current state + per-entity interpolation alpha.
- Reliable-stream dispatch: the existing schema
  handshake handler is extended to switch on `msgType` and route
  `kFrameWelcome` (stores identity fields under mutex) and
  `kFrameEditTransaction` (queues for main-thread processing).
  Unknown msg types log + drop.
- `drainNetReliableInbox()` new method — runs at the top of every
  frame before `drainNetInbox()`. Spawn transactions populate the
  entity map, Despawn removes. SetField/Undo/Redo are logged but
  not acted on (datagram snapshots carry the new state).
- `sendAvatarMove()` / `sendUndoRequest()` / `sendRedoRequest()` —
  client-side senders that construct a SetField / Undo / Redo
  transaction and push it on the reliable stream. All three are
  scope-gated (`m_netScope >= Editor`) and welcome-gated.
- Network Demo panel adds: identity readout
  (clientId, avatarEntityId, scope), Ctrl+Z/Ctrl+Y keybinds +
  arrow-key avatar movement, six ImGui buttons (X- / X+ / Z- / Z+
  / Undo / Redo), per-entity dots on the XZ canvas with colour
  (orange cube / cyan local avatar / magenta other avatars) and
  ImGui-drawn text nameplates ("Client1", "Client2 (you)", etc.).

**AdminPanel Edit tab:** new `drawTabEdit()` method gated on
`AdminBindings::networkContext != nullptr`. Shows identity +
scope, then `ImGui::BeginDisabled(!canEdit)` around the four
move buttons + Undo + Redo. Games wire up
`onAvatarMove`/`onUndo`/`onRedo` callbacks in their admin-panel
setup. When `scope < Editor` the buttons are greyed out but the
identity line still renders so a spectator can see they're
authenticated but read-only.

**Tests (24 new cases, `[edit]` tagged, `sv_core_tests`):**

- PermissionScope ladder + `permissionScopeFromByte` safe narrowing
- `EditKind` toString round-trip
- NetTransform wire round-trip + short-buffer rejection
- Spawn payload round-trip + short-buffer rejection
- EditTransaction wire: full SetField round-trip,
  Spawn-with-owner round-trip, header-only Despawn,
  malformed/truncated rejection, wrong-msgType rejection, payload
  overrun rejection
- WelcomeMessage round-trip + rejection
- UndoLog: empty state, record + findLatestUndoable, per-client
  LIFO walk-back, markUndone / markRedone toggle, missing-txId
  no-op, undo-then-redo round-trip, per-client undoableCount /
  redoableCount diagnostics
- Wire layout invariant: hand-summed header width matches
  `kEditTransactionHeaderSize`; `kSpawnPayloadSize` matches
  NetTransform + u32

**End-to-end evidence:**

- Full Windows build (`stratumv.lib` + `stratumv_core.lib`): 324/324
  tests passing (300 baseline + 24 new `[edit]`).
- Core-only Linux-style build (`STRATUMV_CORE_ONLY=ON`): 109/109
  tests passing (85 baseline + 24 new). Boundary probe in
  `test_StratumVCore.cpp` confirms the new core TUs do not pull
  volk/glm/ImGui/ozz.
- Live smoke test: server + two staggered clients reproduces the
  full flow. Server log shows `Accepted connection #1 (clientId=1,
  avatar=100)` → `Replayed 2 spawn tx to client 1` → `Accepted
  connection #2 (clientId=2, avatar=101)` → `Replayed 3 spawn tx to
  client 2` (cube + client 1 avatar + client 2 own avatar) →
  `tick=360 entities=3 clients=2 sent=6 pending=0` → on disconnect
  `Client N disconnected (avatar=N reaped)`.
- Side-by-side capture of two clients. Left panel is Client 1's view
  (cube + own cyan avatar, "Client1 (you)" nameplate). Right panel is
  Client 2's view (cube + own cyan avatar "Client2 (you)" +
  magenta "Client1" dot proving cross-client visibility).

**Explicitly out of scope at this stage** (deferred to later work):

- Per-type EditTransaction dispatch — the wire format is hardcoded
  to NetTransform. Generalising to any `SV_REPLICATE` type via
  `ReplicationRegistry::findByHash` + runtime encode/decode
  dispatch landed later (§9.2).
- Join-with-snapshot + world persistence — new clients only see
  the current transaction state via the Spawn replay; they do not
  see the history of SetField edits made before they joined. The
  server does not persist the world file; all state is lost on
  restart.
- Client-side undo log mirror — clients rely entirely on
  datagram snapshots for state updates. The server rebroadcasts
  Undo/Redo transactions so clients *can* mirror the log, but the
  lab harness does not consume them.
- Explicit concurrent-edit locking — this slice ships the
  last-write-wins conflict policy documented in §3.4. Locks /
  claim primitives are future work.
- Production certificate management — the server still uses the
  self-signed loopback cert helper.
- AdminPanel Edit tab wiring in consumer games — consumers must
  adopt the `bindings.network = ...` rename and a dep bump to 1.3.4.

### 9.2 Generic dispatch + persistence (1.3.4 → 1.3.5)

The second slice of the collab substrate. Three sub-goals
landed: generic per-type EditTransaction payload
dispatch, join-with-snapshot at CURRENT state, and disk-backed world
persistence with load-at-startup + periodic autosave + SIGINT flush.
Nothing in the 1.3.4 wire layout needed to change; the extensions
are purely additive and older [edit] tests continue to pass
as-is.

**New / extended core modules** (all in `stratumv_core.lib`):

- `src/engine/EditTransaction.h` + `.cpp` gain four generic helpers:
  `writeGenericSetFieldPayload(meta, instance, mask, out)` +
  `readGenericSetFieldPayload(typeNameHash, data, size, out, mask)` +
  `writeGenericSpawnPayload(meta, instance, ownerClientId, out)` +
  `readGenericSpawnPayload(typeNameHash, data, size, outOwner,
  outInstance, outMask)`. All four dispatch through
  `ReplicationRegistry::findByHash` → `encodeSnapshot` /
  `decodeSnapshot`, so any `SV_REPLICATE`'d component type can flow
  on the SetField / Spawn wire without hand-written raw codecs.
  The earlier `writeNetTransformLE` / `readNetTransformLE` /
  `writeSpawnPayload` / `readSpawnPayload` helpers still exist and
  are still covered by their own wire-layout tests, but the server
  and lab client have switched to the generic path. The Spawn
  payload format is `[u32 ownerClientId][encodeSnapshot full-mask
  payload]` — the server reads `ownerClientId` via a single
  `readU32LE` without dispatching through the field list.
- `src/engine/WorldPersistence.h` + `.cpp` — new module. Binary
  file format (`SVWLD001` magic + u32 version + u32 entityCount +
  u32 nextEntityId + u32 nextClientId + u64 nextTxId + per-entity
  records carrying `encodeSnapshot` bytes). Public API:
  `saveWorldToFile(path, world)` / `loadWorldFromFile(path, outWorld)`
  / `encodeWorldToBytes` / `decodeWorldFromBytes` plus the
  `PersistedWorld` / `PersistedEntity` POD value types. Save uses
  a temp-file + rename swap so an interrupted flush never leaves a
  half-written file at the target path. Load returns
  `MissingFile` on a non-existent path (not a failure — the server
  treats it as "start fresh"), `CorruptHeader` on magic / field
  drift, `UnsupportedVersion` on a version > 1, `UnknownType` on a
  `typeNameHash` not in the local `ReplicationRegistry`. The
  module is header-isolated from the server's `ReplicatedEntity`
  struct via the intermediate `PersistedEntity` POD, which keeps
  it dependency-free enough to live in the core subset alongside
  `EditTransaction` and `ReplicationProtocol`.

**Server (`src/stratumv_server/main.cpp`) — rewrite of the
state init + tick loop:**

- New CLI flags: `--server-data DIR` (enables persistence;
  otherwise the server stays fully ephemeral as before) and
  `--save-interval-sec N` (autosave cadence in seconds, default
  30). Adding the flag with a valid directory triggers a load
  attempt at startup (`loadWorldFromFile`), a periodic flush via
  a `persistWorldNow("autosave")` lambda inside the tick loop,
  and a final flush on SIGINT before `Transport::stop()`.
- `makeSpawnTransaction` / `makeDespawnTransaction` /
  `applyClientTransaction` now take a `const
  sv::ReplicationMeta&` so the generic encode / decode path can
  dispatch without the registry lookup duplicated at every call
  site. Scope-gating, owner-authority checking, and the
  overwrite of client-supplied origin/txId/timestamp fields are
  unchanged — the only real change is the payload codec.
- `snapshotWorldForPersistence(world, meta)` + `applyPersistedWorld(
  world, persisted, meta)` + `worldFilePath(serverDataDir)` are the
  three server-local bridges between the in-memory
  `ReplicatedEntity` map and the on-disk `PersistedEntity` POD.
  Avatars are intentionally NOT persisted — they are
  `Authority::Owner` and tied to a live client connection, so
  after a restart they would point at stale client ids. Only
  server-authoritative entities (the orbiting cube) are written
  to disk. The `nextEntityId` / `nextClientId` / `nextTxId`
  counters ARE persisted, so per-session monotonicity survives a
  restart (a visible side effect is that clients reconnecting
  after a restart get a client id strictly greater than anyone
  who connected before the restart).
- Join-with-snapshot: the `tickClientHandshake` step-3 replay
  calls `makeSpawnTransaction` with `ent.transform` (the LIVE
  current state, updated every tick for the cube and
  per-mutation for avatars via SetField). A third client joining
  a running session therefore sees the orbiting cube at its
  current phase rather than a reset initial state, and sees
  every already-connected client's avatar at its most recently
  edited position. This is technically a behaviour fix on top of
  the earlier replay — that path was already passing
  `ent.transform`, but without the generic encoder the server
  could only send the raw 28-byte NetTransform blob, which has
  no schema version so a drifted client would happily decode
  garbage. The generic path runs through
  `decodeSnapshot` which verifies `schemaVersion` and refuses on
  drift.

**Client (`lab/skinned_test/main.cpp`) — generic dispatch:**

- `drainNetReliableInbox` Spawn handling routes through
  `readGenericSpawnPayload` instead of the earlier
  `readSpawnPayload` raw-byte path. The ClientEntity layout is
  unchanged — the decoded `NetTransform` is pasted into
  `prev`/`current` state the same way.
- `sendAvatarMove` encodes via `writeGenericSetFieldPayload` with
  a full-mask DirtyMask. Future work can partial-mask the
  three position deltas for bandwidth, but this slice keeps the
  full mask because the wire overhead on the reliable stream is
  ~30 bytes and the server-side diff is not measurably cheaper.
- Nothing else in the lab harness changed. The Network Demo
  panel, per-entity canvas rendering, keybinds, and auto-exit
  capture frame are unchanged.

**Tests (18 new cases, both `sv_core_tests` flavors):**

- `tests/test_EditTransaction.cpp` (+6 `[edit][wire][generic]`
  cases): NetTransform round-trip via ReplicationRegistry,
  partial-mask preservation (untouched fields survive the
  decode), null/unknown-type/short-buffer rejection, Spawn
  payload owner prefix + full-mask body round-trip, spawn
  short-buffer rejection, end-to-end via full
  `encodeEditTransaction` → `parseEditTransaction` →
  `readGenericSetFieldPayload`.
- `tests/test_WorldPersistence.cpp` (+12 `[persistence]` cases):
  status-string coverage, empty-world encode/decode round-trip
  (header-only), three-entity round-trip preserving every
  field, decoded payloads re-inflating via
  `readGenericSetFieldPayload`, save+load file round-trip,
  overwrite via atomic swap, MissingFile on non-existent path,
  corrupt magic rejected, unsupported version rejected,
  short-buffer rejection, unknown-typeNameHash rejection,
  parent-directory auto-create on save. Uses a local
  `CoreTempDir` RAII helper that duplicates the
  `tests/test_util.h::svtest::TempDir` pattern — `sv_core_tests`
  must not pull graphics-adjacent headers into its include path,
  so the duplication is load-bearing for the boundary probe in
  `test_StratumVCore.cpp` to keep working.

**End-to-end evidence:**

- Full Windows build: **342/342** tests passing (324 baseline +
  18 new). Core-only Linux-style build: **127/127** tests
  passing (109 baseline + 18 new).
- Live persistence smoke test: `stratumv_server --port 9301
  --server-data tmp/... --save-interval-sec 2` writes a 94-byte
  `world.svbin` after the first autosave. Hexdumping the file
  decodes to `SVWLD001 v1` + 1 entity (`OrbitingCube`) + a
  31-byte payload (2 bytes `schemaVersion` + 1 byte byte-packed
  mask + 7 floats). Restarting the server with the same
  `--server-data` path emits `[WorldPersistence][info] loaded
  tmp/.../world.svbin (94 bytes, 1 entities)` and `[Server][info]
  Loaded 1 entities from ... (nextEntityId=100 nextClientId=1
  nextTxId=1)`. The orbiting cube's phase is preserved across
  the restart.
- Capture of client 3's view of a three-client session. Panel shows
  `Connected`, `Identity: client 6, avatar 105`, `Scope: Editor`,
  `Entities: 4`, `Reliable: rx 6, tx 0`, `Datagrams: 148`,
  `Decoded: 148`, `Dropped: 0`. The top-down canvas has four
  labelled dots: orange `Cube`, magenta `Client4`, magenta
  `Client5`, cyan `Client6 (you)`. The client id starts at 6
  rather than 3 because this test ran against a server that had
  persisted counters from earlier runs — a visible side
  effect of `--server-data` preserving `nextClientId`.

**Explicitly out of scope at this stage** (deferred to later work):

- Asset sync + thumbnail replication — new textures / meshes do
  NOT flow across the wire (added in §9.3).
- Additional replicated component types — this slice still ships
  with NetTransform as the only registered type. The generic
  dispatch path is ready for more types, but adding a second
  type requires a new `SV_REPLICATE` block and a `Field-type
  coverage` update in `REPLICATION_CONTRACT.md`.
- Client-side undo log mirror UI — the server still rebroadcasts
  Undo / Redo transactions, and the lab harness still logs but
  does not act on them.
- Explicit concurrent-edit locking — last-write-wins stays in
  place.
- Production certificate management — still the self-signed
  loopback helper.
- Headless Linux VM bring-up of the dedicated-server carve-out
  (still deferred).

---

### 9.3 Asset sync (1.3.5 → 1.3.6)

The third slice of the collab substrate. Asset sync +
thumbnail-scope replication land via a new content-addressable
store on the server plus three new wire message types on the
reliable stream. The core subset picks up three new modules —
`Sha256`, `AssetPersistence`, `AssetUploadClient` — and the
dedicated-server carve-out still compiles without any graphics
dependencies.

**New core-subset modules** (all in `stratumv_core.lib`):

- `src/engine/Sha256.{h,cpp}` — pure C++ FIPS 180-4 SHA-256. Two
  entry points: a one-shot `sha256(data, size)` free function and
  an incremental `Sha256Hasher` that buffers partial blocks between
  `update` calls. Plus `digestToHex` / `digestFromHex` helpers for
  the hex path used by the on-disk store. Zero dependencies — a
  hand-rolled scalar implementation keeps the core subset from
  needing an OpenSSL or BLAKE3 build target just for one hash.
- `src/engine/AssetPersistence.{h,cpp}` — server-side content-
  addressable store. Always maintains an in-memory `AssetRecord`
  cache keyed by hex digest; `setRootDir(<server-data>/assets)`
  enables write-through persistence at
  `<root>/<2hex>/<62hex>.bin` + sibling `.meta.json`. Save
  verifies `sha256(bytes) == declared hash` before touching
  memory or disk, so a malicious client cannot pin arbitrary
  bytes under a chosen hash. `setRootDir` scans the root on
  startup, re-hashes each file as it reads it, and refuses
  corrupt or tampered bytes. Atomic writes via temp + rename.
- `src/engine/AssetUploadClient.{h,cpp}` — shared sender /
  receiver state machine used by both `stratumv_server` (to
  broadcast cached bytes to other welcomed clients) and client
  engines (to upload their own bytes). `buildAssetAnnounce` /
  `buildAssetChunks` slice a contiguous byte buffer into wire
  messages; `AssetReceiver::depositChunk` handles assembly and
  per-chunk receipt tracking. Default chunk size is
  `kAssetChunkSize = 65536` (64 KiB) — conservative for MsQuic
  reliable streams. Upper-bounded by `kAssetByteLimit = 64 MiB`
  per asset (configurable for the very large
  assets that fall outside live-collab scope per §5.5).

**Wire protocol additions** (`src/engine/net/ReplicationProtocol.{h,cpp}`):

- `kFrameAssetAnnounce = 5` — `[u8 msgType][32 hash][u32 byteSize][u8
  kind][u16 nameLen][name bytes]`. Fixed header 40 bytes.
- `kFrameAssetChunk    = 6` — `[u8 msgType][32 hash][u32 chunkIndex][u32
  chunkCount][u32 chunkLen][chunk bytes]`. Fixed header 45 bytes.
- `kFrameAssetAck      = 7` — `[u8 msgType][32 hash][u8 status]`.
  Fixed 34 bytes. Status is `AssetAckStatus::NeedChunks` (0) or
  `HaveIt` (1); unknown bytes downgrade to NeedChunks on parse.

All three sit on the reliable stream alongside
`kFrameEditTransaction`, `kFrameWelcome`, and
`kFrameSchemaHandshake`. The drain path on both the server and
the lab harness dispatches on the first byte so edit
transactions and asset messages coexist on one stream per
connection.

**Server (`src/stratumv_server/main.cpp`):**

- New `AssetPersistence m_assetStore` instance, initialised from
  `--server-data` if the flag is set. Without `--server-data`
  the store still works — assets stay in the in-memory cache
  only and are lost on restart.
- `drainClientInbox` now dispatches on `msgType[0]`. The
  EditTransaction branch is unchanged from the prior slice; the three
  new branches are:
  - **AssetAnnounce**: scope-check (Editor required), look up
    the CAS by hash. Cache hit → reply with `Ack(HaveIt)` and
    broadcast the cached bytes to other welcomed clients via
    `pushAssetToConnection`. Cache miss → allocate a per-client
    `AssetReceiver`, reply with `Ack(NeedChunks)`, and wait for
    Chunk messages.
  - **AssetChunk**: route to the matching receiver by hex hash;
    on completion verify SHA-256, call `AssetPersistence::save`
    (which logs `dedup hit` if another client beat us to it),
    and broadcast the authoritative bytes to the other welcomed
    clients. Invalid chunks drop the entire in-progress upload
    rather than leaving a half-assembled receiver dangling.
  - **AssetAck**: server currently doesn't accept upstream acks
    — they're logged and dropped. Future work can use
    client-side acks to signal "don't send me chunks, I already
    have this hash" for the broadcast direction.
- New helper `broadcastAsset(rec, origin, clients)` slices a
  cached record into Announce + Chunks and pushes them on every
  welcomed client's connection except the uploader.
- SIGINT log now includes `assets=N assetBroadcasts=N` so the
  shutdown heartbeat reflects how much asset traffic the
  session carried.

**Client (`lab/skinned_test/main.cpp`):**

- New `sv::AssetPersistence m_assetStore` instance on the lab
  harness — one per process, shared between the upload path
  and the receive path. Uploaded-locally and received-from-
  server assets both live here.
- New `uploadAssetFromDisk(relPath, absPath)` method reads the
  file via `std::ifstream`, hashes with `sv::sha256`, builds
  `Announce + Chunks` via `AssetUploadClient`, and pumps each
  wire buffer through `m_netConn.sendReliableMessage`. Pinned
  into the local CAS immediately on success so the uploader
  can see its own upload in the Replicated Assets panel.
- New `drainAssetInbox()` method runs every frame before the
  panel draw. Announce → allocate an `AssetReceiver` (unless
  we already have the hash cached, in which case we dedup-drop
  the incoming broadcast). Chunk → deposit into the matching
  receiver; on completion verify + save + push a row onto the
  UI. Ack(HaveIt) → bump the local `m_assetDedupHits` counter
  so the panel reflects that the server short-circuited our
  upload.
- New "Replicated Assets" ImGui panel lists every
  cached asset with a per-row tag (`[uploaded]` lime / `[from
  server]` cyan) + byte size + 12-char hash prefix. Header
  shows the local CAS size and the upload / receive counters.
- New "Upload to server" button under the Asset
  Browser's selected-entry detail. Disabled until the client
  is connected AND welcomed AND at Editor scope.

**Tests (22 new cases, `sv_core_tests`, both flavors):**

- `tests/test_AssetPersistence.cpp` covers the full asset-sync
  surface. Sub-tags `[sha256]`, `[hex]`, `[chunkcount]`,
  `[announce]`, `[chunk]`, `[ack]`, `[upload]`, `[receiver]`,
  `[store]`, `[disk]`, `[paths]`, `[status]`.
  - SHA-256: empty input vector, FIPS 180-4 appendix B.1
    "abc" vector, appendix B.2 two-block vector (56 bytes
    crosses the block boundary), incremental hasher equivalence
    across varied chunk sizes (1/63/64/65/1000/32768 bytes),
    finalize() idempotence.
  - Hex: round-trip + short / long / non-hex rejection + case-
    insensitive accept.
  - Chunk arithmetic: `assetChunkCount` for 0 / empty / exact /
    tail-byte sizes, `chunkSize=0` caller-error path, real
    150 KiB / 64 KiB split.
  - Announce: encode + parse round-trip, reject short buffer /
    wrong msgType / nameLen overrun.
  - Chunk: encode + parse round-trip, reject wrong msgType /
    chunkLen overrun / chunkIndex ≥ chunkCount.
  - Ack: round-trip both statuses, unknown status byte
    downgrades to NeedChunks.
  - Upload client: 150 KiB asset slices into 3 chunks of
    64 KiB / 64 KiB / 22 KiB with the right header + payload
    sizes per chunk.
  - Receiver: Announce → chunk sequence → verifyHash round-
    trip, double-deposit rejection, out-of-range index +
    wrong-length rejection, verifyHash fails after byte
    tampering.
  - AssetPersistence: save + find + load round-trip, dedup hit
    on re-save is a no-op, hash mismatch rejection, load on
    missing hash returns `MissingFile`, `setRootDir` + disk
    round-trip via a temp dir, reopened store rehydrates from
    the on-disk cache.
  - Path composition: `assetFilePath` / `assetMetaPath` for a
    known digest value, empty root → empty string.
  - Status strings: every enum value maps to a stable label.
- Full Windows build: **364/364 tests passing** (342 baseline
  + 22 new). Core-only build (`STRATUMV_CORE_ONLY=ON`):
  **149/149** (127 baseline + 22 new).

**End-to-end evidence:**

- Capture showing Client A (with the Asset Browser's Upload button and
  a populated Replicated Assets panel containing the locally-
  uploaded entry) and Client B (with the same asset showing
  up in its Replicated Assets panel tagged `[from server]`).
  Server running with `--server-data tmp/assets/... --port
  9320`. Client A clicks Upload on
  `textures/grass_albedo.png`; Client B's panel populates on
  the next frame (~33 ms at 30 Hz network tick, ~500 ms worst
  case for the full reliable-stream round trip).

**Explicitly out of scope at this stage** (deferred to later work):

- No Blender live link — added in §9.4 / §9.5.
- No upload resume / chunk-retry logic. A single chunk failure
  drops the entire in-progress upload rather than retrying the
  failing chunk.
- No `AssetWatcher` integration — the upload is a manual-click
  action rather than "save the file, see it auto-replicate".
- No BaseSystemContext shape change.
- No per-client bandwidth budgeting.
- No production certificate management.
- No consumer-game fixes — downstream games still need the
  `bindings.network = ...` rename.
- No server-to-client ack negotiation on the broadcast path.
  Broadcasts always push Announce + Chunks; receiving clients
  dedup silently on hash hit.
- No concurrent-edit locking or per-author branching of the
  asset upload queue.

### 9.4 Blender live link (1.3.6 → 1.3.7)

**Goal:** Blender becomes a first-class editor client of a running
StratumV session. An artist running `blender` next to a running
`skinned_test --connect 127.0.0.1:9331 --editor-bridge-port 9401`
can click Connect in a new "StratumV Live Link" sidebar panel, see
every replicated entity in the shared world appear as Blender
objects, and — on any transform edit — push the new state up
through skinned_test and on to the rest of the session.

**Topology.** The edit flow from Blender to the server is a three-hop
path rather than a direct connection:

```
Blender (Python addon, plain socket)
    ↓ TCP 127.0.0.1:9401 (length-prefixed bridge protocol)
skinned_test (EditorBridge mode, translator)
    ↓ MsQuic reliable stream (EditTransaction / SetField)
stratumv_server
    ↓ MsQuic datagrams (NetTransform snapshots)
all welcomed clients (including any other skinned_test or Blender)
```

This topology is a deliberate simplification of the original
"Blender speaks QUIC directly" framing. The motivation:

- Pure Python QUIC (aioquic) needs a `pip install` into Blender's
  bundled Python plus an asyncio background loop that fights with
  Blender's single-threaded operator model.
- A ctypes wrapper over `msquic.dll` is ~400 lines of C API
  bindings with callback-from-C-to-Python threading traps.
- Plain TCP between Blender and a nearby skinned_test needs only
  `socket` + `struct` from the stdlib and stays orthogonal to the
  engine's transport choice. Swapping MsQuic out later is zero
  Blender-addon churn.

The cost is one extra hop (skinned_test sits in the middle as a
translator) but the benefit is a clean DLL boundary: Blender never
links against msquic.dll and the bridge protocol is ~6 message
types that can be documented in a single comment block.

**Bridge protocol.** Defined in `src/engine/net/EditorBridge.h`.
Every message on either direction is `[u32 payloadLen][u8 msgType]
[payload bytes]` where `msgType` is part of the payload (so the
reader can pre-allocate the full buffer once per frame). Layout is
little-endian throughout.

Downstream (bridge → Blender):
- `kBridgeMsgHello = 0x01` — one per connection, right after accept.
  Carries the bridge's own clientId, avatarEntityId, scope, server
  semver, NetTransform schema version, a `serverState` byte, and an
  appName string. Lets the Blender side display "connected to
  StratumV 1.3.6 as Editor on client 1 / avatar 100".
- `kBridgeMsgEntityState = 0x02` — one per entity push. Includes
  entityId, ownerClientId, an `isSelf` flag, the Authority byte, a
  full 7-float NetTransform, and a label string.
- `kBridgeMsgEntityGone = 0x03` — entity despawn notification.
- `kBridgeMsgServerState = 0x04` — pending / tls-ok / welcomed.

Upstream (Blender → bridge):
- `kBridgeMsgMoveSelf = 0x81` — 28 bytes of IEEE-754 floats in
  NetTransform order. The bridge translates this to a full-mask
  SetField EditTransaction aimed at the bridge's own avatar and
  forwards it to the server over the existing QUIC reliable stream.
- `kBridgeMsgPing = 0x82` — keepalive.

**EditorBridge class** (`src/engine/net/EditorBridge.h` + `.cpp`):

- Listens on `127.0.0.1:<port>` only. Loopback-only is an
  intentional safety rail — a Blender live-link tool is a dev
  feature, not a WAN service.
- One accept thread per Bridge + one reader thread per accepted
  client. Idempotent `start()` / `stop()`, destruction-order-safe
  (tracks dead clients with an atomic flag so `stop()` can force a
  close + join without deadlocking on recv).
- `setHello` / `setServerState` / `pushEntityState` / `pushEntityGone`
  are called from the main thread as the lab harness's `m_netEntities`
  map changes. `drainMoves()` returns pending MoveSelf requests per
  frame; the lab harness turns each into a full-mask SetField via
  `sv::writeGenericSetFieldPayload`.
- Not included in the `stratumv_core` carve-out (`cmake/stratumv_core_
  sources.cmake`). The Linux dedicated-server path has no use for a
  live-link gateway and the module uses winsock2 today.

**Lab harness integration** (`lab/skinned_test/main.cpp`): new CLI
flag `--editor-bridge-port N` (off by default). When non-zero:

- `maybeStartEditorBridge()` fires the first frame after the Welcome
  message arrives — sets the cached Hello from the client's own
  identity + NetTransform schema version + `STRATUMV_VERSION_*`
  macros, walks `m_netEntities` to seed the bridge's cache, and
  calls `EditorBridge::start(port)`.
- The main `onFrame` loop, after draining the reliable + datagram
  inboxes, pushes an `EditorBridgeEntityState` per known entity to
  the bridge (keeping connected Blender clients up to date), then
  calls `pumpBridgeMoves()` which drains any MoveSelf requests and
  forwards them as SetField transactions via the existing
  `m_netConn.sendReliableMessage` path.
- The `Despawn` case in `drainNetReliableInbox` calls
  `EditorBridge::pushEntityGone(entityId)` before erasing from the
  local map so Blender sees removals in lock-step.
- The Network Demo ImGui panel grows a new "Bridge: :9401 clients=N"
  line plus `Bridge rx/tx`, and `Blender edits applied: N (states
  pushed N)` counters under the existing edit buttons.

**Blender addon** (`tools/blender/stratumv_exporter/`): three new
files.

- `net_client.py` — pure-stdlib `BridgeClient` class. Blocking
  connect, background reader thread, mutex-protected state cache,
  `send_move_self` / `send_ping` / `get_hello` / `get_entities`
  accessors. Also exposes module-level `build_move_self()` for
  scripting and a `send_move_once()` one-shot helper.
- `live_link.py` — UI panel + operator classes (registered only
  when running inside Blender, gated on a `_HAVE_BPY` import
  probe so the module can also be imported under plain CPython).
  Connect / Disconnect operators, a status box showing the cached
  Hello, a live list of replicated entities, and a
  `bpy.app.handlers.depsgraph_update_post` hook that watches a
  designated `StratumV.Avatar` object and pushes a MoveSelf frame
  whenever its transform changes. Entities are mirrored into
  Blender as empties in a `StratumV` collection via a
  `bpy.app.timers` pump running every 100 ms.
- `__init__.py` — bumped to `1.1.0` and registers the new classes
  alongside the existing scene-export panel.

The addon uses the existing `coord_convert.py` Blender-Z-up →
StratumV-Y-up helpers: Blender's `matrix_world` decomposes to TRS
and each component flows through `position_to_engine` /
`quaternion_to_engine` / `scale_to_engine` before the wire encode.

**Headless proxy test** (`tools/blender/test_live_link.py`): stands
in for a real Blender instance when the rig runs without a UI.
Opens a `BridgeClient`, retries until a welcomed Hello with a
non-zero avatar arrives, prints the cached world, then walks the
bridge's avatar in a short diagonal burst of MoveSelf frames. Used
by the visual-checkpoint rig and as a smoke test during the dev
loop.

**Capture**: composite of two `skinned_test` windows, both rendering
the same replicated world. Left is the bridge (client 1) after the
Python proxy has driven it diagonally — cyan "Client1 (you)" dot
at upper-right, magenta "Client2" dot near centre, orange cube.
Right is the observer (client 2) — cyan "Client2 (you)" dot near
centre, magenta "Client1" dot at upper-right (mirroring the
moved avatar), orange cube. The cube orbits authoritatively on
the server; the two views are separated by ~50 ms so they show
slightly different cube phases. Bridge panel reads `Bridge: :9401
clients=0`, `Bridge rx/tx: 5 / 1144`, `Blender edits applied: 5`
— the 5 matches the proxy's 5-frame move burst exactly.

The capture rig launches
stratumv_server + observer + bridge back-to-back (critical — the
lab harness `--auto-exit` path cascades a Despawn when a client
disconnects, so capturing before either client reaches
`--capture-frame` is the only way to see the moved other-client
avatar on both sides), then runs the proxy in retry mode so the
connect races ~15 s of lab-harness init on a cold cache.

**Explicitly out of scope at this stage** (deferred to later work):

- No asset push from Blender — the full `Announce/Chunk` upload path
  via the addon lands in §9.5. At this stage, dragging
  a GLB into Blender does NOT auto-replicate into the engine.
- No light, camera, material, or hierarchy sync. Only
  `NetTransform` flows (the existing component).
- No per-Blender-client identity on the server — a Blender edit
  is attributed to its paired skinned_test bridge client.
- No real Blender instance in CI — the headless proxy tests the
  TCP + translator + server path; the addon's `bpy` code path is
  validated by running Blender locally and capturing
  a `1 Blender + 2 skinned_test` three-way scene.
- No C++ integration tests — the new test surface is all Python
  (ast parse check on the three addon files) plus the manual
  end-to-end rig. Unit tests for EditorBridge's wire codec are
  covered by the existing `test_ReplicationWire.cpp` suite which
  already exercises `writeGenericSetFieldPayload` — the bridge is
  pure-logic glue on top.
- No WAN / firewall handling. Bridge binds to 127.0.0.1 only.
- No authentication — a local TCP client on loopback inherits the
  paired skinned_test's scope automatically. For an eventual
  WAN-capable mode a per-Blender identity + password / SSO flow
  is required.
- No BaseSystemContext shape change. `NetworkContext` stays the
  same 5 slots from 1.3.4.

---

### 9.5 Blender asset push + hierarchy sync (1.3.7 → 1.3.8)

This slice extended the Blender live-link bridge with asset push
from Blender and scene-hierarchy sync via a new `ParentLink`
replicated component.

**Design decisions**

- **Asset protocol: explicit new bridge wire types**, not byte-pump
  passthrough of the existing QUIC asset messages. The bridge has
  enough state per-asset (chunk assembly + local CAS pinning + UI
  counters) that a passthrough would push the same work into
  Python. Three new upstream message bytes in the bridge protocol:
  `kBridgeMsgAssetAnnounce = 0x83`, `kBridgeMsgAssetChunk = 0x84`,
  `kBridgeMsgSetParent = 0x86`. The asset Announce / Chunk body
  layout mirrors the existing `kFrameAssetAnnounce` /
  `kFrameAssetChunk` QUIC bodies byte-for-byte so the Python
  codec and the C++ `AssetUploadClient` share a single layout
  spec.

- **Scene hierarchy: new `ParentLink` SV_REPLICATE'd component**,
  not a new `parent` field on NetTransform. Parenting is a
  relationship, not part of the transform. Merging it into
  NetTransform would bump NetTransform's schemaVersion, invalidate
  every pre-existing `world.svbin` file, and grow every
  entity's datagram snapshot by 4 bytes whether or not it has a
  parent. ParentLink is **SetField-only on the wire** — never part
  of Spawn payloads — so the persistence file format stays byte-
  identical across the 1.3.7 → 1.3.8 bump.

**ParentLink component**

- Single field: `uint32_t parentEntityId` (0 = unparented, root-
  level).
- `Authority::Owner` — a client may only re-parent entities it
  owns, matching the NetTransform owner-authority contract.
- Wire size inside the EditTransaction payload with the single
  field marked dirty: 2 bytes schemaVersion + 1 byte packed dirty
  mask + 4 bytes u32 = 7 bytes total.
- Lives in `cmake/stratumv_core_sources.cmake` so the Linux
  headless-server carve-out compiles it for the schema-handshake
  path without pulling graphics deps.

**Anchor quirk — future non-Server authority components must follow
the same pattern**

The `ensureParentLinkRegistered()` force-link anchor in
`ParentLink.cpp` must mirror the full `SV_COMPONENT_AUTHORITY` macro
behaviour — both the builder call AND an explicit `setAuthority`
patch:

```cpp
const ReplicationMeta& ensureParentLinkRegistered() {
    const ReplicationMeta& meta =
        sv_buildReplicationMetaFor(static_cast<ParentLink*>(nullptr));
    ReplicationRegistry::get().setAuthority("ParentLink", Authority::Owner);
    return meta;
}
```

Reason: `ReplicationRegistry::registerType` overwrites the stored
meta on every re-registration (`existing->second = std::move(meta)`),
which wipes any authority patch from a prior `SV_COMPONENT_AUTHORITY`
static init. For NetTransform this is harmless because its default
authority is Server, so overwriting with default Server is a no-op.
For ParentLink, the intended Owner tag has to be re-applied on every
anchor call. Any future non-Server-authority component — for example
a `LightComponent` with `Authority::Editor` — will need the same
two-step anchor.

**Server SetField dispatch generalisation**

`src/stratumv_server/main.cpp`'s `applyClientTransaction` SetField
path used to hardcode
`tx.typeNameHash != netTransformMeta.typeNameHash` as a rejection
check. It's now a dispatch switch:

- `tx.typeNameHash == netTransformMeta.typeNameHash` — the existing
  full path: decode via `readGenericSetFieldPayload`, push a
  `UndoEntry` onto `world.undoLog`, write the new state to
  `ReplicatedEntity::transform`, `broadcastTransaction(tx, ...)`.
- `tx.typeNameHash == parentLinkMeta.typeNameHash` — a new shorter
  path: decode the single u32 into `ReplicatedEntity::parent`
  (which is a new inlined sidecar field on `ReplicatedEntity`),
  `broadcastTransaction(tx, ...)`. No undo-log entry — ParentLink
  changes are rare and undo for them is deferred.
- Unknown typeNameHash — WARN log and drop.

`drainClientInbox` + `applyClientTransaction` grew a new
`parentLinkMeta` param pulled from a new startup pre-flight
`ensureParentLinkRegistered()` block that also authority-checks
the returned meta.

`ReplicatedEntity` gains `sv::ParentLink parent;` as an inlined
sidecar field. A fully generic per-entity component map
(`unordered_map<typeHash, bytes>`) is a future refactor
when ≥3 components ship.

**Persistence not touched**

`WorldPersistence` still only serialises NetTransform. Parent
relationships do NOT survive server restart — a documented
limitation. Future work that wants this can either (a) emit a
second reliable-stream SetField(ParentLink) after each
makeSpawnTransaction during join-with-snapshot replay, or (b) bump
the `SVWLD001` file format version to v2 and add a per-entity
parent field.

**Bridge wire types**

`src/engine/net/EditorBridge.{h,cpp}`:

- New message bytes `kBridgeMsgAssetAnnounce = 0x83` /
  `kBridgeMsgAssetChunk = 0x84` / `kBridgeMsgSetParent = 0x86`.
- New `EditorBridgeAsset` / `EditorBridgeParentChange` POD types.
- New `drainAssets()` / `drainParentChanges()` main-thread pull
  methods, symmetric to the existing `drainMoves()`.
- Per-hash `sv::AssetReceiver` state machine under a new
  `m_assetsMu`. The reader thread assembles inbound Announce +
  Chunks into a `std::vector<uint8_t>`, verifies the declared
  SHA-256 on completion, and only then moves the payload onto
  `m_pendingAssets`. Partial assemblies never reach the main
  thread.
- `kMaxFrameBytes` bumped 64 KiB → 256 KiB. A 64 KiB chunk payload
  + 44-byte chunk header + 1-byte msgType + 4-byte outer length
  prefix totals 65605 bytes, which overflows the old cap. The new
  cap is documented with a C++ `static_assert` in
  `test_EditorBridgeWire.cpp` so a future shrink fails the build.
- `m_assetsReceived` / `m_parentChanges` atomic counters on the
  bridge for observability.

**Lab harness integration**

`lab/skinned_test/main.cpp`:

- `ClientEntity` grows a `sv::ParentLink parent;` sidecar.
- `drainNetReliableInbox` SetField case dispatches on
  `tx.typeNameHash`: NetTransform stays a no-op (the datagram path
  carries state); ParentLink decodes via
  `readGenericSetFieldPayload` into the local entity's parent
  field and bumps `m_bridgeParentChanges`.
- New `pumpBridgeAssets()` + `pumpBridgeParents()` main-thread
  drains (symmetric to `pumpBridgeMoves()`).
- `uploadAssetFromDisk` factored into a new
  `uploadAssetBytes(relPath, kind, data, size)` helper so the
  bridge asset drain shares the same in-memory upload path as the
  existing "Upload to server" button.
- Network Demo panel grows `Bridge assets: N recv / N uploaded`
  and `Bridge parents: N applied` counter lines + a new
  `Parented entities: N` block with bullet list showing
  `#entityId -> #parentId` pairs.
- `ensureParentLinkRegistered()` called from `main()` alongside
  `ensureNetTransformRegistered()`.

**Blender addon**

`tools/blender/stratumv_exporter/net_client.py`:

- `hashlib.sha256` hashing + `build_asset_announce(digest,
  byte_size, kind, name)` / `build_asset_chunks(digest, data,
  chunk_size=65536)` / `build_set_parent(parent_id)` free-function
  wire-codec helpers.
- `BridgeClient.send_asset(path, kind, rel)` (reads file, hashes,
  ships Announce + Chunks under the shared send lock so they stay
  adjacent on the wire) + `send_asset_bytes(data, kind, rel)` (in-
  memory variant used by the Blender operator after the gltf
  export + tempfile read + unlink cycle) + `send_set_parent(id)`.
- `assets_pushed` / `parents_sent` observability counters.
- `MAX_FRAME_BYTES = 256 * 1024` matches the C++ cap.
- `ASSET_KIND_*` constants mirroring `sv::AssetKind`.
- Still zero non-stdlib dependencies — `hashlib` is Python 3.11
  stdlib.

`tools/blender/stratumv_exporter/live_link.py`:

- New `STRATUMV_OT_link_push_asset` operator. On click: saves the
  current selection, force-selects the active object, runs
  `bpy.ops.export_scene.gltf(filepath=<tempfile>.glb,
  use_selection=True, export_format="GLB", export_apply=True)`,
  reads the bytes, calls `BridgeClient.send_asset_bytes`, then
  restores the selection in a `finally` block. The temp file is
  removed after the send completes.
- Sidebar panel grows a new "Assets" section with the Push Asset
  button (disabled when no active object or bridge is down) + an
  `Assets pushed: N  Parents sent: N` counter row under the Bridge
  Identity block.
- Depsgraph hook gains a parent watcher: walks `obj.parent` on the
  `StratumV.Avatar` object, resolves to a replicated entityId via
  the mirrored `StratumV.<id>.<label>` object-name prefix (new
  `_entity_id_from_object_name()` helper), and calls
  `BridgeClient.send_set_parent(entity_id)` when the parent
  changes from the cached `_last_parent_entity`. Falls back to
  `send_set_parent(0)` when the Blender parent is None or does not
  map to a replicated entity.

`tools/blender/stratumv_exporter/__init__.py`:

- `bl_info.version` bumped `(1, 1, 0)` → `(1, 2, 0)`.

**Headless proxy**

`tools/blender/test_live_link.py` gains new
`--push-asset PATH` / `--push-asset-name REL` / `--push-asset-kind
N` / `--set-parent ENTITY_ID` CLI flags. `--dry-run` now still
fires the asset push + SetParent if those flags are set — it only
skips the MoveSelf burst. The proxy uses the exact same
`net_client.py` module the real addon runs, so its wire path is
byte-identical to a real Blender addon run.

**Tests**

- 6 `[parentlink]` cases in `tests/test_EditTransaction.cpp`:
  registration round-trip, full-mask SetField payload encode/
  decode, zero-mask preserves existing state, end-to-end via full
  `encodeEditTransaction`/`parseEditTransaction`, wrong-
  typeNameHash rejection, `ensureParentLinkRegistered()`
  idempotence. Runs in both `sv_core_tests` and `sv_tests` via the
  core-subset inclusion.
- 9 `[bridgewire]` cases in a new `tests/test_EditorBridgeWire.cpp`:
  msgtype constant checks, Announce body layout byte-by-byte,
  Chunk body layout byte-by-byte, SetParent body layout, single-
  chunk + multi-chunk `AssetReceiver` assembly round-trip with
  byte-exact payload comparison, hash-mismatch rejection, and a
  `static_assert` proving 64 KiB chunk payloads fit inside the
  new 256 KiB `kMaxFrameBytes`. Runs only in `sv_tests` (full
  build) because `EditorBridge.h` pulls in `<winsock2.h>` through
  its implementation and is not in the `stratumv_core` carve-out.
- Full build **367/367 → 382/382** (+15).
- Core-only **152/152 → 158/158** (+6).

**Capture**

A 2×2 zoomed composite from a single rig run. Note: this is not a
live Blender instance — the headless proxy drove the bridge
instead. The proxy runs the exact same `net_client.py` module the
real addon runs, so the wire path is byte-identical.

Top row (Client A — bridge, proxy-driven) shows `Schema: OK
(server 1.3.8, 2 types)`, `Bridge assets: 1 recv / 1 uploaded`,
`Bridge parents: 1 applied`, `Parented entities: 1  #100 -> #1`,
and a Replicated Assets row `[texture] push/grass_albedo.png
(88 B, a9c21...)` tagged `[uploaded]`.

Bottom row (Client B — observer) shows the same schema handshake,
no Bridge line, the same `#100 -> #1` parent bullet (proving the
ParentLink echo reached the second client via the server
broadcast), and a Replicated Assets row tagged `[from server]`
(proving the asset replicated end-to-end: proxy → bridge → upload
→ QUIC server → broadcast → observer).

**Explicitly out of scope at this stage**

- No light ECS work.
- No camera parameter sync.
- No material parameter sync.
- No `sv_`-prefixed custom-property mapping.
- No BaseSystemContext shape change.
- No persistence of parent relationships across server restart.
- No ParentLink undo/redo.
- No acyclic parenting check.
- No server-side CAS eviction / GC.
- No consumer-game fixes (downstream games still need the
  `bindings.network = ...` rename).

**Semver**: 1.3.7 → 1.3.8 (patch). `BaseSystemContext` shape
unchanged.

---

## 10. Related Docs

- **`NETWORK_DESIGN.md`** — Transport (MsQuic), authoritative server,
  determinism, scale target. Collab editing runs on the same transport.
- **`REPLICATION_CONTRACT.md`** — The `SV_REPLICATE` macro and
  `Authority::Editor` enum value. Collab editing is the primary consumer
  of `Authority::Editor`.
- **`PLUGIN_CONTRACT.md`** — DLL boundary rules. Transaction log lives
  in the engine, but transaction kinds are often defined by game DLLs.
- **`ASSET_PIPELINE.md`** — Asset import and the `AssetBrowser` /
  `ThumbnailCache` modules that asset sync builds on.
- **`ARCHITECTURE.md`** — Layer model. `TransactionLog` and
  `AssetSyncEngine` are Layer 4 services that depend on the Layer 4
  `ReplicationRegistry`.

---

## 11. Research Sources

- Unreal Multi-User Editing (UE 5.7):
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/multi-user-editing-overview-for-unreal-engine>
- Resonite architecture overview:
  <https://wiki.resonite.com/Architecture_Overview>
- Resonite data model synchronization:
  <https://wiki.resonite.com/index.php?title=Data_model_synchronization>
- Automerge (considered and rejected for v1):
  <https://automerge.org/>
- Figma collaborative editing (reference for multi-user UX patterns):
  <https://www.figma.com/blog/how-figmas-multiplayer-technology-works/>
