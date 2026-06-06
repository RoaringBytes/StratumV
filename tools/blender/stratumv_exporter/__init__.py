# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""StratumV Blender Exporter — scene export addon for the StratumV engine.

Exports Blender scenes as .scene.json + per-object .glb files with:
- Z-up → Y-up coordinate conversion
- PBR material extraction (Principled BSDF → baseColor/metallic/roughness + texture paths)
- Hierarchy preservation (parent/child)
- Markers from empty objects (spawn points, triggers, audio zones)
- Asset validation (non-manifold, missing UVs, oversized textures)

The live-link panel adds operators that open a
plain-TCP connection to a running `skinned_test --editor-bridge-port`
instance and mirror transform edits in both directions. See
`live_link.py`.

A "Push Asset" operator + a parent-sync hook
forward Blender object `obj.parent` changes as ParentLink
SetField transactions.

A "Push Light" operator reads the active
Blender Light object and pushes its type + color + intensity + range
through the bridge as a LightComponent SetField.

"Push Camera" + "Push Material" operators
read the active Blender camera (or scene camera) and the active
mesh's first material slot and push them as CameraComponent +
MaterialComponent SetFields. Material sync is scoped to a single
basecolor + strength override (no full PBR per-entity material
rewrite).
"""

bl_info = {
    "name": "StratumV Scene Exporter",
    "author": "RoaringBytes",
    "version": (1, 4, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > StratumV",
    "description": ("Export scenes as .scene.json, push meshes + lights + "
                    "cameras + materials as replicated components, and open "
                    "a live-link connection to a running StratumV bridge"),
    "category": "Import-Export",
}

from . import operators, panels
from . import live_link


def menu_func_export(self, context):
    self.layout.operator(operators.STRATUMV_OT_export_scene.bl_idname,
                         text="StratumV Scene (.scene.json)")


def register():
    for cls in operators.classes:
        bpy.utils.register_class(cls)
    for cls in panels.classes:
        bpy.utils.register_class(cls)
    live_link.register()
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    live_link.unregister()
    for cls in reversed(panels.classes):
        bpy.utils.unregister_class(cls)
    for cls in reversed(operators.classes):
        bpy.utils.unregister_class(cls)


import bpy
