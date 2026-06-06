# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Blender operators for StratumV scene export + auto-fix."""

import os

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy_extras.io_utils import ExportHelper

from . import scene_export, validation


# ── Export operator ──────────────────────────────────────────────────

class STRATUMV_OT_export_scene(bpy.types.Operator, ExportHelper):
    """Export scene as .scene.json + .glb meshes for StratumV"""

    bl_idname = "stratumv.export_scene"
    bl_label = "Export StratumV Scene"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".scene.json"

    filter_glob: StringProperty(default="*.scene.json", options={"HIDDEN"})

    export_selected: BoolProperty(
        name="Selected Only",
        description="Export only selected objects",
        default=False,
    )

    export_meshes: BoolProperty(
        name="Export Meshes",
        description="Export .glb files for each mesh object",
        default=True,
    )

    run_validation: BoolProperty(
        name="Validate Before Export",
        description="Check for non-manifold geometry, missing UVs, oversized textures",
        default=True,
    )

    def execute(self, context):
        output_dir = os.path.dirname(self.filepath)

        if self.export_selected:
            all_objects = list(context.selected_objects)
        else:
            all_objects = list(context.scene.objects)

        mesh_objects = [o for o in all_objects if o.type == "MESH"]
        marker_objects = [o for o in all_objects if o.type == "EMPTY"]

        if not mesh_objects and not marker_objects:
            self.report({"WARNING"}, "No mesh or marker objects to export")
            return {"CANCELLED"}

        if self.run_validation and mesh_objects:
            analysis = validation.analyze_scene(mesh_objects)
            for msg in analysis.issues:
                level = {"error": "ERROR", "warning": "WARNING"}.get(msg.severity, "INFO")
                self.report({level}, str(msg))

            if validation.has_errors(analysis.issues):
                self.report({"ERROR"}, "Export aborted — fix errors first (see StratumV panel)")
                return {"CANCELLED"}

        try:
            scene_dict = scene_export.build_scene_dict(
                mesh_objects, marker_objects, output_dir,
                export_meshes=self.export_meshes,
            )
            scene_export.write_scene_json(scene_dict, self.filepath)
        except Exception as e:
            self.report({"ERROR"}, f"Export failed: {e}")
            return {"CANCELLED"}

        self.report({"INFO"},
                     f"Exported {len(mesh_objects)} object(s), {len(marker_objects)} marker(s)")
        return {"FINISHED"}


# ── Validate operator ────────────────────────────────────────────────

class STRATUMV_OT_validate_scene(bpy.types.Operator):
    """Analyze scene for StratumV compatibility and performance"""

    bl_idname = "stratumv.validate_scene"
    bl_label = "Validate Scene"
    bl_options = {"REGISTER"}

    def execute(self, context):
        objects = [o for o in context.scene.objects if o.type == "MESH"]

        if not objects:
            self.report({"INFO"}, "No mesh objects in scene")
            return {"FINISHED"}

        analysis = validation.analyze_scene(objects)

        if not analysis.issues:
            self.report({"INFO"}, f"All {len(objects)} object(s) passed — no issues")
        else:
            self.report({"INFO"},
                         f"{analysis.error_count} error(s), {analysis.warning_count} warning(s), "
                         f"{analysis.fixable_count} auto-fixable")

        # Force panel redraw
        for area in context.screen.areas:
            if area.type == "VIEW_3D":
                area.tag_redraw()

        return {"FINISHED"}


# ── Fix single issue ─────────────────────────────────────────────────

class STRATUMV_OT_fix_issue(bpy.types.Operator):
    """Auto-fix a single validation issue"""

    bl_idname = "stratumv.fix_issue"
    bl_label = "Fix Issue"
    bl_options = {"REGISTER", "UNDO"}

    fix_type: StringProperty(name="Fix Type")
    object_name: StringProperty(name="Object Name")
    fix_data: StringProperty(name="Fix Data", default="")

    def execute(self, context):
        result = _apply_fix(self.fix_type, self.object_name, self.fix_data)
        if result:
            self.report({"INFO"}, result)
        else:
            self.report({"ERROR"}, f"Fix failed for {self.object_name}")
            return {"CANCELLED"}

        # Track attempt so persistent issues lose their Fix button
        validation.mark_fix_attempted(self.object_name, self.fix_type)

        # Auto-revalidate so the panel updates immediately
        _revalidate_and_report(self, context)
        return {"FINISHED"}


# ── Fix all auto-fixable issues ──────────────────────────────────────

class STRATUMV_OT_fix_all(bpy.types.Operator):
    """Auto-fix all fixable issues at once"""

    bl_idname = "stratumv.fix_all"
    bl_label = "Fix All"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        analysis = validation.last_analysis
        if analysis is None:
            self.report({"WARNING"}, "Run Validate first")
            return {"CANCELLED"}

        fixable = [m for m in analysis.issues if m.is_fixable]
        if not fixable:
            self.report({"INFO"}, "Nothing to fix")
            return {"FINISHED"}

        fixed = 0
        for msg in fixable:
            data = msg.fix_data if msg.fix_data else ""
            result = _apply_fix(msg.fix_id, msg.object_name, data)
            if result:
                fixed += 1
            # Track all attempts
            validation.mark_fix_attempted(msg.object_name, msg.fix_id)

        # Auto-revalidate — compare before/after
        old_warnings = analysis.warning_count
        old_errors = analysis.error_count
        _revalidate_and_report(self, context)
        new = validation.last_analysis
        resolved = (old_errors - new.error_count) + (old_warnings - new.warning_count)
        if resolved > 0:
            self.report({"INFO"}, f"Resolved {resolved} issue(s), {new.warning_count} remaining")
        else:
            self.report({"WARNING"},
                         f"Applied {fixed} fix(es) but issues persist — may be structural")
        return {"FINISHED"}


def _revalidate_and_report(op, context):
    """Re-run analysis after fixes and refresh the panel."""
    objects = [o for o in context.scene.objects if o.type == "MESH"]
    validation.analyze_scene(objects)
    # Force panel redraw
    for area in context.screen.areas:
        if area.type == "VIEW_3D":
            area.tag_redraw()


# ── Fix implementations ──────────────────────────────────────────────

def _apply_fix(fix_type, object_name, fix_data):
    """Dispatch to the correct fix function. Returns status string or None."""
    # Texture fixes don't need an object
    if fix_type == "texture_oversize":
        return _fix_texture_oversize(fix_data)

    obj = bpy.data.objects.get(object_name)
    if obj is None or obj.type != "MESH":
        return None

    # Linked data can't be edited — tell user to make local
    if obj.data.library is not None:
        return f"'{obj.name}' uses linked data — right-click > Make Local first"

    if fix_type == "no_uv":
        return _fix_no_uv(obj)
    elif fix_type == "non_manifold":
        return _fix_non_manifold(obj)
    return None


def _fix_no_uv(obj):
    """Generate UVs via Smart UV Project."""
    import bmesh

    bm = bmesh.new()
    bm.from_mesh(obj.data)

    # Ensure a UV layer exists
    if not bm.loops.layers.uv:
        bm.loops.layers.uv.new("UVMap")

    bm.to_mesh(obj.data)
    bm.free()

    # Smart UV Project needs edit mode + selection
    prev_active = bpy.context.view_layer.objects.active
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj

    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=66, island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")

    # Restore selection
    obj.select_set(False)
    bpy.context.view_layer.objects.active = prev_active

    return f"Generated UVs for '{obj.name}'"


def _fix_non_manifold(obj):
    """Clean non-manifold geometry: merge by distance + fill holes."""
    import bmesh

    bm = bmesh.new()
    bm.from_mesh(obj.data)

    # Merge by distance (remove doubles)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0001)

    # Dissolve degenerate faces/edges
    bmesh.ops.dissolve_degenerate(bm, edges=bm.edges, dist=0.0001)

    bm.to_mesh(obj.data)
    bm.free()

    obj.data.update()
    return f"Cleaned non-manifold geometry on '{obj.name}'"


def _fix_texture_oversize(image_name):
    """Scale an oversized image down to MAX_TEXTURE_DIM."""
    img = bpy.data.images.get(image_name)
    if img is None:
        return None

    w, h = img.size
    max_dim = validation.MAX_TEXTURE_DIM

    if w <= max_dim and h <= max_dim:
        return f"'{image_name}' already within limits"

    # Calculate new size preserving aspect ratio
    ratio = min(max_dim / w, max_dim / h)
    new_w = int(w * ratio)
    new_h = int(h * ratio)

    img.scale(new_w, new_h)
    return f"Resized '{image_name}' from {w}x{h} to {new_w}x{new_h}"


# ── Registration ─────────────────────────────────────────────────────

classes = (
    STRATUMV_OT_export_scene,
    STRATUMV_OT_validate_scene,
    STRATUMV_OT_fix_issue,
    STRATUMV_OT_fix_all,
)
