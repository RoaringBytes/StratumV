# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Asset validation and performance analysis for StratumV Blender export.

Checks:
- Non-manifold geometry          (auto-fixable)
- Missing UV maps                (auto-fixable)
- Oversized textures >4096       (auto-fixable)
- Per-object triangle budget
- Scene-wide triangle budget
- Draw call estimate
- Texture memory estimate
- Duplicate mesh detection
"""

import bpy

# ── Budgets (sensible game-engine defaults) ──────────────────────────
MAX_TEXTURE_DIM         = 4096
TRIS_PER_OBJECT_WARN    = 100_000
TRIS_SCENE_BUDGET       = 500_000
DRAW_CALL_WARN          = 100
TEXTURE_MEMORY_WARN_MB  = 256.0


# ── Data classes ─────────────────────────────────────────────────────

class ValidationMessage:
    """Single validation finding."""

    __slots__ = ("severity", "object_name", "message", "fix_id", "fix_data", "fix_attempted")

    def __init__(self, severity, object_name, message, fix_id=None, fix_data=None):
        self.severity = severity       # "error" | "warning" | "info"
        self.object_name = object_name
        self.message = message
        self.fix_id = fix_id           # None = manual, str = auto-fixable
        self.fix_data = fix_data       # Extra data for the fix operator
        self.fix_attempted = False     # Set True after fix tried but issue persists

    @property
    def is_fixable(self):
        return self.fix_id is not None

    def __str__(self):
        tag = " [FIX]" if self.is_fixable else ""
        return f"[{self.severity.upper()}] {self.object_name}: {self.message}{tag}"


class SceneAnalysis:
    """Aggregate scene performance stats + per-issue list."""

    def __init__(self):
        self.total_tris = 0
        self.total_objects = 0
        self.draw_call_estimate = 0
        self.texture_memory_mb = 0.0
        self.unique_materials = 0
        self.shared_mesh_groups = {}   # mesh_data_name → [obj_names]
        self.issues = []               # list[ValidationMessage]

    @property
    def error_count(self):
        return sum(1 for m in self.issues if m.severity == "error")

    @property
    def warning_count(self):
        return sum(1 for m in self.issues if m.severity == "warning")

    @property
    def fixable_count(self):
        return sum(1 for m in self.issues if m.is_fixable)


# ── Module-level result store (panels read this) ────────────────────

last_analysis = None   # type: SceneAnalysis | None
_attempted_fixes = set()   # (object_name, fix_id) pairs already tried


def mark_fix_attempted(object_name, fix_id):
    """Record that a fix was attempted (called by operators after apply)."""
    _attempted_fixes.add((object_name, fix_id))


def clear_fix_history():
    """Reset attempted-fix tracking (e.g., after scene reload)."""
    _attempted_fixes.clear()


# ── Per-object checks ───────────────────────────────────────────────

def _check_non_manifold(obj):
    import bmesh
    msgs = []
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.edges.ensure_lookup_table()
    non_manifold = [e for e in bm.edges if not e.is_manifold]
    if non_manifold:
        linked = obj.data.library is not None
        suffix = " (linked — Make Local first)" if linked else ""
        msgs.append(ValidationMessage(
            "warning", obj.name,
            f"{len(non_manifold)} non-manifold edge(s){suffix}",
            fix_id="non_manifold" if not linked else None,
        ))
    bm.free()
    return msgs


def _check_uvs(obj):
    msgs = []
    if not obj.data.uv_layers:
        linked = obj.data.library is not None
        suffix = " (linked — Make Local first)" if linked else ""
        msgs.append(ValidationMessage(
            "error", obj.name,
            f"No UV map — textures won't map correctly{suffix}",
            fix_id="no_uv" if not linked else None,
        ))
    return msgs


def _check_textures_per_object(obj, seen_images):
    """Check texture dimensions; also accumulates image set for memory calc."""
    msgs = []
    for slot in obj.material_slots:
        mat = slot.material
        if mat is None or not mat.use_nodes:
            continue
        for node in mat.node_tree.nodes:
            if node.type != "TEX_IMAGE" or node.image is None:
                continue
            img = node.image
            seen_images.add(img.name)
            w, h = img.size
            if w > MAX_TEXTURE_DIM or h > MAX_TEXTURE_DIM:
                msgs.append(ValidationMessage(
                    "warning", obj.name,
                    f"Texture '{img.name}' is {w}x{h} (max {MAX_TEXTURE_DIM})",
                    fix_id="texture_oversize",
                    fix_data=img.name,
                ))
    return msgs


def _check_tri_count(obj):
    msgs = []
    me = obj.data
    me.calc_loop_triangles()
    tris = len(me.loop_triangles)
    if tris > TRIS_PER_OBJECT_WARN:
        msgs.append(ValidationMessage(
            "warning", obj.name,
            f"{tris:,} triangles — consider decimation (budget: {TRIS_PER_OBJECT_WARN:,})",
        ))
    return msgs, tris


# ── Scene-wide checks ───────────────────────────────────────────────

def _calc_texture_memory(images):
    """Estimate GPU texture memory for a set of images (with mipmaps)."""
    total = 0.0
    for name in images:
        img = bpy.data.images.get(name)
        if img is None:
            continue
        w, h = img.size
        bpp = 4  # RGBA8
        base = w * h * bpp
        with_mips = base * 1.33  # mipmap chain ≈ +33%
        total += with_mips
    return total / (1024 * 1024)  # MB


def _find_shared_meshes(objects):
    """Group objects by shared mesh data — detect instancing opportunities."""
    groups = {}
    for obj in objects:
        mesh_name = obj.data.name
        groups.setdefault(mesh_name, []).append(obj.name)
    return groups


def _estimate_draw_calls(objects):
    """Estimate draw calls: unique (mesh_data, material) pairs."""
    pairs = set()
    for obj in objects:
        mesh_name = obj.data.name
        for slot in obj.material_slots:
            mat_name = slot.material.name if slot.material else "__none__"
            pairs.add((mesh_name, mat_name))
        if not obj.material_slots:
            pairs.add((mesh_name, "__none__"))
    return len(pairs)


# ── Rig helper filter ────────────────────────────────────────────────

def _is_rig_helper(obj):
    """Return True if this mesh is a rig control shape (not a real asset).

    Detects: cs_* prefix (Rigify), WGT-* prefix (Rigify weights), objects
    parented to an Armature with hide_render=True, or zero-area meshes used
    as custom bone shapes.
    """
    name = obj.name
    # Common rig control shape prefixes
    if name.startswith(("cs_", "CS_", "WGT-", "WGT_", "wgt_")):
        return True
    # Hidden from render = not a real game asset
    if obj.hide_render:
        return True
    # Custom bone shape reference (assigned as bone.custom_shape on an armature)
    if obj.parent and obj.parent.type == "ARMATURE":
        for bone in obj.parent.pose.bones:
            if bone.custom_shape == obj:
                return True
    return False


# ── Main entry point ────────────────────────────────────────────────

def analyze_scene(objects):
    """Run all validation + performance checks on scene objects.

    Stores result in module-level `last_analysis` so panels can read it.

    Args:
        objects: Iterable of bpy.types.Object (meshes only).

    Returns:
        SceneAnalysis with issues and stats.
    """
    global last_analysis

    mesh_objects = [o for o in objects if o.type == "MESH" and not _is_rig_helper(o)]
    analysis = SceneAnalysis()
    analysis.total_objects = len(mesh_objects)
    seen_images = set()

    # Per-object checks
    for obj in mesh_objects:
        analysis.issues.extend(_check_non_manifold(obj))
        analysis.issues.extend(_check_uvs(obj))
        analysis.issues.extend(_check_textures_per_object(obj, seen_images))

        tri_msgs, tris = _check_tri_count(obj)
        analysis.issues.extend(tri_msgs)
        analysis.total_tris += tris

    # Scene-wide checks
    analysis.draw_call_estimate = _estimate_draw_calls(mesh_objects)
    analysis.texture_memory_mb = _calc_texture_memory(seen_images)
    analysis.shared_mesh_groups = _find_shared_meshes(mesh_objects)
    analysis.unique_materials = len(set(
        slot.material.name
        for o in mesh_objects for slot in o.material_slots
        if slot.material
    ))

    # Scene budget warnings
    if analysis.total_tris > TRIS_SCENE_BUDGET:
        analysis.issues.append(ValidationMessage(
            "warning", "(scene)",
            f"Total {analysis.total_tris:,} tris exceeds budget of {TRIS_SCENE_BUDGET:,}",
        ))

    if analysis.draw_call_estimate > DRAW_CALL_WARN:
        analysis.issues.append(ValidationMessage(
            "warning", "(scene)",
            f"~{analysis.draw_call_estimate} draw calls — consider merging objects with shared materials",
        ))

    if analysis.texture_memory_mb > TEXTURE_MEMORY_WARN_MB:
        analysis.issues.append(ValidationMessage(
            "warning", "(scene)",
            f"Texture memory ~{analysis.texture_memory_mb:.0f} MB exceeds {TEXTURE_MEMORY_WARN_MB:.0f} MB budget",
        ))

    # Duplicate mesh opportunities
    for mesh_name, obj_names in analysis.shared_mesh_groups.items():
        if len(obj_names) > 1:
            analysis.issues.append(ValidationMessage(
                "info", "(scene)",
                f"'{mesh_name}' shared by {len(obj_names)} objects — good for instancing",
            ))

    # Mark issues that persist after a previous fix attempt
    for msg in analysis.issues:
        if msg.fix_id and (msg.object_name, msg.fix_id) in _attempted_fixes:
            msg.fix_attempted = True
            msg.message += " (structural — manual fix needed)"
            msg.fix_id = None  # Remove Fix button

    # Sort: errors → warnings → info
    severity_order = {"error": 0, "warning": 1, "info": 2}
    analysis.issues.sort(key=lambda m: severity_order.get(m.severity, 3))

    last_analysis = analysis
    return analysis


# ── Legacy API (used by export operator) ─────────────────────────────

def validate_objects(objects):
    """Run analysis, return just the issues list."""
    analysis = analyze_scene(objects)
    return analysis.issues


def has_errors(messages):
    return any(m.severity == "error" for m in messages)
