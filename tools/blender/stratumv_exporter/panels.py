# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Blender UI panels for StratumV scene export + validation results."""

import bpy

from . import validation


class STRATUMV_PT_export_panel(bpy.types.Panel):
    """StratumV export panel in the 3D Viewport sidebar."""

    bl_label = "StratumV Export"
    bl_idname = "STRATUMV_PT_export_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "StratumV"

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        # Scene stats
        mesh_count = sum(1 for o in scene.objects if o.type == "MESH")
        marker_count = sum(1 for o in scene.objects if o.type == "EMPTY")

        box = layout.box()
        box.label(text="Scene Summary", icon="SCENE_DATA")
        row = box.row()
        row.label(text=f"Meshes: {mesh_count}")
        row.label(text=f"Markers: {marker_count}")

        if context.selected_objects:
            sel_mesh = sum(1 for o in context.selected_objects if o.type == "MESH")
            sel_marker = sum(1 for o in context.selected_objects if o.type == "EMPTY")
            row = box.row()
            row.label(text=f"Selected: {sel_mesh}M / {sel_marker}E")

        layout.separator()
        layout.operator("stratumv.validate_scene", icon="CHECKMARK")
        layout.operator("stratumv.export_scene", icon="EXPORT")


class STRATUMV_PT_results_panel(bpy.types.Panel):
    """Validation results with performance stats and fix buttons."""

    bl_label = "Validation Results"
    bl_idname = "STRATUMV_PT_results_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "StratumV"
    bl_parent_id = "STRATUMV_PT_export_panel"
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return validation.last_analysis is not None

    def draw(self, context):
        layout = self.layout
        analysis = validation.last_analysis
        if analysis is None:
            return

        # ── Performance summary ──────────────────────────────────
        perf = layout.box()
        perf.label(text="Performance", icon="SORTTIME")

        col = perf.column(align=True)

        # Triangles
        tris = analysis.total_tris
        tris_icon = "ERROR" if tris > validation.TRIS_SCENE_BUDGET else "CHECKMARK"
        col.label(text=f"Triangles: {tris:,} / {validation.TRIS_SCENE_BUDGET:,}", icon=tris_icon)

        # Draw calls
        dc = analysis.draw_call_estimate
        dc_icon = "ERROR" if dc > validation.DRAW_CALL_WARN else "CHECKMARK"
        col.label(text=f"Draw calls: ~{dc} / {validation.DRAW_CALL_WARN}", icon=dc_icon)

        # Texture memory
        mem = analysis.texture_memory_mb
        mem_icon = "ERROR" if mem > validation.TEXTURE_MEMORY_WARN_MB else "CHECKMARK"
        col.label(text=f"Texture mem: ~{mem:.0f} MB / {validation.TEXTURE_MEMORY_WARN_MB:.0f} MB",
                  icon=mem_icon)

        # Materials
        col.label(text=f"Materials: {analysis.unique_materials}", icon="MATERIAL")

        # ── Issues list ──────────────────────────────────────────
        if not analysis.issues:
            layout.label(text="No issues found", icon="CHECKMARK")
            return

        issues_box = layout.box()
        header = issues_box.row()
        header.label(
            text=f"Issues: {analysis.error_count}E / {analysis.warning_count}W",
            icon="ERROR" if analysis.error_count > 0 else "INFO",
        )

        # Fix All button (only if there are fixable issues)
        if analysis.fixable_count > 0:
            row = issues_box.row()
            row.operator("stratumv.fix_all",
                         text=f"Fix All ({analysis.fixable_count})",
                         icon="TOOL_SETTINGS")

        issues_box.separator()

        # Per-issue rows
        for msg in analysis.issues:
            row = issues_box.row(align=True)

            # Severity icon
            if msg.severity == "error":
                icon = "CANCEL"
            elif msg.severity == "warning":
                icon = "ERROR"
            else:
                icon = "INFO"

            # Message text
            text = f"{msg.object_name}: {msg.message}"
            if len(text) > 60:
                text = text[:57] + "..."
            row.label(text=text, icon=icon)

            # Fix button or info hint
            if msg.is_fixable:
                op = row.operator("stratumv.fix_issue", text="Fix", icon="TOOL_SETTINGS")
                op.fix_type = msg.fix_id
                op.object_name = msg.object_name
                op.fix_data = msg.fix_data if msg.fix_data else ""


classes = (
    STRATUMV_PT_export_panel,
    STRATUMV_PT_results_panel,
)
