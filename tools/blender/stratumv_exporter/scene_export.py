# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Core scene export logic: Blender scene → .scene.json + per-object .glb files."""

import json
import os
from datetime import datetime, timezone

import bpy
from mathutils import Vector

from . import coord_convert

# Schema version — increment on breaking format changes
SCENE_FORMAT_VERSION = 1
ADDON_VERSION = "1.0.0"


def _collect_custom_properties(obj):
    """Extract user-defined custom properties (skip Blender internals)."""
    props = {}
    for key in obj.keys():
        if key.startswith("_"):
            continue
        val = obj[key]
        # Only serialize JSON-safe types
        if isinstance(val, (int, float, str, bool)):
            props[key] = val
        elif hasattr(val, "to_list"):
            props[key] = val.to_list()
    return props


def _get_marker_type(obj):
    """Determine marker type from custom property or name prefix.

    Convention: set custom property 'marker_type' or use name prefix:
      SP_  → spawn_point
      TRG_ → trigger
      AZ_  → audio_zone
      NR_  → nav_region
      LP_  → light_probe
    """
    if "marker_type" in obj:
        return str(obj["marker_type"])

    prefix_map = {
        "SP_": "spawn_point",
        "TRG_": "trigger",
        "AZ_": "audio_zone",
        "NR_": "nav_region",
        "LP_": "light_probe",
    }
    for prefix, mtype in prefix_map.items():
        if obj.name.startswith(prefix):
            return mtype
    return "custom"


def _extract_material(obj):
    """Extract PBR material properties from the first material slot.

    Reads from Principled BSDF node if available, falls back to defaults.
    Returns dict matching the Material schema.
    """
    if not obj.material_slots:
        return None

    mat = obj.material_slots[0].material
    if mat is None:
        return None

    result = {
        "baseColor": [1.0, 1.0, 1.0, 1.0],
        "metallic": 0.0,
        "roughness": 1.0,
    }

    if not mat.use_nodes:
        return result

    # Find Principled BSDF
    principled = None
    for node in mat.node_tree.nodes:
        if node.type == "BSDF_PRINCIPLED":
            principled = node
            break

    if principled is None:
        return result

    # Base color factor
    bc_input = principled.inputs.get("Base Color")
    if bc_input and hasattr(bc_input, "default_value"):
        c = bc_input.default_value
        result["baseColor"] = [c[0], c[1], c[2], c[3] if len(c) > 3 else 1.0]

    # Metallic
    met_input = principled.inputs.get("Metallic")
    if met_input and hasattr(met_input, "default_value"):
        result["metallic"] = float(met_input.default_value)

    # Roughness
    rough_input = principled.inputs.get("Roughness")
    if rough_input and hasattr(rough_input, "default_value"):
        result["roughness"] = float(rough_input.default_value)

    # Texture paths
    textures = {}
    tex_socket_map = {
        "Base Color": "baseColor",
        "Normal": "normal",
        "Metallic": "metallicRoughness",
        "Emission Color": "emissive",
    }

    for socket_name, tex_type in tex_socket_map.items():
        inp = principled.inputs.get(socket_name)
        if inp is None or not inp.links:
            continue
        from_node = inp.links[0].from_node

        # Handle normal map node (has an intermediate Normal Map node)
        if socket_name == "Normal" and from_node.type == "NORMAL_MAP":
            color_inp = from_node.inputs.get("Color")
            if color_inp and color_inp.links:
                from_node = color_inp.links[0].from_node

        if from_node.type == "TEX_IMAGE" and from_node.image:
            img = from_node.image
            if img.filepath:
                textures[tex_type] = bpy.path.basename(img.filepath)
            else:
                textures[tex_type] = img.name

    if textures:
        result["textures"] = textures

    return result


def _build_hierarchy(objects):
    """Build parent/children maps for a set of objects.

    Returns:
        parent_map: dict[obj_name] → parent_name or None
        children_map: dict[obj_name] → list of child names
    """
    obj_set = set(o.name for o in objects)
    parent_map = {}
    children_map = {o.name: [] for o in objects}

    for obj in objects:
        if obj.parent and obj.parent.name in obj_set:
            parent_map[obj.name] = obj.parent.name
            children_map[obj.parent.name].append(obj.name)
        else:
            parent_map[obj.name] = None

    return parent_map, children_map


def export_glb(obj, output_dir):
    """Export a single mesh object as .glb using Blender's built-in glTF exporter.

    The glTF exporter handles Z-up → Y-up for mesh data internally.
    The object is exported at the origin (transforms are in .scene.json).

    Returns:
        Relative filename (e.g., "meshes/Building_01.glb").
    """
    meshes_dir = os.path.join(output_dir, "meshes")
    os.makedirs(meshes_dir, exist_ok=True)

    filename = f"{obj.name}.glb"
    filepath = os.path.join(meshes_dir, filename)

    # Deselect all, select only this object
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj

    # Parameter names vary across Blender versions — use only stable ones
    export_kwargs = {
        "filepath": filepath,
        "use_selection": True,
        "export_format": "GLB",
        "export_texcoords": True,
        "export_normals": True,
        "export_materials": "EXPORT",
    }
    # 'export_apply' replaced 'export_apply_modifiers' in Blender 4.x
    if bpy.app.version >= (4, 0, 0):
        export_kwargs["export_apply"] = True
    bpy.ops.export_scene.gltf(**export_kwargs)

    return f"meshes/{filename}"


def build_scene_dict(objects, markers, output_dir, export_meshes=True):
    """Build the .scene.json dict from Blender objects.

    Args:
        objects: List of mesh objects to export.
        markers: List of empty objects to export as markers.
        output_dir: Directory where .glb files are written.
        export_meshes: If True, export .glb files for each mesh.

    Returns:
        dict matching scene_json_schema.json.
    """
    parent_map, children_map = _build_hierarchy(objects + markers)

    scene_objects = []
    for obj in objects:
        # World-space transform → engine coordinates
        transform = coord_convert.matrix_to_engine(obj.matrix_world)

        entry = {
            "name": obj.name,
            "mesh": export_glb(obj, output_dir) if export_meshes else f"meshes/{obj.name}.glb",
            "transform": transform,
        }

        # Material
        mat = _extract_material(obj)
        if mat:
            entry["material"] = mat

        # Bounding box (world space)
        if obj.bound_box:
            # bound_box is in object-local space — transform to world
            world_corners = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
            bb_min, bb_max = coord_convert.bounds_to_engine(world_corners)
            entry["bounds"] = {"min": bb_min, "max": bb_max}

        # Hierarchy
        if parent_map.get(obj.name):
            entry["parent"] = parent_map[obj.name]
        if children_map.get(obj.name):
            entry["children"] = children_map[obj.name]

        # Custom properties
        custom = _collect_custom_properties(obj)
        if custom:
            entry["custom_properties"] = custom

        scene_objects.append(entry)

    # Markers (empties)
    scene_markers = []
    for obj in markers:
        transform = coord_convert.matrix_to_engine(obj.matrix_world)
        marker = {
            "name": obj.name,
            "type": _get_marker_type(obj),
            "transform": transform,
        }
        props = _collect_custom_properties(obj)
        # Remove marker_type from forwarded properties — already in 'type' field
        props.pop("marker_type", None)
        if props:
            marker["properties"] = props

        scene_markers.append(marker)

    return {
        "version": SCENE_FORMAT_VERSION,
        "generator": "StratumV Blender Exporter",
        "coordinate_system": "y_up_right_handed",
        "objects": scene_objects,
        "markers": scene_markers,
        "metadata": {
            "exported_at": datetime.now(timezone.utc).isoformat(),
            "blender_version": bpy.app.version_string,
            "scene_name": bpy.context.scene.name,
            "addon_version": ADDON_VERSION,
        },
    }


def write_scene_json(scene_dict, filepath):
    """Write scene dict to .scene.json with pretty formatting."""
    with open(filepath, "w", encoding="utf-8") as f:
        json.dump(scene_dict, f, indent=2, ensure_ascii=False)
        f.write("\n")
