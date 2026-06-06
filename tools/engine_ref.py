#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""StratumV engine_ref — generates ENGINE_REF.md and engine_state.json.

Scans src/engine/, classifies modules into layers via arch_map_config.json,
parses BaseSystemContext.h for the full field API, and writes:
  docs/ENGINE_REF.md   — human-readable layer map + context API
  docs/engine_state.json — machine-readable snapshot
"""

import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENGINE_ROOT = REPO_ROOT / "src" / "engine"
CONFIG_PATH = Path(__file__).resolve().parent / "arch_map_config.json"
OUT_MD = REPO_ROOT / "docs" / "ENGINE_REF.md"
OUT_JSON = REPO_ROOT / "docs" / "engine_state.json"

# ─── Config loading ─────────────────────────────────────────────────────

def load_config():
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        return json.load(f)

# ─── File scanning ──────────────────────────────────────────────────────

def scan_engine_files():
    """Return sorted lists of .h and .cpp relative paths under src/engine/."""
    headers, sources = [], []
    for root, _dirs, files in os.walk(ENGINE_ROOT):
        for fname in sorted(files):
            rel = os.path.relpath(os.path.join(root, fname), ENGINE_ROOT).replace("\\", "/")
            if fname.endswith(".h"):
                headers.append(rel)
            elif fname.endswith(".cpp"):
                sources.append(rel)
    return sorted(headers), sorted(sources)

# ─── Layer classification ───────────────────────────────────────────────

def classify_module(rel_path, config):
    """Classify a header into a layer number using config rules."""
    dir_rules = config.get("directory_rules", {})
    overrides = config.get("module_overrides", {})

    parts = rel_path.replace("\\", "/").split("/")
    # Module name = stem of the file
    stem = Path(rel_path).stem

    # Check explicit override first (by stem)
    if stem in overrides:
        return overrides[stem]

    # Check directory rules
    if len(parts) > 1:
        subdir = parts[0]
        if subdir in dir_rules:
            return dir_rules[subdir]

    return 0  # unclassified

def build_module_map(headers, sources, config):
    """Build {module_name: {layer, header, source, header_only}} for each module."""
    source_set = {s for s in sources}
    modules = {}
    for h in headers:
        stem = Path(h).stem
        layer = classify_module(h, config)
        # Find matching .cpp
        cpp_path = h.replace(".h", ".cpp")
        has_source = cpp_path in source_set
        modules[stem] = {
            "name": stem,
            "layer": layer,
            "header": h,
            "source": cpp_path if has_source else None,
            "header_only": not has_source,
        }
    return modules

# ─── BaseSystemContext parser ───────────────────────────────────────────

SECTION_RE = re.compile(r"^\s*//\s*──\s*(.+?)\s*──")
# Matches a nested sub-struct member line with `{}` value-init:
#   `RenderingContext rendering {};`
#   `PerformanceBudget budget {};`
#   `NetworkStats     network {};`
# Generic FIELD_RE doesn't accept the `{}` default so we catch this first.
# Originally this regex required the type name to end in "Context"
# (matching the nested sub-struct convention). Two non-Context
# POD types nested inside PerformanceContext (PerformanceBudget,
# NetworkStats) mean the regex now accepts any capitalised type name.
NESTED_MEMBER_RE = re.compile(
    r"^\s+"
    r"([A-Z][A-Za-z0-9_]*)"          # any CapName type (was: <Name>Context)
    r"\s+"
    r"(\w+)"                          # member name
    r"\s*\{\s*\}\s*;"
)
FIELD_RE = re.compile(
    r"^\s+"                           # leading whitespace
    r"(?:const\s+)?"                  # optional const
    r"([\w:<>&*,\s]+?)"              # type (greedy but lazy enough)
    r"\s+"
    r"(\w+)"                          # field name
    r"(?:\s*=\s*(.+?))?"             # optional default
    r"\s*;"
)
FUNC_RE = re.compile(
    r"^\s+"
    r"(std::function<.+?>)"           # std::function<...>
    r"\s+"
    r"(\w+)"                          # field name
    r"\s*;"
)


def _preprocess_lines(raw_lines, struct_name):
    """Return a list of logical lines, with multi-line declarations merged
    and anything outside of `struct BaseSystemContext { ... }` stripped out.

    The struct body ends at the matching closing `};` — we track brace
    depth so that nested sub-structs inside BaseSystemContext don't cut
    us off early.
    """
    logical = []
    accum = ""
    in_struct = False
    depth = 0  # brace depth inside `struct BaseSystemContext`
    header_re = re.compile(rf"struct\s+{re.escape(struct_name)}\b")

    for raw in raw_lines:
        if not in_struct:
            if header_re.search(raw):
                in_struct = True
                # If the opening brace is on the same line, count it.
                depth = raw.count("{") - raw.count("}")
                if depth <= 0:
                    depth = 0  # brace likely on next line
            continue

        stripped = raw.strip()

        # Track brace depth BEFORE deciding what to do with this line.
        opens = raw.count("{")
        closes = raw.count("}")

        # End of the outer struct: depth reaches zero after subtracting
        # this line's closes.
        if depth + opens - closes <= 0 and ";" in stripped and "}" in stripped:
            if accum:
                logical.append(accum)
                accum = ""
            depth = 0
            break

        # Accumulate multi-line declarations (logical line without ';')
        if accum:
            accum = accum + " " + stripped
            if ";" in stripped:
                logical.append(accum)
                accum = ""
            depth += opens - closes
            continue

        # Start of a potential multi-line declaration (no ';' yet)
        if (stripped and not stripped.startswith("//") and "{" not in stripped
                and "}" not in stripped and ";" not in stripped
                and not SECTION_RE.match(raw)):
            accum = raw
            depth += opens - closes  # usually a no-op here
            continue

        logical.append(raw)
        depth += opens - closes

    if accum:
        logical.append(accum)

    return logical


def _parse_struct_fields(raw_text, struct_name, section_prefix=""):
    """Parse a single `struct <struct_name> { ... };` block from raw_text.
    Returns a list of {section, type, name, default} records for leaf
    fields. The caller composes them into the final field list.
    """
    lines = raw_text.splitlines()
    logical = _preprocess_lines(lines, struct_name)

    current_section = section_prefix or "Uncategorized"
    fields = []
    for line in logical:
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            # Section header (may sit inside a comment)
            sec = SECTION_RE.match(line)
            if sec:
                name = sec.group(1).strip()
                current_section = f"{section_prefix} — {name}" if section_prefix else name
            continue
        # Skip access specifiers / structural tokens
        if stripped.startswith("{") or stripped.startswith("}") or stripped.startswith("struct"):
            continue
        if stripped in ("public:", "private:", "protected:"):
            continue

        # Nested sub-struct member with `{}` value-init — matched first
        # because FIELD_RE can't express the `{}` default.
        nm = NESTED_MEMBER_RE.match(line)
        if nm:
            fields.append({
                "section": current_section,
                "type": nm.group(1).strip(),
                "name": nm.group(2).strip(),
                "default": "{}",
            })
            continue

        fm = FUNC_RE.match(line)
        if fm:
            fields.append({
                "section": current_section,
                "type": fm.group(1).strip(),
                "name": fm.group(2).strip(),
                "default": None,
            })
            continue

        fm = FIELD_RE.match(line)
        if fm:
            ftype = fm.group(1).strip()
            fname = fm.group(2).strip()
            fdefault = fm.group(3).strip() if fm.group(3) else None
            fields.append({
                "section": current_section,
                "type": ftype,
                "name": fname,
                "default": fdefault,
            })
    return fields


def parse_base_system_context():
    """Parse BaseSystemContext.h → list of {section, type, name, default}.

    Recognises the nested sub-struct layout: when the
    outer BaseSystemContext declares a `<Name>Context <member>;` field,
    that sub-struct's own fields are parsed (from the same header) and
    substituted in-place, with a section prefix so their origin is
    clear in the generated docs. The total field count therefore
    reflects all *leaf* fields across every sub-struct — same number
    that plugin code actually touches.

    Recursively expands any nested type that has
    its own `struct` definition in the same header (not just the
    *Context naming convention). PerformanceContext nests
    PerformanceBudget + NetworkStats — both POD types whose own
    leaf fields would otherwise be silently dropped by the parser.
    Recursion depth is bounded by the set of types that actually
    exist as struct definitions in the file (no infinite loops).
    """
    path = ENGINE_ROOT / "BaseSystemContext.h"
    if not path.exists():
        return []

    raw_text = path.read_text(encoding="utf-8")

    # Build the set of types that have a `struct <Name> { ... }`
    # definition in this header. Anything outside this set is treated
    # as a primitive leaf even if its name happens to start with a
    # capital letter (e.g. external C types like VkBuffer).
    struct_def_re = re.compile(r"^\s*struct\s+([A-Z][A-Za-z0-9_]*)\s*\{", re.MULTILINE)
    known_structs = set(struct_def_re.findall(raw_text))

    def _expand(struct_name, section_prefix, visited):
        """Parse `struct_name`'s fields, recursing into any nested
        sub-struct that we've defined in this same header. `visited`
        guards against accidental cycles."""
        if struct_name in visited:
            return []
        visited = visited | {struct_name}
        recs = _parse_struct_fields(raw_text, struct_name,
                                    section_prefix=section_prefix)
        out = []
        for rec in recs:
            tname = rec["type"]
            if tname in known_structs and tname != struct_name:
                # Recurse — replace this rec with the leaves of its
                # sub-struct, prefixed for breadcrumb clarity.
                inner_section = f"{section_prefix} → {rec['name']} ({tname})" if section_prefix \
                                else f"{rec['name']} ({tname})"
                inner = _expand(tname, inner_section, visited)
                if inner:
                    out.extend(inner)
                    continue
            out.append(rec)
        return out

    return _expand("BaseSystemContext", "", set())

# ─── Git info ───────────────────────────────────────────────────────────

def git_short_hash():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=REPO_ROOT, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except Exception:
        return "unknown"

def git_branch():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=REPO_ROOT, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except Exception:
        return "unknown"

# ─── Markdown generation ────────────────────────────────────────────────

LAYER_STACK_ART = """\
+---------------------------------------------------------------+
|                      GAME  (consumer)                         |
|  SceneUBO   GameSystemContext   Game domain systems   DLLs    |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                   LAYER 6 -- Game Interface                   |
|  BaseSystemContext | IModularSystem | EngineSystem             |
|  SystemRegistry | DLLLoader | AssetWatcher | PipelineRegistry |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                   LAYER 5 -- Dev / Debug                      |
|  DevServer (TCP :9999 loopback)                               |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                   LAYER 4 -- Engine Services                  |
|  Input | InputAction | InputBindings | Camera | Audio         |
|  Config | Events | Components | ECS (EnTT)                    |
|  WorldStateIO | SceneStatePersistence | SceneStateVersioning  |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                   LAYER 3 -- Render Graph                     |
|  RenderGraph | BuiltinPasses | GraphResources | GpuProfiler  |
|  RenderPass | PostProcess | ShadowPass                       |
|  ImGuiLayer | AdminPanel | AdminPanelDecorations | UiStyle    |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                   LAYER 2 -- Vulkan Abstraction               |
|  VkContext | VkSwapchain | VkShader | VkBuffer | VkTexture    |
|  VkDescriptors | VkPipeline | VkMesh | VkAccelStructure       |
|  VkRTPipeline | VkComputePipeline | VkClusterAS               |
|  PipelineRegistry | DlssWrapper | VkNtcTexture                |
+-------------------------------+-------------------------------+
                                |
+-------------------------------v-------------------------------+
|                   LAYER 1 -- Platform                         |
|  Window (GLFW) | Types (glm, VkTypes) | QualityPresets        |
+---------------------------------------------------------------+
"""

def generate_md(config, modules, fields, headers, sources):
    layers = config["layers"]
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    commit = git_short_hash()
    branch = git_branch()

    by_layer = defaultdict(list)
    for m in modules.values():
        by_layer[m["layer"]].append(m)
    for v in by_layer.values():
        v.sort(key=lambda m: m["header"])

    lines = []
    w = lines.append

    w("# StratumV Engine Reference")
    w("")
    w(f"> Auto-generated by `tools/engine_ref.py` on {now}")
    w(f"> Branch: `{branch}` | Commit: `{commit}`")
    w(f"> Total: {len(headers)} headers, {len(sources)} sources, "
      f"{len(headers) + len(sources)} files, {len(modules)} modules")
    w("")
    w("---")
    w("")

    # Layer stack
    w("## Layer Stack")
    w("")
    w("```")
    w(LAYER_STACK_ART.strip())
    w("```")
    w("")
    w("---")
    w("")

    # Modules by layer
    w("## Modules by Layer")
    w("")
    for layer_num in sorted(by_layer.keys()):
        if layer_num == 0:
            continue
        layer_info = layers.get(str(layer_num), {"name": "Unknown", "description": ""})
        mods = by_layer[layer_num]
        w(f"### Layer {layer_num} -- {layer_info['name']} ({len(mods)} modules)")
        w("")
        w(f"_{layer_info['description']}_")
        w("")
        w("| Module | Header | Source | Type |")
        w("|--------|--------|--------|------|")
        for m in mods:
            src = m["source"] if m["source"] else "--"
            mtype = "header-only" if m["header_only"] else "h+cpp"
            w(f"| {m['name']} | `{m['header']}` | `{src}` | {mtype} |")
        w("")

    # Unclassified
    if 0 in by_layer:
        w("### Unclassified")
        w("")
        w("| Module | Header |")
        w("|--------|--------|")
        for m in by_layer[0]:
            w(f"| {m['name']} | `{m['header']}` |")
        w("")

    w("---")
    w("")

    # BaseSystemContext API
    w("## BaseSystemContext API Reference")
    w("")
    w(f"Defined in `src/engine/BaseSystemContext.h` ({len(fields)} fields)")
    w("")

    if fields:
        current_sec = None
        for f in fields:
            if f["section"] != current_sec:
                if current_sec is not None:
                    w("")
                current_sec = f["section"]
                w(f"### {current_sec}")
                w("")
                w("| Type | Field | Default |")
                w("|------|-------|---------|")
            default = f["default"] if f["default"] else "--"
            # Truncate very long types for readability
            ftype = f["type"]
            if len(ftype) > 60:
                ftype = ftype[:57] + "..."
            w(f"| `{ftype}` | `{f['name']}` | `{default}` |")
        w("")

    w("---")
    w("")
    w("*End of generated reference.*")
    w("")

    return "\n".join(lines)


# ─── JSON generation ────────────────────────────────────────────────────

def generate_json(config, modules, fields, headers, sources):
    layers = config["layers"]
    by_layer = defaultdict(list)
    for m in modules.values():
        by_layer[m["layer"]].append(m["name"])

    layer_stats = {}
    for num, info in layers.items():
        mods = sorted(by_layer.get(int(num), []))
        layer_stats[num] = {
            "name": info["name"],
            "description": info["description"],
            "module_count": len(mods),
            "modules": mods,
        }

    # Group fields by section
    sections = []
    current_sec = None
    current_fields = []
    for f in fields:
        if f["section"] != current_sec:
            if current_sec is not None:
                sections.append({"name": current_sec, "fields": current_fields})
            current_sec = f["section"]
            current_fields = []
        current_fields.append({
            "type": f["type"],
            "name": f["name"],
            "default": f["default"],
        })
    if current_sec is not None:
        sections.append({"name": current_sec, "fields": current_fields})

    return {
        "generated": datetime.now(timezone.utc).isoformat(),
        "commit": git_short_hash(),
        "branch": git_branch(),
        "stats": {
            "total_files": len(headers) + len(sources),
            "headers": len(headers),
            "sources": len(sources),
            "modules": len(modules),
            "header_only_modules": sum(1 for m in modules.values() if m["header_only"]),
        },
        "layers": layer_stats,
        "base_system_context": {
            "total_fields": len(fields),
            "sections": sections,
        },
    }

# ─── Main ───────────────────────────────────────────────────────────────

def main():
    if not ENGINE_ROOT.exists():
        print(f"ERROR: engine root not found at {ENGINE_ROOT}", file=sys.stderr)
        sys.exit(1)
    if not CONFIG_PATH.exists():
        print(f"ERROR: config not found at {CONFIG_PATH}", file=sys.stderr)
        sys.exit(1)

    config = load_config()
    headers, sources = scan_engine_files()
    modules = build_module_map(headers, sources, config)
    fields = parse_base_system_context()

    # Ensure output dir exists
    OUT_MD.parent.mkdir(parents=True, exist_ok=True)

    md_content = generate_md(config, modules, fields, headers, sources)
    OUT_MD.write_text(md_content, encoding="utf-8")
    print(f"Wrote {OUT_MD}  ({len(md_content)} bytes)")

    json_content = generate_json(config, modules, fields, headers, sources)
    json_text = json.dumps(json_content, indent=2, ensure_ascii=False)
    OUT_JSON.write_text(json_text, encoding="utf-8")
    print(f"Wrote {OUT_JSON}  ({len(json_text)} bytes)")

    # Summary
    print(f"\nModules: {len(modules)}  |  Fields: {len(fields)}  |  "
          f"Files: {len(headers)}h + {len(sources)}cpp = {len(headers)+len(sources)}")

    unclassified = [m for m in modules.values() if m["layer"] == 0]
    if unclassified:
        print(f"\nWARNING: {len(unclassified)} unclassified module(s):")
        for m in unclassified:
            print(f"  - {m['header']}")

if __name__ == "__main__":
    main()
