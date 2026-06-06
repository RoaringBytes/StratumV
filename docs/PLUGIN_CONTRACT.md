# PLUGIN_CONTRACT — DLL Plugin Rationale

> Decision-layer companion to `ARCHITECTURE.md`. This doc explains **why** the DLL
> plugin system is shaped the way it is. For the what — module layering, file map,
> render graph stages — see `ARCHITECTURE.md`. For the surface API, read
> `src/engine/IModularSystem.h` and `src/engine/BaseSystemContext.h` directly.

StratumV is a frozen render core plus hot-reloadable gameplay DLLs. Every design
decision in this document traces back to two hard requirements:

1. **The render core cannot break when the gameplay DLL rebuilds.** Artists and
   gameplay programmers iterate on plugins every few seconds; a crash in the
   DLL cycle is a full engine restart.
2. **Games consume the engine as a library**, not as a
   framework. Their `struct SystemContext` is defined in game code, not engine
   code. The engine never includes game headers.

Everything below — the forward-declared `SystemContext`, the nested POD
sub-structs, the `reinterpret_cast` in `DLLLoader::loadSystems`, the
`getInterfaceVersion()` virtual, the null fn-ptr sweep, the JSON
serialize/deserialize hooks — is a consequence of one or both of those rules.

---

## 1. The DLL Boundary

### 1.1 Forward-declared `SystemContext`

`IModularSystem.h` forward-declares `struct SystemContext;` and passes it through
the lifecycle methods as `const SystemContext&`. The engine never accesses its
members. Games define the full struct in game code:

```cpp
// game-side
struct SystemContext : sv::BaseSystemContext {
    const SceneUBO*        sceneState   = nullptr;
    GameSpecificManager*   worldManager = nullptr;
    std::function<void(const char*)> spawnProjectile;
};
```

**Why:** When the engine was extracted from its original host game, the
`SystemContext` was a 456-line struct with SceneUBO and dozens of
game-specific systems (terrain UBO, world simulation, player, inventory,
crafting, etc.) — every game system the host had bolted on. Leaving that in
the engine would have tied the engine to one game's design. Stripping it out
via forward declaration let us move game-specific fields back into the game's
own repo without changing the engine ABI.

The engine-internal `DLLLoader` and `DevServer` pass the context through by
reference without dereferencing it. The only place engine code reads the base
subset is `DLLLoader::sweepContextSlots()`, which uses a `reinterpret_cast`
(see §1.3).

### 1.2 `BaseSystemContext` as a public single-inheritance base

`BaseSystemContext` (defined in `src/engine/BaseSystemContext.h`) holds the
135 leaf fields the engine itself owns — VkDevice, VmaAllocator, ECS registry,
event dispatcher, 29 volk-loaded `PFN_vk*` function pointers, the ozz
animation runtime, audio callbacks, rendering state, and so on. Games extend it
via **public single inheritance**:

```cpp
struct SystemContext : sv::BaseSystemContext { ... };
```

This is load-bearing. The C++ standard guarantees that with public
single-inheritance and no virtual bases, the base subobject lives at offset 0
of the derived object. `DLLLoader::loadSystems()` takes advantage of this:

```cpp
reinterpret_cast<const BaseSystemContext&>(ctx)
```

If that invariant ever breaks — someone changes `SystemContext` to use multiple
inheritance, or adds a virtual base — the reinterpret_cast silently corrupts
every plugin's context. The rule is encoded in the generated scaffold (see
`tools/scaffold_game.py`) and documented in the game contract rule #6.

### 1.3 POD-only sub-structs (1.2.0)

Prior to 1.2.0, `BaseSystemContext` was 135 flat fields in a single struct.
Release 1.2.0 regrouped them into **14 flat hot fields plus 9 nested POD
sub-structs** — `rendering`, `ui`, `input`, `buffers`, `vkfn`, `meshRegistry`,
`audio`, `animation`, `world`. See `CHANGELOG.md` for the full migration table.

Each sub-struct is explicitly **POD with default member initializers only**. No
constructors. No destructors. No virtuals. No base classes. `sizeof` is
identical before and after the refactor (3376 bytes on VS 2022 x64 Release).

**Why the POD rule is absolute:**

- Adding a constructor to a sub-struct changes whether it's trivially default-
  constructible. `BaseSystemContext members{};` might start invoking user code,
  which means plugin init order starts mattering.
- Adding a virtual method to a sub-struct adds a vtable pointer, shifting every
  field offset below it. The reinterpret_cast in §1.2 then targets the wrong
  bytes.
- Adding a non-POD member (e.g. `std::shared_ptr<Foo>`) destroys the guarantee
  that copying `BaseSystemContext` by value is safe. `std::function` is already
  non-trivial — that's why the layout tests use
  `std::is_default_constructible_v` instead of `std::is_trivially_copyable_v`
  (the pre-refactor struct was never trivially copyable either).

The test suite enforces this. `tests/test_MockContext.cpp` contains
`static_assert(sizeof(BaseSystemContext) <= 3376)` plus a
default-constructibility check. If a change accidentally adds a
constructor to a sub-struct, the assertion fails at compile time.

### 1.4 Why not `pImpl` or opaque handles?

The obvious alternative is to hide `BaseSystemContext` behind an opaque pointer
and give plugins accessor functions (`ctx_get_device(handle)`, etc.). We
rejected this for two reasons:

- **Debuggability.** Plugins routinely touch 40+ context fields in tick().
  Wrapping each access in a function call trashes the debugger's variable
  inspector and adds a layer of indirection nothing in the engine benefits from.
- **No ABI churn savings.** Adding a field to an opaque API still requires
  recompiling every plugin (new accessor) and bumping the interface version.
  The POD layout has the same constraint with less ceremony.

The trade-off is that plugins see more than they strictly need — but plugins
are in-process, trusted, and compiled against the same Vulkan SDK as the
engine. "Least privilege" is the wrong frame here.

---

## 2. Version Gating & Interface Stability

`IModularSystem.h` declares:

```cpp
static constexpr uint32_t kEngineInterfaceVersion = 1;

virtual uint32_t getInterfaceVersion() const { return kEngineInterfaceVersion; }
```

Because the default implementation is inlined in the header, **a stale DLL
compiled against an older copy of `IModularSystem.h` returns the old constant**.
`DLLLoader` reads that value at load time and refuses any plugin whose version
doesn't match `kEngineInterfaceVersion` in the currently-running engine.

**Why inline and not pure virtual:** Making it pure virtual would force every
plugin to override it, which is busywork for the 99% case where the version
matches. Inlining the default means existing plugins keep working unchanged
when version gating landed — a full set of 28 plugins compiled clean without
code edits.

**When to bump `kEngineInterfaceVersion`:**

- A new pure virtual method is added to `IModularSystem` (breaks all existing
  plugins at compile time).
- The memory layout of `BaseSystemContext` changes in a way that invalidates
  the offset-0 reinterpret_cast assumption (§1.2).
- A function pointer type in `BaseSystemContext` changes signature.

**When NOT to bump:**

- Adding a new field to the end of a sub-struct. Existing plugins keep their
  old field layout; the new field is accessed via `ctx.<group>.<field>` which
  didn't exist before. No recompile needed at the ABI level, though plugins
  that want to use the new field obviously need to rebuild.
- Adding a new non-pure virtual with a sensible default (like
  `getInterfaceVersion` itself).
- Renaming a field inside a sub-struct (plugins using the old name fail to
  compile, which is loud enough without a version bump).

The 1.2.0 sub-struct regrouping is the awkward case: the migration from flat to
nested paths is a source-level break for plugins, but the binary layout is
unchanged.
We chose to bump the StratumV library version to 1.2.0 but kept
`kEngineInterfaceVersion` at 1, because any plugin that builds against the new
header will produce a correct binary.

---

## 3. Initialization Priority

`IModularSystem::initPriority()` returns `int32_t` with a default of 0. Lower
values initialize first. `DLLLoader::loadSystems` uses `std::stable_sort` on
the priority before calling `init()`, then calls `shutdown()` in reverse order.

**Why stable sort:** Early plugins all returned the default priority 0.
A stable sort means their load order is preserved from whatever order the DLL
exported them in — a game had subtle dependencies on its original load
order that would have broken with any other sorting strategy. New plugins
that actually need to init early pick a negative priority; priority 0 remains
the "load in export order" escape hatch.

---

## 4. Null Fn-Ptr Sweep

`DLLLoader::loadSystems` calls the static `sweepContextSlots(ctx)` helper
before invoking any plugin's `init()`. This walks the sub-struct paths on
`BaseSystemContext` and logs any required function pointer that is still null:

```
[DLLLoader] MISSING REQUIRED: input.isKeyDown
[DLLLoader] MISSING REQUIRED: buffers.createBuffer
[DLLLoader] MISSING REQUIRED: vkfn.fnCmdBindPipeline
```

The error paths use the sub-struct-qualified field names (post-1.2.0), which
makes them directly searchable in `BaseSystemContext.h`.

**What counts as "required":**

- All 13 `std::function` slots in `input`, `buffers`, `rendering` render-graph
  hooks, etc.
- All 29 `PFN_vk*` slots in `vkfn`.

**What is "optional" (warn-only):**

- Audio callbacks — games without audio can leave them null.
- Gamepad fn-ptrs — games without gamepad support can leave them null.
- Physics / network interfaces — both are `I*Context*` pointers defaulted to
  `nullptr`; a missing no-op is a legitimate choice.

Total: 42 required slots, 12 optional.

**Why sweep at all:** a game was discovered to be calling
fn-ptrs the engine never wired up — the code Just Worked because the missing
pointer happened to be adjacent in memory to a real function and nothing crashed
in the profile runs. The sweep catches that class of bug at load time with a
clear error message, instead of at frame 2000 with an access violation.

---

## 5. Hot-Reload Lifecycle

The reload cycle is driven by `AssetWatcher` polling the DLL path. When the
file mtime changes, `DLLLoader::checkReload` runs the following sequence:

1. **Serialize plugin state.** `unloadSystems` iterates `systems`, calls
   `serializeState()` on each, and stores the resulting JSON strings in a
   parallel vector.
2. **Destroy old instances.** The old DLL's `DestroySystemsFn` is called. This
   runs each plugin's `shutdown()`, frees their allocations, and returns
   control to the DLL itself.
3. **`FreeLibrary` the old handle.** At this point, any code still referencing
   the old DLL's memory (captured lambdas, callback pointers) will segfault.
   The contract is that the engine owns all long-lived state; plugins are
   fire-and-forget.
4. **Shadow-copy the new DLL.** The fresh build is copied to a sibling path
   (`game_systems.dll` → `game_systems.shadow.dll`) so that the build output
   is never file-locked. This lets the game rebuild the DLL while the engine
   still holds a handle to the previous copy.
5. **`LoadLibrary` the shadow.** The new DLL is loaded from the shadow path.
6. **Resolve exports + call `CreateSystemsFn`.** The new plugin array is
   populated.
7. **Sweep context slots** (§4) — catches the case where the plugin added a
   new required fn-ptr the engine hasn't wired yet.
8. **Sort by `initPriority`** (§3).
9. **Deserialize state.** Each new plugin receives its saved JSON via
   `deserializeState()`, restoring its in-process state from before the reload.
10. **Call `init()`** on each plugin in sorted order, followed by `postInit()`.

### 5.1 Why shadow copy?

Windows locks DLLs that are currently mapped into a process. If the engine
loaded the file directly from the build output, every rebuild would fail with
"file in use". The shadow copy is the single-line fix: `CopyFile` to a sibling
path, `LoadLibrary` that, and the build output stays writable.

### 5.2 Why `vkDeviceWaitIdle` is the caller's responsibility

Originally, `DLLLoader::checkReload` called `vkDeviceWaitIdle(ctx.device)`
internally. That required the loader to touch `SystemContext::device`, which
violates §1.1 (the engine doesn't look inside game-defined SystemContext
fields). The engine moved that call out to the caller — it is now
caller-required and documented in the header.

`EngineBase::run()` does it before `m_dllLoader.checkReload(...)`. Games that
embed their own loops have to do it too — the rule is that all command buffers
referencing plugin-owned resources must be idle before FreeLibrary fires,
otherwise Vulkan sees a dangling function pointer mid-frame.

### 5.3 JSON round-trip as the state-preservation protocol

Plugins implement `serializeState()` → `std::string` and
`deserializeState(const std::string&)`. The string format is required to be
JSON, but beyond that it's per-plugin. Using JSON and not a binary protocol
was a deliberate choice:

- **Forward-compatible.** A plugin can add new state fields and the old
  deserializer ignores them. Binary formats punish this.
- **Debuggable.** When a reload cycles incorrectly restores state, the engine
  logs the raw JSON and you can diff it against the expected output.
- **DLL-boundary-safe.** `std::string` is a standard container whose ABI is
  stable across DLL boundaries when the engine and plugins are built with
  the same compiler and STL version (our CI enforces this — all three
  binaries use the same VS 2022 toolchain and /MD dynamic CRT).

The `"{}"` default return means plugins that don't care about hot-reload
state get the correct behavior for free.

---

## 6. Per-Frame Hooks

`IModularSystem` exposes seven per-frame hooks, all of which default to no-op:

| Hook | Where in frame | Command buffer state |
|------|----------------|----------------------|
| `tick(dt, ctx)` | Before any GPU work | N/A — not a recording callback |
| `recordCompute` | Before main-pass compute dispatch | Compute stage, no render pass active |
| `recordShadowDraw` | Inside each shadow cascade record | Shadow pipeline + cascade layout bound |
| `recordPreDraw` | Main pass, before scene geometry | Scene descriptor set 0 bound |
| `recordPostDraw` | Main pass, after scene geometry | Same state as PreDraw |
| `recordUIPass` | After post-process, before ImGui | Swapchain in COLOR_ATTACHMENT_OPTIMAL |
| `recordPostUIPass` | After ImGui, before present | Final composition stage |

The `HookMode` enum (`PreDraw` / `PostDraw` / `Both`) is a declarative hint
letting the engine skip plugins that don't need a given stage — a performance
optimization, not a correctness requirement. Plugins overriding
`recordShadowDraw` must be idempotent across cascades because the engine calls
it once per cascade (currently 4).

**Why seven hooks and not a more flexible RenderGraph API:** The RenderGraph
(`src/engine/graph/`) already exists as the mechanism for plugin-owned passes.
The seven hooks are a shortcut for the common case where a plugin just wants
to draw some extra geometry inside an existing pass. The rule of thumb:

- If you need custom attachments or a new pipeline barrier pattern,
  register a RenderGraph pass via `ctx.rendering.registerRenderPass`.
- If you just want to draw stuff inside the main pass, override
  `recordPreDraw` or `recordPostDraw`.

---

## 7. DevServer Command Registration

DevServer (TCP :9999) exposes engine commands directly and lets game code
register additional commands via `registerCommand` (read-only, runs on the
server thread) and `registerMainThreadCommand` (mutates state, runs on the
game-loop thread). A shipping game can register dozens of game commands
through this mechanism.

**Why two registration methods:** The server thread accepts connections and
dispatches requests without blocking the game loop. Read-only queries
(`get_state`, `get_health`) can run directly on the server thread because they
only read snapshot data. Mutations (`set_state scene/postprocess`, `spawn_*`)
must run on the game-loop thread to avoid races with ECS updates and Vulkan
command buffer recording. The split is explicit because getting it wrong is
hard to debug — a race between DevServer writes and the main thread manifests
as random crashes under load, not validation errors.

**Why not let plugins register TCP commands directly:** The DevServer socket
lives in engine-owned memory. A plugin reload would invalidate any callback
the plugin registered with the engine-side command table. The engine-facing
registration API means that the game's Engine object owns the lifecycle of
those handlers — when the game shuts down, it drops the DevServer before the
DLL unloads, so no dangling lambdas survive.

---

## 8. Frozen Render Core Policy

The render core is explicitly frozen:

- No new render passes without a StratumV library version bump (semver minor).
- No new fields in `BaseSystemContext` without updating every consumer.
- `IModularSystem` is ABI-stable until `kEngineInterfaceVersion` bumps.
- `SceneUBO` is per-game — the engine does not define it.

**Why freeze:** The goal is that games consuming StratumV via `add_subdirectory`
can pull a new version in 30 seconds, not 30 days. Every change that forces
gameplay programmers to update DLL plugin code is a change that slows them
down. The freeze is enforced at PR review time — if a change needs to modify
a render pass or `BaseSystemContext`, it has to also update every consumer, or
justify why deferring them is safe.

The 1.2.0 sub-struct regrouping violated this rule deliberately: it touched
`BaseSystemContext` layout. The justification was that the new nested layout
reduces future churn (plugin code is more discoverable, `engine_ref.py` emits
readable paths, DLL error messages gain a qualifier), and consumer migration is
a mechanical sed pass.

---

## 9. Things the Contract Deliberately Doesn't Do

- **No plugin-side memory allocator.** Plugins allocate using their own CRT.
  The engine does not hand out an arena. Consequence: plugin allocations leak
  if the plugin fails to free them in `shutdown()`. Mitigation: reload cycles
  happen many times per dev session, and leaks show up as monotonically growing
  committed bytes in Task Manager very quickly.
- **No sandboxing.** A plugin bug crashes the engine. This is acceptable for
  first-party code; mods and third-party plugins are a different category and
  would need an out-of-process plugin host (backlog).
- **No cross-plugin dependency declaration.** Plugins don't say "I need
  PhysicsSystem initialized before me". Instead, they use `initPriority()`
  to put themselves after known foundation systems. It's crude but works with
  dozens of plugins and will work until we have hundreds.
- **No Lua/scripting language layer.** Plugins are C++ DLLs. Scripting is a
  recurring request but adds another ABI boundary to maintain, another set
  of bugs, and another tool dev has to learn. ECS + hot-reloadable C++ has
  been sufficient so far.

---

## Related Files

- `src/engine/IModularSystem.h` — the interface itself
- `src/engine/BaseSystemContext.h` — the nested POD layout
- `src/engine/DLLLoader.h/.cpp` — loading, unloading, sweep, hot-reload
- `src/engine/AssetWatcher.h/.cpp` — file-mtime polling that drives reloads
- `src/engine/SystemRegistry.h/.cpp` — the runtime plugin list
- `src/engine/DevServer.h/.cpp` — registrable command handlers
- `tests/test_MockContext.cpp` — layout static_asserts + sub-struct guards
- `CHANGELOG.md` — version history, including the 1.2.0 sub-struct regrouping migration table
- `ARCHITECTURE.md` — the structural view this doc complements
