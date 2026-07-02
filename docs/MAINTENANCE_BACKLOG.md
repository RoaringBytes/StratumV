# Maintenance backlog

Working queue for the automated daily maintenance run (and anyone else).
The bot reads this at the start of each run, picks the highest-value items
it can land through the full test gate, and updates statuses via PR.

Tiers: S = small (one run, low risk) · M = medium (one run, needs care) ·
L = large (multi-run, land in gated slices)

| id | title | tier | status | notes |
|----|-------|------|--------|-------|
| 1 | Refresh docs/LINUX_SERVER.md MsQuic recipe | S | todo | Old release-tarball download 404s; document the headers-from-tag + packages.microsoft.com .so approach now used by CI (see #12, PR #17). |
| 2 | /W4 warning inventory + burn-down | M | todo | Build once with /W4, log to file, fix top offenders in small PRs. |
| 3 | Test-coverage gap scan | M | todo | Compare src/ modules against tests/; add unit tests for pure-logic code with none (replication, animation state machine edges). |
| 4 | TODO/FIXME/HACK mining pass | S | todo | Inventory src/ markers; implement S items, backlog the rest here. |
| 5 | Bump JoltPhysics v5.2.0 → v5.5.x | M | blocked | STRATUMV_ENABLE_JOLT is OFF by default so the gate never exercises it; bump + gate with -DSTRATUMV_ENABLE_JOLT=ON explicitly, keep default OFF. |
| 6 | Vulkan-Headers/volk bump | M | blocked | Pinned to installed Vulkan SDK 1.4.313 by design; bump only together with an SDK update on the dev box. |
| 7 | MSVC /analyze pass on src/engine/vk | M | todo | One subsystem per run; fix real findings only. |
| 8 | Golden suite on more passes | M | todo | Consider goldens for cluster AS / shadow cascades once deterministic. |
| 9 | MSB8029 warning (build dir under temp) | S | todo | Machine-local CMake/MSBuild warning on the bot clone; investigate TMP env or suppress in docs. |
