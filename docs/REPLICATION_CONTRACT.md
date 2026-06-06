# REPLICATION_CONTRACT — The SV_REPLICATE Macro

> Decision-layer companion to `ARCHITECTURE.md` and `NETWORK_DESIGN.md`.
> This doc specifies the contract between game code and the engine's
> replication system: what `SV_REPLICATE` means, what `Authority` does,
> what each component author is agreeing to when they opt in. For the
> networking transport see `NETWORK_DESIGN.md`; for collaborative editing
> semantics on top see `COLLAB_EDITING.md`; for DLL boundary rules
> see `PLUGIN_CONTRACT.md`.

The replication contract is the load-bearing primitive for every network-
aware feature in StratumV. The reflection registry provides field metadata,
the `SV_REPLICATE` macro, and the `DirtyMask`; authority metadata and the
scalar snapshot encoder/decoder layer on top
(`SV_COMPONENT_AUTHORITY` + `Authority` enum + `SnapshotWriter` /
`SnapshotReader` + `encodeSnapshot` / `decodeSnapshot`); the transport
binding connects the encoder output to MsQuic; and the whole thing is the
substrate that collaborative editing is built on.

Every decision below traces back to the same two constraints that drove
`NETWORK_DESIGN.md`:

1. **Game components must opt in per-field, not wholesale.** A `Transform`
   component is replicated; the `VkMesh*` pointer it holds is not. A
   player inventory is replicated; the per-frame input scratch buffer is
   not. Opt-in is the only discipline that keeps replication bandwidth
   bounded and security boundaries clear.
2. **Games must not be able to accidentally replicate something that
   crashes across the wire.** Pointers, raw handles, PIMPL opaques, and
   unowned references are not serializable. The contract has to make
   this obvious at the callsite, not as a debugging surprise.

---

## 1. The Macro at a Glance

### 1.1 Minimal example

```cpp
// game-side, in your game
struct TransformComponent {
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;
    float     lastMoveTime;  // client-only cache; NOT replicated
};

// Registration happens at namespace scope (not inside the struct
// body). Placing it adjacent to the type — or in a neighboring .cpp —
// is the convention.
SV_REPLICATE(TransformComponent,
    SV_FIELD(position),
    SV_FIELD(rotation),
    SV_FIELD(scale)
);

SV_COMPONENT_AUTHORITY(TransformComponent, sv::Authority::Server);
```

That's the whole surface. The game declares which fields flow across the
wire and who has the right to mutate the component. Everything else —
registering with the engine, generating snapshot/delta metadata, hooking
into the lifetime of an EnTT entity — is handled by template/macro
machinery.

**Why namespace scope, not inside the struct?** Putting `SV_REPLICATE`
inside the class body would require the macro to synthesize a
`static inline` class-scope constant whose initializer reaches across
multiple TUs, and to handle the interaction with forward declarations.
Keeping it at namespace scope — next to the struct definition — lets
the macro emit a plain inline free function plus a file-scope static
trigger, which is much easier to reason about and matches how
`SV_COMPONENT_AUTHORITY` is already spelled.

### 1.2 What the macro generates

`SV_REPLICATE(Type, fields...)` expands to:

1. A `sv::ReplicationMeta` descriptor (live in the
   `ReplicationRegistry` singleton) with field offsets, sizes, type
   tags, name hashes, dirty-bit positions, optional quantization
   hints, and a 16-bit schema version hash derived from the field
   name+type sequence.
2. An inline free function
   `sv_buildReplicationMetaFor(Type*)` that rebuilds and re-registers
   the metadata on every call. Test code calls this directly after
   `ReplicationRegistry::resetForTests()` to repopulate the registry
   in a deterministic order.
3. A file-scope `static const bool` whose initializer invokes (2)
   once at program start so the metadata is in the registry before
   the (future) runtime spins up.

The type ADL lookup of `sv_buildReplicationMetaFor(Type*)` means
qualified component types (e.g. `mygame::TransformComponent`)
work as long as the `SV_REPLICATE` macro is invoked in the same
namespace as the struct — the emitted function lives in that
namespace by construction.

### 1.2.1 Snapshot primitives

`SV_REPLICATE` itself is NOT extended with per-type serialize/
deserialize free functions. Instead a pair of generic
byte-buffer primitives (`SnapshotWriter` / `SnapshotReader`) plus
two free functions walk the reflected metadata at runtime:

```cpp
bool sv::encodeSnapshot(const ReplicationMeta& meta,
                        const void*            instance,
                        const DirtyMask&       mask,
                        SnapshotWriter&        out);

bool sv::decodeSnapshot(const ReplicationMeta& meta,
                        void*                  instance,
                        DirtyMask&             outMask,
                        SnapshotReader&        in);
```

Walking `ReplicationMeta::fields` at runtime rather than code-genning
per-type free functions:
- Avoids bloating every `SV_REPLICATE` expansion with two full
  encoders (saves binary size across every consumer game).
- Makes it trivial to add field types to the encoder without
  retroactively regenerating per-type stubs.
- Matches how the game-side EnTT runtime will want to drive
  replication — "walk the registry, call one function per component
  type" — without the extra indirection of a per-type vtable.

Delta encoding (`diff` / `patch`) is not a separate code path. The
snapshot encoder IS the delta encoder: a "delta" is a snapshot whose
DirtyMask contains only the fields that changed since the last ack.
The scalar snapshot path is the foundation; the last-acked-ring is
layered on top by the transport.

### 1.3 What the macro does **not** generate

- **Threading.** The macro is pure data description. All calls happen on
  the server tick thread (or the client apply thread) owned by the
  replication runtime. Game code does not synchronize.
- **Network I/O.** The macro does not touch MsQuic, file I/O, or any
  system resources. It describes fields. The runtime does the sending.
- **EnTT hooks.** The macro does not `emplace`, `remove`, or observe EnTT
  state. The runtime does that via EnTT's own observer APIs based on what
  the macro registered.
- **Version bumping.** The macro does not bump the StratumV semver.
  Schema versions are per-component FNV-1a hashes over the field
  declaration sequence and are computed automatically inside
  `svRegisterReplicatedType`; the engine semver only changes when the
  DLL boundary shape changes (e.g. bundling the network
  pointers into a sub-struct bumped 1.2.x → 1.3.0).

Keeping the macro dumb is a feature. Dumb macros are easy to audit.

---

## 2. Authority

### 2.1 The enum

```cpp
namespace sv {
enum class Authority : uint8_t {
    Server = 0,   // authoritative server process only
    Owner  = 1,   // the owning client (e.g. a player's own input state)
    Editor = 2,   // any connected client with Editor-scope permission
    None   = 3    // not replicated; presence of the field is a mistake
};
}
```

Four values, one byte, and the uint8_t storage is deliberate: the
authority tag is packed into the snapshot stream per component type.

### 2.2 What each value means

**`Authority::Server`** — the default for almost everything gameplay-
related. Only the dedicated server can change these fields. Any inbound
mutation from a client that carries a `Server` authority field is dropped
with a log warning (and is a cheat attempt in production). Examples:

- Player health, stamina, position-as-authoritative (after reconciliation)
- AI agent state
- Inventory contents
- World item placements
- Weather state
- Server tick counters

**`Authority::Owner`** — the owning client is the authority. Used for
client-side predicted state that the server will validate after the fact.
Examples:

- Player input intent (WASD pressed, mouse delta, button states)
- Camera yaw/pitch for look direction
- Predicted player position during the prediction window
- UI state that only matters to the local player

Owner-authoritative fields still flow through the server — the client
sends the intent, the server receives it, validates it, and relays it to
other clients. The server is still the broadcast hub; "Owner" just means
"this client gets to declare the value and the server trusts it unless
the validator says otherwise."

**`Authority::Editor`** — any connected client that holds the Editor
permission scope (see `COLLAB_EDITING.md`) can mutate the field. Used for
live collaborative editing of world data. Examples:

- Placed entity transforms during editor sessions
- Material parameter overrides
- Light color/intensity
- Terrain edits (per-chunk, but the chunk component is editor-authoritative)

Editor mutations are wrapped in edit transactions (see
`COLLAB_EDITING.md`) so they can be undone and logged. The replication
runtime delivers them to all other clients the same way Server updates
flow — it just accepts the writer's identity differently.

**`Authority::None`** — explicitly not replicated. The value exists on
this enum so that a component author can declare "I looked at this field
and decided it doesn't belong on the wire," which is a different statement
than "I forgot to add this to SV_REPLICATE." A future tool can scan
components and warn about fields that are neither listed in SV_REPLICATE
nor marked `None`; components that don't declare authority at all are
errors.

### 2.3 Why not just booleans

An earlier draft used `bool replicated` + `bool ownerWritable` + `bool
editorWritable`. Three booleans means 8 combinations, most of which are
nonsense (`replicated=false editorWritable=true` — what?). The enum is 4
semantically meaningful states with a single tag. Simpler to read,
impossible to mis-spell.

### 2.4 Authority is per-component, not per-field

`SV_COMPONENT_AUTHORITY(TransformComponent, Authority::Server)` sets the
authority for the whole component. A component with mixed authority
(some fields server-authoritative, some owner-predicted) must be split
into two components at the ECS level — for example, `PlayerInputState`
(Owner) and `PlayerPhysicsState` (Server).

This is a design constraint, not a limitation. Mixing authority in a
single component makes it hard to reason about who can touch what, which
is already the hardest part of networked gameplay. Forcing a split keeps
the rules readable.

---

## 3. Replicatable Field Types

### 3.1 What works out of the box

| Type family | Example | Encoding |
|-------------|---------|----------|
| Fixed-size integers | `int32_t`, `uint16_t`, `uint64_t` | Direct bit-packed. |
| Floats | `float`, `double` | Direct, with optional quantization hint. |
| Fixed-size arrays | `std::array<T, N>` | Direct, if T is replicatable. |
| `glm::vec2/3/4`, `glm::quat` | Transform components | Direct. Quaternions optionally normalized. |
| Enums backed by integer types | Any `enum class : uint8_t` | Direct, tagged as the underlying. |
| Fixed strings | `sv::FixedString<N>` | Length-prefixed up to N bytes. |
| ECS entity handles | `entt::entity` | Remapped client-side (see 3.3). |

These cover approximately 90% of the gameplay field surface.

### 3.2 What works with an opt-in adapter

| Type family | Example | How |
|-------------|---------|-----|
| Variable-length strings | `std::string` | `SV_FIELD(name, sv::StringField<256>)` — declares a max size for bandwidth budgeting. |
| Variable-length arrays | `std::vector<T>` | `SV_FIELD(slots, sv::VectorField<T, 32>)` — max count is declared. |
| Bitsets | `std::bitset<N>` | Direct if N ≤ 256. |
| Owned resources | `std::unique_ptr<T>` where T is replicatable | `sv::OwnedField<T>` — wraps nullability. |

All of these require an explicit adapter wrapper because they have variable
wire costs that the bandwidth budget needs to know about. "This string is
256 bytes max" is metadata. "This string is a `std::string`, good luck"
is a footgun.

### 3.3 Entity handle remapping

EnTT entity handles are 32-bit IDs that are valid only in the registry
that created them. Server entity 42 is not the same as client entity 42.
The replication runtime maintains a per-connection `serverEnt → localEnt`
map and rewrites entity references during snapshot apply. Games don't
see this — they write `entt::entity target` in their component, call
`SV_FIELD(target)`, and the runtime handles the translation.

The same mechanism handles entity lifetime: when the server destroys an
entity, the runtime queues the remapping entry for removal after any
referring snapshots drain.

### 3.4 What explicitly does not work

| Type family | Why it's banned | What to do instead |
|-------------|-----------------|---------------------|
| Raw pointers (`T*`) | Points into process memory; meaningless on the remote end. | Use an entity handle, or a stable ID, or a resource hash. |
| `std::shared_ptr<T>` | Same reason. Ownership doesn't cross process boundaries. | Same. |
| Vulkan handles (`VkBuffer`, `VkImage`) | Driver-local handles. | These don't belong in replicated components at all — they're render-layer, not gameplay. |
| Jolt body handles | Server-local. | Store the gameplay-side entity handle; let the server authority resolve to a body. |
| `std::function`, `std::any`, `std::variant` without an adapter | No stable wire format. | Refactor to a plain-old-data discriminated union, or split into separate components. |
| File paths as `std::string` | Machine-local; may leak PII. | Asset hash (content-addressable). |
| `void*` | No. | No. |

The macro rejects unsupported types at compile time via concept checks, so
the error shows up at the `SV_FIELD` callsite, not as a runtime misencode.

---

## 4. Dirty Bits and Delta Encoding

### 4.1 Per-field dirty bits

Each replicated field in a component gets a bit in a per-component
`sv::DirtyMask`. The mask lives alongside the component instance (one
mask per entity per replicated component type — typically a sibling
ECS component, or a flat array on the replication system) and is
owned by game code, not the registry. When game code mutates a
field, it signals the mutation via `DirtyMask::set` with the bit
index returned by `SV_DIRTY`:

```cpp
void MovementSystem::tick(entt::registry& reg, float dt) {
    reg.view<TransformComponent, sv::DirtyMask>().each(
        [&](entt::entity e, TransformComponent& t, sv::DirtyMask& mask) {
            t.position.x += 1.0f * dt;
            mask.set(SV_DIRTY(TransformComponent, position));
        });
}
```

`SV_DIRTY(Type, field)` looks up the dirty-bit index in the registry
via `ReplicationMeta::findField` — it returns the same index the
macro assigned at registration time, in declaration order. The
`DirtyMask::set` call is a plain OR into either a `uint64_t` (for
components with ≤64 fields) or a heap-allocated overflow bitset
(for larger components).

The snapshot generator walks the dirty set each tick and serializes
only the fields that changed since the last ack. This walk is
`sv::encodeSnapshot(meta, instance, mask, writer)` —
the writer's buffer then flows to the transport.

The atomic-OR detail is a future
optimization: the `DirtyMask` is not thread-safe. Gameplay
systems today touch components from a single tick thread, so plain
bitwise OR is sufficient. When multi-threaded systems start mutating
replicated state concurrently, the mask will be upgraded to
`std::atomic<uint64_t>` + CAS on the overflow path.

### 4.2 Alternative considered: auto-dirty via write observers

EnTT has `on_update` observers that fire whenever a component is mutated.
We could auto-mark dirty without the `markDirty` call. We chose not to,
for three reasons:

1. **Over-dirty.** An observer fires on any write, including writes that
   didn't actually change the value, and including writes by the replication
   runtime itself during apply. Bandwidth bloat.
2. **Hot path cost.** `on_update` triggers every time the component is
   `patch`ed. Hot gameplay loops touch transforms multiple times per frame
   per entity. Firing an observer every time is cheap but not free.
3. **Explicit-is-better.** `sv::markDirty` at the mutation callsite makes
   replication cost visible to the author. Invisible replication is the
   kind of thing that regresses silently.

The cost is that gameplay code has to remember to call `markDirty`. A
static analyzer / clang-tidy check that flags writes to replicated fields
without an accompanying `markDirty` is on the deferred list — once the
churn settles and the surface is stable, the check becomes worthwhile.

### 4.3 Delta encoding format

The snapshot is a per-entity bitfield of "which fields changed" followed
by the new values of only those fields. The per-entity wire layout
(once entities are on the transport) is:

```
[entity handle : varint]
[component     : per-component snapshot (see §4.5 for the layout)]
```

The per-component block is the schemaVersion +
byte-packed dirty mask + dirty field values. Entity handles are
remapped by the per-connection map described in §3.3; they are NOT
part of the component encoder and are written by the transport
layer on the outside of each component block.

Integer fields are bit-packed (varint for unsigned, zigzag for signed).
Quantized floats use their quantization hint. Quaternions use
smallest-three encoding when the `Quat` type lands in the
encoder extension.

The previous-state reference for delta encoding is the last snapshot the
client acked. QUIC gives us reliable ack on the reliable stream it runs
alongside the snapshot stream, so the server can keep a ring of the last
N unacked snapshots per client and compute deltas against them. `N ≈ 8`
is the working target — roughly 250 ms at 30 Hz, which is longer than any
ack round-trip on a healthy connection. A "delta" is simply a
snapshot whose DirtyMask contains only the per-field-changed bits; the
last-acked ring is a transport-layer addition on top.

### 4.4 Quantization hints

```cpp
SV_FIELD_QUANT(position, 0.001f)  // 1 mm precision
SV_FIELD_QUANT(yaw,      0.01f)   // 0.6° precision (in radians)
SV_FIELD_QUANT(health,   1.0f)    // integer HP
```

Quantization converts a float to a fixed-point representation for
wire transmission. The game reads a `float` in its component struct;
the wire carries a zigzag-varint-encoded integer that is `round(v/step)`.
On decode the reader recovers the original as `int * step`, with a
round-trip error bounded by half-step.

**Encoder dispatch.** `encodeSnapshot` reads `FieldDesc::quantStep`
on each `FieldType::Float`:
- `quantStep == 0.0f` → four raw IEEE-754 bytes (little-endian)
- `quantStep >  0.0f` → `writeFloatQuantized(v, step)`, which scales
  by `1/step`, rounds to nearest via `lrintf`, and emits a zigzag +
  varint pair. Small magnitudes like a ±10 m position at 1 mm
  precision cost 2–3 bytes instead of 4.

Games don't have to use quantization. A `SV_FIELD(position)` without
the `_QUANT` variant records `quantStep = 0.0f` and the encoder sends
the raw float bits. Turning on quantization is a bandwidth
optimization for known-precision data.

An earlier draft proposed a compile-time template wrapper
(`sv::Quantized<float, 0.001f>`) so encoders could dispatch without
runtime branches. The plain-step form is simpler, keeps the
`FieldDesc` POD, and has zero measurable branch cost in the
encoder benchmarks (the switch is already dispatching on `FieldType`).
A template variant remains open as a later optimization if profiler
evidence demands it.

### 4.5 Wire format

The byte layout emitted by `encodeSnapshot` per component is:

```
[u16       : meta.schemaVersion]     -- little-endian
[byteMask  : ceil(fieldCount / 8) bytes, LSB-first within each byte]
[for each dirty field in declaration order:]
  [field encoding dispatched on FieldDesc::type]
```

Per-field scalar encodings:

| `FieldType` | Encoding                                  | Typical width |
|-------------|-------------------------------------------|---------------|
| `Bool`      | 1 byte (0 or 1)                           | 1             |
| `Int8/16/32`| zigzag + LEB128 varint                    | 1–5           |
| `Int64`     | zigzag + LEB128 varint                    | 1–10          |
| `UInt8/16/32`| LEB128 varint                            | 1–5           |
| `UInt64`    | LEB128 varint                             | 1–10          |
| `Float`     | 4 raw bytes, OR zigzag-varint if `quantStep > 0` | 4 / 1–10 |
| `Double`    | 8 raw bytes                               | 8             |

Composite types (`Vec2/3/4`, `Quat`, `EntityHandle`, `StringFixed`,
`Enum`, `Blob`) are not in the scalar switch. Consumers that need
them today roll their own `FieldType` specializations and extend the
encoder branch in their own TU. Baseline `glm` + entity remap support
lands alongside the transport when there are entities to remap *into*.

Delta encoding is not a separate wire format. A "delta" is a
snapshot whose DirtyMask contains only the fields that changed since
the last ack. Same encoder, same decoder — the transport adds the
per-client last-acked-snapshot ring that lets the server decide
which bits go into the next mask.

### 4.6 Transport framing

`encodeSnapshot` produces the component bytes. The transport layer adds
the tiny datagram header that lets both ends agree on *which* entity +
component the bytes describe. The full wire layout, datagram-granular, is:

```
Offset  Size   Field
------  -----  ------------------------------------------------------
0       1      msgType        (u8)   kFrameSnapshot = 1
1       4      tickIndex      (u32)  server-monotonic tick counter
5       4      entityId       (u32)  caller-defined entity id
9       4      typeNameHash   (u32)  ReplicationMeta::typeNameHash (FNV-1a)
13      2      payloadLen     (u16)  encodeSnapshot byte count
15      P      payload        (u8[]) §4.5 component encoding
```

Total header overhead: **15 bytes**. One datagram carries exactly one
(entity, component) pair. A `NetTransform` full-mask snapshot weighs
**46 bytes** on the wire (15 header + 31 payload — 2 schemaVersion +
1 byte-packed dirty mask + 7 raw floats × 4 bytes).

`sv::net::encodeSnapshotFrame` wraps `encodeSnapshot` with the header;
`sv::net::parseSnapshotFrame` returns a header view borrowing from the
caller's datagram buffer; `sv::net::applySnapshotFrame` looks the
component type up by `typeNameHash`, delegates to `decodeSnapshot`,
and returns the decoded `DirtyMask`.

**Design choices:**

- **One component per datagram** (not batched). Simpler wire format,
  simpler loss recovery — a lost datagram drops exactly one (entity,
  component) update, not a batch. Batching multiple updates per
  datagram is a later addition, once interest management + per-client
  bandwidth budgets land.
- **`entityId` on the wire even for single-entity sessions.** The
  minimal transport replicates one server-owned entity, but the header
  already carries the id so multiple entities can be replicated later
  without bumping the frame version.
- **`typeNameHash` on the wire.** Lets the client reject frames for
  unknown component types and look up the meta without a string
  compare. 32-bit collision risk is negligible with <1000 types.
- **`u16` payloadLen ceiling (65535 bytes).** Well above any single
  QUIC datagram (~1200–1450 bytes after MsQuic MTU probing). The
  `encodeSnapshotFrame` guard rejects component payloads that would
  overflow the field — the caller must split across multiple
  datagrams, which the single-component path never triggers.
- **Little-endian for every multi-byte header field.** Matches the
  component-level encoding in §4.5.

**Transport binding (sv::net::Connection):**

```cpp
sv::net::Connection& conn = /* from Transport::connect or Listener::acceptOne */;
std::vector<uint8_t> frameBytes;
sv::net::encodeSnapshotFrame(
    tickIndex, entityId, meta, &instance, dirtyMask, frameBytes);
conn.sendDatagram(frameBytes.data(), frameBytes.size());

// On the receive side, the datagram handler runs on an MsQuic
// worker thread and must be thread-safe — the usual pattern is to
// copy the bytes into a lock-protected inbox and drain on the main
// thread during the game tick.
conn.setDatagramHandler([](const uint8_t* data, size_t size) {
    auto frame = sv::net::parseSnapshotFrame(data, size);
    if (!frame) return;
    sv::DirtyMask mask;
    sv::net::applySnapshotFrame(*frame, &localComponent, mask);
});
```

**Datagram negotiation.** MsQuic automatically negotiates unreliable
datagrams when both peers set `QUIC_SETTINGS::DatagramReceiveEnabled`.
The Transport enables this for both server and client configurations
in `Transport::start`; no per-connection extra step is required. The
negotiated `MaxSendLength` surface via the
`QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED` event — the minimal
transport ignores it and relies on MsQuic to fail `DatagramSend` for
over-MTU payloads. A later budgeter will surface the limit into the
outbound path.

**Loss tolerance.** Datagrams are fire-and-forget — MsQuic does not
retransmit lost frames. The minimal server sends full-mask
snapshots every tick, so a dropped datagram is fully recovered by
the next one. When delta encoding + last-acked rings land,
the server will detect "client has not acked tick N" and re-send the
delta relative to an earlier baseline.

**What this framing does NOT cover** (all later additions):
- Reliable streams (edit transactions, join-with-snapshot, asset
  uploads) — those use `sv::net::Connection::openStream` + a
  length-prefixed message format defined by the collaborative-editing
  layer.
- Heartbeat / keep-alive — MsQuic's idle timeout handles this.
- Per-client interest management or priority queues.
- Schema version negotiation on first contact — the client simply
  drops frames whose embedded schemaVersion doesn't match its local
  meta. A proper handshake that exchanges schema hashes up front so
  clients can fail fast is a later addition.

---

## 5. Per-Component Opt-In Discipline

### 5.1 The rule

**Replicated components opt in explicitly. Non-replicated components
have no replication surface and pay no cost.**

A gameplay-relevant component that should flow across the wire gets
`SV_REPLICATE` and `SV_COMPONENT_AUTHORITY`. A rendering or cache
component (e.g. `MeshRenderComponent` holding a `VkMesh*`) gets neither,
and the runtime ignores it. There is no "default replicate everything"
switch.

### 5.2 Why this, instead of reflection everywhere

Three engines were studied as prior art:

- **Resonite** — every component field is automatically a replicated
  variable. The entire data model is the replication model. This is the
  principle StratumV stole and adapted.
- **Unreal** — UPROPERTY with `Replicated` flags. Per-property opt-in,
  similar to what StratumV is doing, but built on Unreal's
  macro+preprocessor reflection system.
- **Flecs / bevy_replicon** — ECS-native reflection with per-component
  registration. Cleaner than Unreal's macros, but tied to their specific
  reflection/registry APIs.

StratumV's approach is Unreal-shaped (per-field opt-in via macro) applied
to an EnTT-shaped registry. The reason it's per-field and not
per-component is that many components contain mixed data — for example, a
`CharacterStateComponent` might have replicated health and stamina
alongside a non-replicated `float timeInCurrentAnim` that only matters on
the authoritative server. Per-field opt-in means the wire cost is exactly
what the author declared and nothing else.

### 5.3 Why not codegen

An alternative approach: run a build-time tool (based on libclang, or a
Python AST parser, or similar) over the game's header files, extract the
annotated fields, and emit the replication descriptor as generated C++.
This is how Unreal Header Tool works.

We declined to do this for v1, for three reasons:

1. **Build system cost.** Codegen means adding a pre-build step to every
   consumer's CMake config, plus shipping a parser (libclang is 40 MB+) or
   writing a custom one (non-trivial for C++).
2. **Churn amplification.** Early replication code is going to be
   iterated on frequently. Macros iterate instantly; codegen has a
   rebuild cycle on every schema change.
3. **Macro-based opt-in is Good Enough.** Unreal ships a commercial
   engine on UPROPERTY macros. StratumV can too.

The codegen path remains open as a later optimization if the macro
surface grows hard to maintain. At that point it will look like
`tools/engine_ref.py`'s parser (which already walks C++ headers with brace
depth tracking, no libclang required) but emits a per-component
`ReplicationMeta_*.inc` header instead of a documentation file. Same
pattern, same style. This keeps the toolchain uniform.

### 5.4 Integration with `BaseSystemContext`

No field inside `BaseSystemContext` is replicated. The context holds
function pointers, Vulkan handles, and service instances — all of which
are meaningless off-process. The replication layer lives in a separate
Layer 4 module (`ReplicationRegistry`) that `BaseSystemContext.network`
points at via the nested `NetworkContext` sub-struct.

DLL plugins interact with replication through the game's own components
(which they include directly) and the context's network sub-struct (for
lifecycle hooks). The macro expansion happens at the point of component
definition in game code; the engine doesn't scan plugin binaries.

---

## 6. Lifecycle Hooks

### 6.1 Spawn + destroy

When an entity with at least one replicated component is created on the
server, the replication runtime emits a **spawn record** on the next
tick. The spawn record contains the entity ID, the list of replicated
components, and their full state (not a delta — there is no prior state
to delta against). Clients receive the spawn record and create a local
entity with the same components, remapping the entity handle via the
per-connection map from section 3.3.

When an entity is destroyed on the server, the runtime emits a **destroy
record** containing just the entity ID. Clients receive it and destroy
the matching local entity.

Spawn and destroy records flow on the reliable stream. Lost spawns or
destroys are session-breaking, so QUIC's reliable delivery is the right
tool.

### 6.2 Join-with-snapshot

A new client joining mid-session cannot start from a delta — it has no
prior state to delta against. It receives a full authoritative snapshot
on the reliable stream, containing spawn records for every currently-live
replicated entity, and transitions to the delta stream once the snapshot
is drained.

Join-with-snapshot is handled by the collaborative-editing layer. The
replication contract just says: a snapshot is a sequence of spawn records in the same
wire format as the tick stream uses. Same encoder, same decoder, different
source.

### 6.3 Reconciliation for Owner-authoritative fields

When a client sends an input intent (Owner authority), the server
receives it, runs it through the authoritative simulation, and echoes
back the authoritative result. The client retains a ring of its own
predicted states indexed by input sequence number. When the authoritative
echo arrives, the client rewinds, applies the authoritative value, and
replays any predictions made after that sequence number.

Reconciliation happens in game code, not in the replication runtime. The
runtime just delivers the values; games decide what reconciliation looks
like for their specific gameplay feel. The runtime provides the sequence
number, the last-acked tick, and the current server tick so games can do
the rewind.

### 6.4 Hot reload

DLL reload is already a first-class concern (see `PLUGIN_CONTRACT.md`).
Replicated components defined in game DLLs have a slightly more delicate
reload path than non-replicated components: the registry holds a
descriptor keyed by type name, and if the DLL rebuild changes the field
layout, the cached descriptor is stale.

On reload, the replication runtime:

1. Suspends outbound replication for any entity using the reloaded
   component.
2. Drops the cached descriptor.
3. Re-registers from the new DLL.
4. If the schema version matches (field count, field names, field types
   all identical), resumes replication with the new descriptor.
5. If the schema version does not match, logs a warning and keeps the
   old entities server-side but does not replicate the changed component
   on the reloaded DLL until a restart. This is correct behavior:
   mid-session schema change is a dev action that trades short-term
   breakage for iteration speed.

Games that want zero-downtime schema change can bump the schema version
explicitly and provide a migration. That's a per-component decision, not
an engine-level one.

---

## 7. Schema Versioning

### 7.1 The version field

Every replicated type carries an implicit schema version — a 16-bit
integer that is derived from the `SV_REPLICATE` macro expansion at
compile time (hash of field names + types in declaration order).

The version is stamped on every snapshot record. Clients check their
locally-compiled version against the incoming version and:

- If they match, apply normally.
- If they differ, log a warning and drop the record.

A mismatch means "the server and client disagree on what this component
looks like," which is always a version-skew bug. It's better to drop
than to misinterpret bytes.

### 7.2 Explicit overrides for migrations

If a game needs to change a replicated field layout without a full
rebuild-and-redeploy cycle, it can declare an explicit version pin:

```cpp
SV_REPLICATE_VERSIONED(TransformComponent, 3,
    SV_FIELD(position),
    SV_FIELD(rotation),
    SV_FIELD(scale)
);
```

Version 3 overrides the auto-computed hash. The runtime uses the pinned
version on the wire. Games can register a migration function that reads
a version-2 record and produces a version-3 struct.

This is an escape hatch, not the default. Games should rebuild both
sides together unless they have a specific reason not to.

### 7.3 Engine version vs schema version

The StratumV version is
separate from replication schema versions. Engine version bumps signal
public-header changes to consumers; schema versions signal wire-format
changes to the runtime. They're independent counters.

---

## 8. Testing

### 8.1 Reflection registry unit tests

The reflection registry is tested in isolation, no networking. The
registry suite has 34 Catch2 cases in `tests/test_ReplicationRegistry.cpp`
covering:

- `fnv1a32` hash stability and known-value spot checks.
- `fieldTypeFor<T>()` scalar specializations + Unknown fallback.
- `fieldTypeToString` covers every enum value.
- `SV_REPLICATE` on a synthetic component produces the expected
  `ReplicationMeta` in the registry.
- `SV_FIELD` offsets match `offsetof`, sizes match `sizeof`, types
  map via `fieldTypeFor`.
- Dirty-bit indices are sequential in declaration order.
- Field name hashes are populated and collision-free across a
  synthetic component.
- `SV_FIELD_QUANT` carries `quantStep`; plain `SV_FIELD` leaves it
  at `0.0f`.
- `ReplicationMeta::findField` / `findFieldByHash` / `fieldIndex`
  lookups and miss cases.
- `ReplicationRegistry::find` / `findByHash` / `all` / `size` /
  `empty` / `resetForTests`.
- Two components register independently (distinct `typeNameHash`
  and `schemaVersion`).
- Schema version is stable across re-registration, and changes when
  a field is renamed or its type changes.
- `SV_DIRTY` returns the correct bit index end-to-end.
- `DirtyMask` default state, single-bit set/test/clear, out-of-
  range no-op, `setAll` + `reset`, packed fast path at exactly 64
  fields, overflow path at 100 and 72 fields, `resize` wipes state.
- End-to-end: `DirtyMask` driven by `SV_DIRTY` inside a simulated
  gameplay mutation.

Serializer / deserializer / delta-encoder tests live with the
snapshot encoder suite (§8.2), not here: the registry suite covers
field metadata only, no serialization.

### 8.2 Snapshot encoder unit tests

A further 20 Catch2 cases in
`tests/test_ReplicationRegistry.cpp` bring the module total from
34 → 54. Coverage:

- `Authority` default on a replicated type is `Server` (no explicit
  `SV_COMPONENT_AUTHORITY`).
- `authorityToString` covers every enum value.
- `SV_COMPONENT_AUTHORITY` takes effect at program start (verified
  via a file-scope lambda capture that snapshots the authority
  right after the macro trigger ran, without any test-side helper).
- `ReplicationRegistry::setAuthority` patches a registered type and
  returns false for unknown types.
- `resetForTests` wipes authority along with metas (verified by
  re-registering without the tag and seeing the default come back).
- `SnapshotWriter` / `SnapshotReader` raw primitives round-trip
  u8 / u16 / u32 / u64 / bool / float / double.
- LEB128-style varint round-trips across all 1-byte through 5-byte
  boundaries for u32 and all max-value widths for u64.
- Zigzag round-trips for signed int32 / int64 including min/max.
- Quantized float round-trips within half-step tolerance across six
  step/value combinations (mm, cm, integer, half-unit, fine-step
  large-magnitude).
- `encodeSnapshot` + `decodeSnapshot` full-mask round-trip on the
  `ReplTestScalar` fixture (6 fields, all types covered).
- Partial-mask round-trip: only dirty fields are encoded, decoder's
  `outMask` reports exactly those bits, untouched destination
  fields keep their pre-decode values.
- Empty-mask round-trip: encoder writes `u16` schema + mask bytes
  only, decoder reports an empty mask, destination is unchanged.
- Quantized component round-trip on the `ReplTestQuant` fixture
  (exercises the `quantStep > 0` branch of the encoder switch).
- Decoder rejects mismatched schema version without touching the
  destination instance.
- Decoder rejects truncated buffers (EOF mid-field + EOF in the
  mask bytes).
- Encoder rejects `DirtyMask` size mismatch before writing any
  bytes.
- Reader's `readVarintU32` rejects overlong encodings (>5 bytes)
  without advancing the cursor.

Authority rule enforcement (Server rejects client mutation, Owner
accepts mutation from its owner only, Editor accepts any Editor-
scope client) is not in the encoder suite — those need the transport
+ a per-mutation origin tag and land with the networking layer.
Entity handle remap table round-trips land with the transport (handles
don't exist until the transport does).

### 8.3 Integration tests

Once transport is live, two in-process `stratumv_server` instances (or
server + headless client) exchange snapshots over loopback MsQuic. These
are closer to end-to-end than unit tests and run in CI with the
networking test suite. Target: ≥5 integration tests.

---

## 9. Scope Exclusions

What the replication contract deliberately does not try to do.

- **No prediction framework.** Client-side prediction is game-specific.
  One game's movement prediction and another's shooter prediction will
  look nothing alike. The runtime exposes sequence numbers and
  reconciliation primitives; games build prediction on top.
- **No interpolation curve controls.** The snapshot buffer and
  interpolation delay are per-client defaults with hooks for games to
  override, but the engine does not ship a "feel-good interpolation
  system." That's a polish problem per-game.
- **No lag compensation.** Server-side lag compensation (rewinding
  history to the moment a client clicked "fire") is a hit-registration
  feature. Some games will need it; others won't.
  Whichever game needs it first builds it in its own DLL, using the
  tick history that the runtime exposes.
- **No save system per se.** Save games are "a snapshot written to disk."
  The runtime's encoder is the same encoder the save system uses. A
  full save/load manager with slots, cloud sync, thumbnail captures, etc.
  is a separate session.
- **No anti-cheat.** As discussed in `NETWORK_DESIGN.md` section 8, the
  authoritative server model makes most cheats ineffective by
  construction. Validation lives in game logic, not the runtime.

Everything on this list is explicitly deferrable because the reflection
registry is a clean primitive that future features can build on without
reworking the core.

---

## 10. Related Docs

- **`NETWORK_DESIGN.md`** — Transport (MsQuic), server/client model,
  determinism, scale target, observability.
- **`COLLAB_EDITING.md`** — Permission scopes, transaction log, undo,
  asset sync. Uses `Authority::Editor` as its authority primitive.
- **`PLUGIN_CONTRACT.md`** — DLL boundary rules. Section on hot reload
  interacts with the replication schema version handling (section 6.4
  here).
- **`ARCHITECTURE.md`** — Layer model. `ReplicationRegistry` lives in
  Layer 4 services.
- **`CHANGELOG.md`** — version history, including the 1.3.0 bump with a
  migration table for `ctx.network->*` → `ctx.network.*`.
