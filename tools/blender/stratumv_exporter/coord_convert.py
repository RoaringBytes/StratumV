# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Blender Z-up → StratumV Y-up coordinate conversion.

Blender:  X=right, Y=forward,  Z=up   (right-handed)
Engine:   X=right, Y=up,       Z=back (right-handed, same as glTF)

Mapping: engine = (blender_x, blender_z, -blender_y)
"""

from mathutils import Vector, Quaternion, Matrix


def position_to_engine(pos):
    """Convert Blender position (x,y,z) → engine (x,z,-y)."""
    return [pos.x, pos.z, -pos.y]


def quaternion_to_engine(q):
    """Convert Blender quaternion [w,x,y,z] → engine [x,y,z,w].

    Axis transform: (qx, qy, qz) → (qx, qz, -qy), w unchanged.
    Output order is [x,y,z,w] to match glTF / glm convention.
    """
    return [q.x, q.z, -q.y, q.w]


def scale_to_engine(s):
    """Convert Blender scale (x,y,z) → engine (x,z,y).

    Scale components are magnitudes — no negation needed.
    """
    return [s.x, s.z, s.y]


def bounds_to_engine(bb_corners):
    """Convert Blender bounding box corners to engine-space AABB.

    Args:
        bb_corners: List of 8 Vector corners from obj.bound_box (world space).

    Returns:
        (min_vec, max_vec) as lists of 3 floats in engine space.
    """
    converted = [position_to_engine(Vector(c)) for c in bb_corners]
    xs = [c[0] for c in converted]
    ys = [c[1] for c in converted]
    zs = [c[2] for c in converted]
    return [min(xs), min(ys), min(zs)], [max(xs), max(ys), max(zs)]


def matrix_to_engine(mat):
    """Convert a 4x4 Blender world matrix to engine-space TRS components.

    Decomposes the matrix to translation, rotation, scale and converts each.

    Returns:
        dict with 'position', 'rotation' [x,y,z,w], 'scale'.
    """
    loc, rot, scl = mat.decompose()
    return {
        "position": position_to_engine(loc),
        "rotation": quaternion_to_engine(rot),
        "scale": scale_to_engine(scl),
    }
