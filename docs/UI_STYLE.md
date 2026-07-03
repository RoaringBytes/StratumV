# UI text and style conventions

Rules for every string and widget a player, creator, or user sees in
StratumV: menus, menu section headers, HUD text, load screens,
onboarding text, help and how-to pages, tooltips, and error dialogs.
Code comments and log output are not covered by this document.

## Separator rule

Slashes and dashes used as decoration belong in code comments only.
They must never appear in user-facing viewing space. This includes
"//", "--", box-drawing runs, and any ASCII-art separator built from
punctuation. User-facing headers and separators must use clean
typography instead: plain text, spacing, or a proper UI divider
widget such as ImGui::Separator or ImGui::SeparatorText.

A quick self-check before merging UI work: read every new or touched
string as a player would see it on screen. If a character in it is
there to draw a line rather than to spell a word, replace it with a
widget or with whitespace.

## Casing and wording

Window titles use Title Case, matching the existing panels: Admin
Panel, Asset Browser, Animation Debug, Network Demo, Replicated
Assets, Thumbnail Bake. Labels and body text use sentence case.
Keep wording short, concrete, and free of placeholder text. Text
must be readable at 100 percent scale and must not clip or overlap
neighboring widgets.

## Color and theming

All panel code takes its colors from the shared palette in
src/engine/ui/UiStyle.h (sv::style constants such as kAccent, kText,
kTextDim, kHeader). Do not hardcode ImVec4 colors in panels except
for genuinely one-off semantic tints. The theme is applied once via
sv::style::applyDefaultStyle after ImGui::CreateContext, and runtime
overrides live in ui_style.json, reloaded through AssetWatcher.

## Enforcement

The daily maintenance run checks every touched or added UI string
against these rules and verifies them visually in screenshots before
merging. Violations found in existing user-facing text are filed as
small-tier items in docs/MAINTENANCE_BACKLOG.md and fixed in gated
follow-up PRs.
