# Maintenance backlog

Working queue for the automated daily maintenance run (and anyone else).
The bot reads this at the start of each run, picks the highest-value items
it can land through the full test gate, and updates statuses via PR.

Tiers: S = small (one run, low risk) · M = medium (one run, needs care) ·
L = large (multi-run, land in gated slices)

| id | title | tier | status | notes |
|----|-------|------|--------|-------|
| 1 | Refresh docs/LINUX_SERVER.md MsQuic recipe | S | done | Rewritten to the headers-from-tag + packages.microsoft.com recipe (matches CI); references bumped to 2.5.8 (this PR, after PR #21 bumped the pin). |
| 2 | /W4 warning inventory + burn-down | M | done | 2026-07-03: final C4244 (Audio.cpp ma_uint64→double) fixed with explicit static_cast; C4996 handled via item 10. Project warnings now zero (MSB8029 tracked as item 9). |
| 3 | Test-coverage gap scan | M | in-progress | 2026-07-03 scan (by header inclusion, not filename): pure-logic modules with NO test coverage: WorldStateIO, SceneStatePersistence, InputBindings, InputAction, ReplicationProtocol, MorphTargetTypes. 2026-07-03 run 2: test_WorldStateIO.cpp added (7 cases: anchor round-trip/defaults/partial-vec3, weather sparse-override round-trip, biome vec3 tint). Remaining uncovered: SceneStatePersistence, InputBindings, InputAction, ReplicationProtocol, MorphTargetTypes. |
| 4 | TODO/FIXME/HACK mining pass | S | done | 2026-07-02 sweep: src/, tests/, lab/, shaders/ are clean; the only TODO markers are intentional template text in tools/scaffold_game.py. Nothing actionable. |
| 5 | Bump JoltPhysics v5.2.0 → v5.5.x | M | done | 2026-07-03: bumped to v5.5.0. Gated in a fresh build dir with -DSTRATUMV_ENABLE_JOLT=ON: JoltPhysicsContext.cpp compiles clean against 5.5.0 (no API migration needed), 423/423 tests, smoke clean. Default stays OFF. |
| 6 | Vulkan-Headers/volk bump | M | blocked | Pinned to installed Vulkan SDK 1.4.313 by design; bump only together with an SDK update on the dev box. |
| 7 | MSVC /analyze pass on src/engine/vk | M | todo | One subsystem per run; fix real findings only. |
| 8 | Golden suite on more passes | M | todo | Consider goldens for cluster AS / shadow cascades once deterministic. |
| 9 | MSB8029 warning (build dir under temp) | S | todo | 2026-07-03 diagnosis: bot shell runs with TMP unset (TEMP normal); persistent MSBuild node processes can inherit a stale/odd TMP, tripping the under-temp heuristic for E:\StratumV-bot\build. Cosmetic, machine-local. Candidate fix: set TMP explicitly in the maintenance environment, or /p:UseSharedCompilation=false. |
| 10 | C4996 deprecation burn-down (21 sites) | M | done | 2026-07-03: added src/engine/CrtCompat.h (sv::StrCopy/FOpen/GetEnv/StrError/U8Path) and migrated every strncpy/fopen/getenv/strerror/u8path site in src, lab, tests; also collapsed pre-existing per-site `#if _WIN32 fopen_s` blocks into sv::FOpen. No _CRT_SECURE_NO_WARNINGS anywhere. |
| 11 | UI style doc (docs/UI_STYLE.md) | S | done | 2026-07-03: created; separator rule, casing, UiStyle palette usage, enforcement notes. |
| 12 | How-to overlay: F1 toggle + first user topics | M | todo | UX epic slice 1. Reusable sv::HelpOverlay in src/engine/ui drawn by skinned_test; F1 toggles; topics: welcome, camera, editing keys, panels, quitting. Games extend via addTopic. Verify with --show-help + capture rig. |
| 13 | How-to overlay: developer topics tab | S | todo | UX epic slice 2. Second tab with architecture pointers (subsystem map, where passes/net/ui live, how to extend); builds on item 12. |
| 14 | Load screen: skeleton pass with progress bar | M | todo | UX epic. Replace bare window during startup/asset load; progress source: EngineBase init + asset scan phases. |
| 15 | First-open experience: welcome panel | M | todo | UX epic. On first launch (marker file absent) open help overlay / welcome panel with docs pointers and default layout. |
| 16 | Controls UX: bindings display panel | M | todo | UX epic. Read InputBindings/InputAction; show current bindings in a panel; conflict detection as a later slice. |
| 17 | Menus: main menu bar consolidating lab debug windows | M | todo | UX epic. ImGui main menu bar in skinned_test toggling the five debug windows instead of always-open floating windows. |
