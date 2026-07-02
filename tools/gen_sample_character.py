# Generates assets/sample_character.fbx — a minimal rigged/skinned humanoid
# used by the golden-image render-regression suite (tests/golden).
#
# The asset is procedurally generated (CC0, no external IP). Regenerate with:
#   blender -b --factory-startup -P tools/gen_sample_character.py -- assets/sample_character.fbx
# then re-bake references:  skinned_test --render-golden tests/golden/
#
# Joint names follow the plain-Mixamo convention the lab harness probes for
# (Hips/Spine/Spine1/Head, Left*/Right* UpLeg/Leg/Foot, Arm/ForeArm/Hand).
import bpy, sys, math

out_path = sys.argv[sys.argv.index("--") + 1]

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 30

# ---- Armature ----------------------------------------------------------
arm_data = bpy.data.armatures.new("Armature")
arm_obj = bpy.data.objects.new("Armature", arm_data)
scene.collection.objects.link(arm_obj)
bpy.context.view_layer.objects.active = arm_obj
bpy.ops.object.mode_set(mode="EDIT")

# name: (head, tail, parent)
BONES = {
    "Hips":        ((0.00, 0.0, 0.90), (0.00, 0.0, 1.00), None),
    "Spine":       ((0.00, 0.0, 1.00), (0.00, 0.0, 1.15), "Hips"),
    "Spine1":      ((0.00, 0.0, 1.15), (0.00, 0.0, 1.30), "Spine"),
    "Head":        ((0.00, 0.0, 1.30), (0.00, 0.0, 1.55), "Spine1"),
    "LeftUpLeg":   ((0.10, 0.0, 0.90), (0.10, 0.0, 0.50), "Hips"),
    "LeftLeg":     ((0.10, 0.0, 0.50), (0.10, 0.0, 0.10), "LeftUpLeg"),
    "LeftFoot":    ((0.10, 0.0, 0.10), (0.10, -0.18, 0.03), "LeftLeg"),
    "RightUpLeg":  ((-0.10, 0.0, 0.90), (-0.10, 0.0, 0.50), "Hips"),
    "RightLeg":    ((-0.10, 0.0, 0.50), (-0.10, 0.0, 0.10), "RightUpLeg"),
    "RightFoot":   ((-0.10, 0.0, 0.10), (-0.10, -0.18, 0.03), "RightLeg"),
    "LeftArm":     ((0.17, 0.0, 1.27), (0.42, 0.0, 1.27), "Spine1"),
    "LeftForeArm": ((0.42, 0.0, 1.27), (0.66, 0.0, 1.27), "LeftArm"),
    "LeftHand":    ((0.66, 0.0, 1.27), (0.80, 0.0, 1.27), "LeftForeArm"),
    "RightArm":    ((-0.17, 0.0, 1.27), (-0.42, 0.0, 1.27), "Spine1"),
    "RightForeArm":((-0.42, 0.0, 1.27), (-0.66, 0.0, 1.27), "RightArm"),
    "RightHand":   ((-0.66, 0.0, 1.27), (-0.80, 0.0, 1.27), "RightForeArm"),
}
ebs = arm_data.edit_bones
created = {}
for name, (head, tail, parent) in BONES.items():
    b = ebs.new(name)
    b.head, b.tail = head, tail
    if parent:
        b.parent = created[parent]
    created[name] = b
bpy.ops.object.mode_set(mode="OBJECT")

# ---- Body mesh: one box per bone segment, joined ------------------------
def add_box(name, center, size):
    bpy.ops.mesh.primitive_cube_add(location=center)
    ob = bpy.context.active_object
    ob.name = name
    ob.scale = size
    bpy.ops.object.transform_apply(scale=True)
    return ob

parts = []
for name, (head, tail, _p) in BONES.items():
    cx = [(h + t) / 2.0 for h, t in zip(head, tail)]
    length = math.dist(head, tail)
    # slim boxes along the bone; torso/head slightly fatter
    if name in ("Hips", "Spine", "Spine1"):
        size = (0.16, 0.10, max(length / 2, 0.05))
    elif name == "Head":
        size = (0.09, 0.10, length / 2)
    elif "Foot" in name:
        size = (0.05, 0.11, 0.035)
    elif "Arm" in name or "Hand" in name:
        size = (max(length / 2, 0.05), 0.04, 0.04)
    else:  # legs
        size = (0.055, 0.055, max(length / 2, 0.05))
    parts.append(add_box("part_" + name, cx, size))

for ob in bpy.context.selected_objects:
    ob.select_set(False)
for ob in parts:
    ob.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
body = bpy.context.active_object
body.name = "SampleCharacter"

# Simple material so loaders that expect >=1 material are satisfied
mat = bpy.data.materials.new("SampleCharacterMat")
mat.use_nodes = True
bsdf = mat.node_tree.nodes.get("Principled BSDF")
if bsdf:
    bsdf.inputs["Base Color"].default_value = (0.55, 0.6, 0.7, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.8
body.data.materials.append(mat)

# ---- Skin with automatic weights ----------------------------------------
for ob in bpy.context.selected_objects:
    ob.select_set(False)
body.select_set(True)
arm_obj.select_set(True)
bpy.context.view_layer.objects.active = arm_obj
bpy.ops.object.parent_set(type="ARMATURE_AUTO")

# ---- Tiny "Idle" animation (hip sway + arm swing, 1s loop) ---------------
bpy.context.view_layer.objects.active = arm_obj
bpy.ops.object.mode_set(mode="POSE")
action = bpy.data.actions.new("Idle")
if arm_obj.animation_data is None:
    arm_obj.animation_data_create()
arm_obj.animation_data.action = action
try:  # Blender 4.4+/5.x slotted actions
    slot = action.slots.new(id_type='OBJECT', name="Idle")
    arm_obj.animation_data.action_slot = slot
except Exception:
    pass

def key_rot(bone_name, frame, euler_xyz):
    pb = arm_obj.pose.bones[bone_name]
    pb.rotation_mode = "XYZ"
    pb.rotation_euler = euler_xyz
    pb.keyframe_insert(data_path="rotation_euler", frame=frame)

d = math.radians(4.0)
for f, s in ((1, 1.0), (15, -1.0), (30, 1.0)):
    key_rot("Hips", f, (0, s * d * 0.5, 0))
    key_rot("LeftArm", f, (0, 0, s * d))
    key_rot("RightArm", f, (0, 0, -s * d))
bpy.ops.object.mode_set(mode="OBJECT")
scene.frame_start, scene.frame_end = 1, 30

# ---- Export --------------------------------------------------------------
bpy.ops.export_scene.fbx(
    filepath=out_path,
    use_selection=False,
    add_leaf_bones=False,
    bake_anim=True,
    bake_anim_use_all_actions=False,
    bake_anim_use_nla_strips=False,
    object_types={"ARMATURE", "MESH"},
    mesh_smooth_type="OFF",
    path_mode="STRIP",
)
print("[gen_sample_character] wrote", out_path)
