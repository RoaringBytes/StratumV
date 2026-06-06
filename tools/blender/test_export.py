# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Test script — run inside Blender:
    blender --background --python tools/blender/test_export.py

Creates a test scene with a cube, a marker empty, and exports to .scene.json.
Validates the output format and coordinate conversion.
"""

import json
import math
import os
import sys
import tempfile

import bpy
from mathutils import Vector, Quaternion

# Ensure our addon package is importable
addon_dir = os.path.join(os.path.dirname(__file__))
if addon_dir not in sys.path:
    sys.path.insert(0, addon_dir)

import stratumv_exporter
from stratumv_exporter import coord_convert, scene_export, validation


def setup_test_scene():
    """Create a minimal scene: one cube mesh + one spawn-point empty."""
    # Clear default scene
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # Add a cube
    bpy.ops.mesh.primitive_cube_add(size=2, location=(3.0, -1.0, 5.0))
    cube = bpy.context.active_object
    cube.name = "TestCube"
    cube.scale = (1.0, 2.0, 0.5)

    # Add UV map
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.uv.smart_project()
    bpy.ops.object.mode_set(mode="OBJECT")

    # Add material with Principled BSDF
    mat = bpy.data.materials.new(name="TestMat")
    mat.use_nodes = True
    cube.data.materials.append(mat)

    # Add a marker empty
    bpy.ops.object.empty_add(type="PLAIN_AXES", location=(0.0, 0.0, 2.0))
    marker = bpy.context.active_object
    marker.name = "SP_PlayerStart"
    marker["team"] = "alpha"

    return cube, marker


def test_coord_conversion():
    """Verify Z-up → Y-up mapping."""
    print("\n=== Coordinate Conversion Tests ===")

    # Position: (3, -1, 5) → (3, 5, 1)
    pos = Vector((3.0, -1.0, 5.0))
    result = coord_convert.position_to_engine(pos)
    assert result == [3.0, 5.0, 1.0], f"Position failed: {result}"
    print(f"  Position (3,-1,5) → {result}  OK")

    # Identity quaternion stays identity
    q = Quaternion((1, 0, 0, 0))  # Blender: (w, x, y, z)
    result = coord_convert.quaternion_to_engine(q)
    # Output: [x, y, z, w] = [0, 0, 0, 1]
    assert result == [0.0, 0.0, 0.0, 1.0], f"Quat identity failed: {result}"
    print(f"  Quaternion identity → {result}  OK")

    # Scale: (1, 2, 0.5) → (1, 0.5, 2) — Y↔Z swap
    s = Vector((1.0, 2.0, 0.5))
    result = coord_convert.scale_to_engine(s)
    assert result == [1.0, 0.5, 2.0], f"Scale failed: {result}"
    print(f"  Scale (1,2,0.5) → {result}  OK")

    # 90° rotation around Blender Z-axis (engine Y-axis)
    q_z90 = Quaternion((0, 0, 1), math.radians(90))  # axis, angle
    result = coord_convert.quaternion_to_engine(q_z90)
    # Blender quat: (w=0.707, x=0, y=0, z=0.707)
    # Engine: [x=0, y=0.707, z=0, w=0.707]
    assert abs(result[1] - 0.7071) < 0.001, f"Quat Z90 y failed: {result}"
    assert abs(result[3] - 0.7071) < 0.001, f"Quat Z90 w failed: {result}"
    print(f"  Quaternion Z-90° → {[round(v,4) for v in result]}  OK")

    print("  All coordinate conversion tests PASSED")


def test_validation(cube):
    """Run validation on the test cube."""
    print("\n=== Validation Tests ===")
    messages = validation.validate_objects([cube])
    print(f"  Messages: {len(messages)}")
    for msg in messages:
        print(f"    {msg}")
    # Cube with UVs and no textures should pass clean
    assert not validation.has_errors(messages), "Unexpected validation errors"
    print("  Validation tests PASSED")


def test_export(cube, marker):
    """Export and validate the .scene.json output."""
    print("\n=== Export Tests ===")

    with tempfile.TemporaryDirectory() as tmpdir:
        scene_dict = scene_export.build_scene_dict(
            [cube], [marker], tmpdir, export_meshes=True
        )

        # Check structure
        assert scene_dict["version"] == 1
        assert scene_dict["coordinate_system"] == "y_up_right_handed"
        assert len(scene_dict["objects"]) == 1
        assert len(scene_dict["markers"]) == 1

        obj = scene_dict["objects"][0]
        assert obj["name"] == "TestCube"
        assert obj["mesh"].endswith(".glb")
        assert "transform" in obj
        assert len(obj["transform"]["position"]) == 3
        assert len(obj["transform"]["rotation"]) == 4
        assert len(obj["transform"]["scale"]) == 3
        print(f"  Object: {obj['name']}")
        print(f"    mesh: {obj['mesh']}")
        print(f"    position: {obj['transform']['position']}")
        print(f"    rotation: {[round(v,4) for v in obj['transform']['rotation']]}")
        print(f"    scale: {obj['transform']['scale']}")

        if "bounds" in obj:
            print(f"    bounds: {obj['bounds']}")

        if "material" in obj:
            print(f"    material: baseColor={obj['material']['baseColor']}")

        mkr = scene_dict["markers"][0]
        assert mkr["name"] == "SP_PlayerStart"
        assert mkr["type"] == "spawn_point"
        print(f"  Marker: {mkr['name']} (type={mkr['type']})")
        print(f"    position: {mkr['transform']['position']}")
        if "properties" in mkr:
            print(f"    properties: {mkr['properties']}")

        # Write and verify JSON is valid
        out_path = os.path.join(tmpdir, "test.scene.json")
        scene_export.write_scene_json(scene_dict, out_path)

        with open(out_path, "r") as f:
            loaded = json.load(f)
        assert loaded["version"] == 1
        print(f"\n  Written to: {out_path}")
        print(f"  JSON round-trip OK ({os.path.getsize(out_path)} bytes)")

    print("  Export tests PASSED")


if __name__ == "__main__":
    test_coord_conversion()
    cube, marker = setup_test_scene()
    test_validation(cube)
    test_export(cube, marker)
    print("\n=== ALL TESTS PASSED ===")
