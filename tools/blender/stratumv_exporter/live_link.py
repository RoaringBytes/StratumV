# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Blender live-link panel + depsgraph hook.

This is the user-facing Blender side of the collaborative editing
substrate. When an artist clicks "Connect" in the StratumV sidebar,
the addon opens a plain-TCP connection to a running `skinned_test
--editor-bridge-port N` instance (see `net_client.py`) and mirrors
state both ways:

- Downstream (skinned_test → Blender): the bridge sends a Hello, the
  current server semver, and one EntityState per replicated entity.
  We cache those for display + track them as Blender scene objects
  named "StratumV.<entityId>.<label>" so the artist can see the
  collaborative world next to their own WIP.

- Upstream (Blender → skinned_test): we register a
  `bpy.app.handlers.depsgraph_update_post` callback. When the user
  moves / rotates / scales the linked avatar object, we rebuild a
  NetTransform from the object's world matrix, run it through the
  Blender-Z-up → StratumV-Y-up conversion in `coord_convert.py`, and
  push a MoveSelf frame down the bridge.

Scope constraints:
- Only the bridge's own avatar is editable from Blender (the
  server enforces owner-authority; a Blender client is conceptually
  the same entity as its paired skinned_test).
- Every edit carries `PermissionScope::Editor` — scope is chosen
  by the bridge, not by Blender.
- Drag a mesh / camera / light in Blender → only avatar transform
  is synced via the depsgraph hook. Full light/camera/material sync
  is handled by the dedicated push operators below.

Threading:
- `net_client.BridgeClient` runs its own background reader thread
  and all of its callbacks fire on that thread. Touching Blender
  state (`bpy.data`, `bpy.context`) from a non-main thread corrupts
  the depsgraph. So the callbacks on this module only touch mutex-
  protected snapshots; Blender-side mutation happens in the
  periodic `bpy.app.timers` callback `_pump_timer`.
"""

from __future__ import annotations

import math
import os
import sys
import tempfile
import threading
from typing import Dict, List, Optional

# Blender imports — these only exist when the addon is loaded inside
# Blender. The `try/except ImportError` guard lets this file compile
# cleanly under a plain CPython interpreter so the test harness can
# import the other helpers it needs from the addon package.
try:
    import bpy   # noqa: F401  (imported for the side effect of being
                 #              available in register/unregister)
    from bpy.props import (
        StringProperty,
        IntProperty,
        BoolProperty,
    )
    _HAVE_BPY = True
except ImportError:  # pragma: no cover — running under plain python
    _HAVE_BPY = False

# Sibling modules — relative import works both as a Blender addon
# package and as a loose script when `tools/blender/stratumv_exporter`
# is on sys.path.
try:
    from . import net_client
    from . import coord_convert
except ImportError:
    import net_client          # type: ignore
    import coord_convert       # type: ignore


# ── Module-level state ──────────────────────────────────────────────
#
# Blender addons run in a single Python process for the whole Blender
# session; there's only ever one live-link connection at a time. We
# stash the bridge client + caches as module globals guarded by a
# lock so the reader thread can mutate them without tripping on the
# main-thread timer.

_state_lock   = threading.Lock()
_bridge:      Optional[net_client.BridgeClient] = None
_entities:    Dict[int, net_client.EntityState] = {}
_last_hello:  Optional[net_client.HelloMessage] = None
_pending_ui_refresh = False
_status_line:         str = "Disconnected"
_status_detail:       str = ""
_moves_sent:          int = 0
_moves_ignored_self:  int = 0

# Push-operator counters
_assets_pushed:     int = 0
_parents_sent:      int = 0
_lights_sent:       int = 0
_cameras_sent:      int = 0
_materials_sent:    int = 0
_last_parent_entity: int = 0      # cached so we only fire when it changes

# Tracks the Blender object names we've created for remote entities
# so unregister / reconnect don't leave orphans behind.
_synced_object_names: Dict[int, str] = {}


# ── Small formatting helpers ────────────────────────────────────────

def _fmt_semver(packed: int) -> str:
    maj = (packed >> 16) & 0xFF
    mnr = (packed >>  8) & 0xFF
    pat = (packed       ) & 0xFF
    return f"{maj}.{mnr}.{pat}"


def _fmt_scope(raw: int) -> str:
    return {0: "Spectator", 1: "Player", 2: "Editor", 3: "Admin"}.get(
        raw, f"raw={raw}")


def _fmt_server_state(raw: int) -> str:
    return {
        net_client.SERVER_PENDING:  "pending",
        net_client.SERVER_TLS_OK:   "tls-ok",
        net_client.SERVER_WELCOMED: "welcomed",
    }.get(raw, f"raw={raw}")


# ── Bridge callbacks (reader thread) ────────────────────────────────
#
# Keep these tiny. Only touch the module-level caches, never bpy.

def _on_hello(hello: net_client.HelloMessage) -> None:
    global _last_hello, _pending_ui_refresh
    with _state_lock:
        _last_hello = hello
        _pending_ui_refresh = True


def _on_entity_state(state: net_client.EntityState) -> None:
    global _pending_ui_refresh
    with _state_lock:
        _entities[state.entity_id] = state
        _pending_ui_refresh = True


def _on_entity_gone(entity_id: int) -> None:
    global _pending_ui_refresh
    with _state_lock:
        _entities.pop(entity_id, None)
        _pending_ui_refresh = True


def _on_server_state(raw: int) -> None:
    global _pending_ui_refresh
    with _state_lock:
        _pending_ui_refresh = True


def _on_disconnect() -> None:
    global _status_line
    with _state_lock:
        _status_line = "Disconnected"


# ── Connect / disconnect ────────────────────────────────────────────

def _connect(host: str, port: int) -> bool:
    global _bridge, _status_line, _status_detail
    if _bridge is not None and _bridge.is_connected():
        _status_line = "Already connected"
        return True
    client = net_client.BridgeClient()
    client.on_hello        = _on_hello
    client.on_entity_state = _on_entity_state
    client.on_entity_gone  = _on_entity_gone
    client.on_server_state = _on_server_state
    client.on_disconnect   = _on_disconnect
    if not client.connect(host=host, port=port, timeout=5.0):
        _status_line   = "Connect failed"
        _status_detail = client.last_error or ""
        return False
    with _state_lock:
        _bridge = client
        _status_line   = f"Connected to {host}:{port}"
        _status_detail = ""
    return True


def _disconnect() -> None:
    global _bridge, _status_line
    with _state_lock:
        if _bridge is None:
            return
        bridge = _bridge
        _bridge = None
        _status_line = "Disconnected"
    bridge.disconnect()


def _send_move(transform) -> bool:
    with _state_lock:
        bridge = _bridge
    if bridge is None or not bridge.is_connected():
        return False
    return bridge.send_move_self(
        transform["position"][0],
        transform["position"][1],
        transform["position"][2],
        transform["rotation"][0],
        transform["rotation"][1],
        transform["rotation"][2],
        transform["rotation"][3])


# ── Depsgraph hook ──────────────────────────────────────────────────
#
# Runs on Blender's main thread whenever the scene's depsgraph is
# updated (i.e. after any object edit). We only care about the object
# that represents the bridge's own avatar — every other update is
# a no-op.

_AVATAR_OBJECT_NAME = "StratumV.Avatar"


def _avatar_object():
    if not _HAVE_BPY:
        return None
    return bpy.data.objects.get(_AVATAR_OBJECT_NAME)


def _depsgraph_update_post(scene, depsgraph):  # noqa: ARG001
    """bpy.app.handlers.depsgraph_update_post signature.

    Blender calls this on the main thread after every dependency
    update. We read the avatar object's world matrix, convert Z-up
    → Y-up via coord_convert, and push a MoveSelf frame. We also
    check `obj.parent` — if it has changed since the last update,
    resolve it against the cached replicated entity list and push
    a SetParent frame.
    """
    global _moves_sent, _moves_ignored_self, _parents_sent, _last_parent_entity
    if not _HAVE_BPY:
        return
    obj = _avatar_object()
    if obj is None:
        return
    # Only fire if this depsgraph update actually touched the avatar.
    # Blender fires depsgraph_update_post on camera pans, selection
    # changes, etc. — we want to filter those out.
    touched = False
    for upd in depsgraph.updates:
        if upd.id.name == obj.name and upd.is_updated_transform:
            touched = True
            break
    if not touched:
        return

    matrix = obj.matrix_world
    engine_trs = coord_convert.matrix_to_engine(matrix)
    if _send_move(engine_trs):
        _moves_sent += 1
    else:
        _moves_ignored_self += 1

    # Parent sync. Resolve the Blender parent to a
    # replicated entityId by walking the bridge's cached _entities
    # for an object whose Blender mirror name matches. The mirror
    # naming convention is `StratumV.<id>.<sanitize(label)>`, so we
    # pull the id off the name prefix.
    new_parent_entity = 0
    parent_obj = obj.parent
    if parent_obj is not None:
        new_parent_entity = _entity_id_from_object_name(parent_obj.name)

    if new_parent_entity != _last_parent_entity:
        if _send_set_parent(new_parent_entity):
            _parents_sent += 1
        _last_parent_entity = new_parent_entity


def _entity_id_from_object_name(obj_name: str) -> int:
    """Pull the replicated entityId out of a mirrored object name.

    The mirror convention from `_mirror_entities_to_scene` is:
        StratumV.<entityId>.<sanitized_label>
    Anything that doesn't start with `StratumV.` or whose prefix is
    not an integer resolves to 0 (unparented).
    """
    if not obj_name.startswith("StratumV."):
        return 0
    tail = obj_name[len("StratumV."):]
    dot = tail.find(".")
    head = tail if dot < 0 else tail[:dot]
    try:
        return int(head)
    except ValueError:
        return 0


def _send_set_parent(parent_entity_id: int) -> bool:
    with _state_lock:
        bridge = _bridge
    if bridge is None or not bridge.is_connected():
        return False
    return bridge.send_set_parent(parent_entity_id)


# ── Pump timer (Blender-side UI sync) ───────────────────────────────
#
# Runs on the main thread every ~100 ms. Copies the current entity
# snapshot out of _state_lock and mirrors it into Blender scene
# objects so the artist sees replicated entities as empties.

_PUMP_INTERVAL_SEC = 0.1


def _pump_timer():
    if not _HAVE_BPY:
        return None
    with _state_lock:
        entities = list(_entities.values())
        hello = _last_hello
    _mirror_entities_to_scene(entities, hello)
    return _PUMP_INTERVAL_SEC


def _ensure_collection():
    col = bpy.data.collections.get("StratumV")
    if col is None:
        col = bpy.data.collections.new("StratumV")
        bpy.context.scene.collection.children.link(col)
    return col


def _mirror_entities_to_scene(entities: List[net_client.EntityState],
                              hello: Optional[net_client.HelloMessage]) -> None:
    if not _HAVE_BPY:
        return
    col = _ensure_collection()

    seen = set()
    avatar_entity_id = hello.bridge_avatar_entity if hello else 0

    for ent in entities:
        seen.add(ent.entity_id)
        name = f"StratumV.{ent.entity_id}.{_sanitize(ent.label)}"
        obj = bpy.data.objects.get(name)
        if obj is None:
            obj = bpy.data.objects.new(name, None)  # empty
            obj.empty_display_type = "PLAIN_AXES"
            obj.empty_display_size = 5.0
            col.objects.link(obj)
            _synced_object_names[ent.entity_id] = name
        # Engine Y-up → Blender Z-up. Inverse of
        # coord_convert.position_to_engine: (x, y, z)_engine →
        # (x, -z, y)_blender.
        obj.location = (ent.pos_x, -ent.pos_z, ent.pos_y)
        # Scale up the bridge's own avatar so it is obvious which
        # entity the depsgraph hook is tracking.
        if ent.entity_id == avatar_entity_id and avatar_entity_id != 0:
            obj.empty_display_size = 12.0
        else:
            obj.empty_display_size = 5.0

    # Reap any locally-cached names that the server has forgotten.
    stale = [eid for eid in _synced_object_names if eid not in seen]
    for eid in stale:
        name = _synced_object_names.pop(eid, None)
        if name is None:
            continue
        obj = bpy.data.objects.get(name)
        if obj is not None:
            bpy.data.objects.remove(obj, do_unlink=True)


def _sanitize(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum() or ch in ("_", "-"):
            out.append(ch)
        else:
            out.append("_")
    return "".join(out) or "unnamed"


# ── UI (Blender operators + panel) ──────────────────────────────────

if _HAVE_BPY:

    class STRATUMV_OT_link_connect(bpy.types.Operator):
        """Open a live-link connection to a running StratumV bridge."""

        bl_idname = "stratumv.link_connect"
        bl_label  = "Connect"

        def execute(self, context):
            host = context.scene.stratumv_link_host
            port = int(context.scene.stratumv_link_port)
            if _connect(host, port):
                self.report({"INFO"}, _status_line)
                return {"FINISHED"}
            self.report({"ERROR"}, _status_line + ": " + _status_detail)
            return {"CANCELLED"}

    class STRATUMV_OT_link_disconnect(bpy.types.Operator):
        """Close the active live-link connection."""

        bl_idname = "stratumv.link_disconnect"
        bl_label  = "Disconnect"

        def execute(self, context):  # noqa: ARG002
            _disconnect()
            return {"FINISHED"}

    class STRATUMV_OT_link_push_asset(bpy.types.Operator):
        """Push the active mesh object as a .glb asset through the bridge."""

        bl_idname = "stratumv.link_push_asset"
        bl_label  = "Push Asset"

        @classmethod
        def poll(cls, context):
            obj = context.active_object
            with _state_lock:
                alive = _bridge is not None and _bridge.is_connected()
            return alive and obj is not None and obj.type in {"MESH", "EMPTY"}

        def execute(self, context):
            global _assets_pushed
            obj = context.active_object
            if obj is None:
                self.report({"ERROR"}, "no active object to push")
                return {"CANCELLED"}
            with _state_lock:
                bridge = _bridge
            if bridge is None or not bridge.is_connected():
                self.report({"ERROR"}, "bridge not connected")
                return {"CANCELLED"}

            # Export the selection to a temp .glb. We force-select
            # the active object so `use_selection=True` picks up
            # exactly what the artist clicked.
            # Save + restore the selection so the Blender state
            # round-trips unchanged.
            prev_selected = [o for o in context.selected_objects]
            prev_active   = context.view_layer.objects.active

            for o in prev_selected:
                o.select_set(False)
            obj.select_set(True)
            context.view_layer.objects.active = obj

            tmp_handle, tmp_path = tempfile.mkstemp(
                suffix=".glb", prefix="stratumv_push_")
            os.close(tmp_handle)
            try:
                bpy.ops.export_scene.gltf(
                    filepath=tmp_path,
                    use_selection=True,
                    export_format="GLB",
                    export_apply=True)
                rel_name = f"push/{_sanitize(obj.name)}.glb"
                # Pull bytes in via a helper that doesn't keep the
                # file open under the temp handle.
                with open(tmp_path, "rb") as f:
                    data = f.read()
            finally:
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass
                # Restore selection.
                for o in context.view_layer.objects:
                    try:
                        o.select_set(False)
                    except RuntimeError:
                        pass
                for o in prev_selected:
                    try:
                        o.select_set(True)
                    except RuntimeError:
                        pass
                if prev_active is not None:
                    try:
                        context.view_layer.objects.active = prev_active
                    except Exception:
                        pass

            if not bridge.send_asset_bytes(
                    data,
                    net_client.ASSET_KIND_MESH,
                    rel_name):
                self.report({"ERROR"},
                            f"send_asset failed: {bridge.last_error or 'unknown'}")
                return {"CANCELLED"}

            _assets_pushed += 1
            self.report({"INFO"},
                        f"pushed {rel_name} ({len(data)} bytes)")
            return {"FINISHED"}

    class STRATUMV_OT_link_push_camera(bpy.types.Operator):
        """Push the active Blender Camera as a CameraComponent SetField.

        Reads `context.active_object` (must be a
        Blender camera, or falls back to `scene.camera`) and forwards
        its `lens_unit`/`angle`/`clip_start`/`clip_end` as a
        CameraComponent snapshot delivered through the bridge. The
        bridge rewraps it as a full-mask SetField targeting the
        bridge's own avatar entity, so the camera "rides on" the
        avatar until a future session adds standalone camera entities.

        The Blender camera's vertical field of view is computed via
        `obj.data.angle_y` (in radians) and converted to degrees.
        Aspect is left at 0 ("use window aspect at frame time on the
        viewer side") because Blender's render aspect doesn't always
        match the lab harness's window aspect, and forcing the
        Blender value would distort the override viewport.
        """

        bl_idname = "stratumv.link_push_camera"
        bl_label  = "Push Camera"

        @classmethod
        def poll(cls, context):
            with _state_lock:
                alive = _bridge is not None and _bridge.is_connected()
            if not alive:
                return False
            cam = context.active_object
            if cam is None or cam.type != "CAMERA":
                cam = context.scene.camera
            return cam is not None and cam.type == "CAMERA"

        def execute(self, context):
            global _cameras_sent
            cam_obj = context.active_object
            if cam_obj is None or cam_obj.type != "CAMERA":
                cam_obj = context.scene.camera
            if cam_obj is None or cam_obj.type != "CAMERA" or cam_obj.data is None:
                self.report({"ERROR"}, "no active Blender camera to push")
                return {"CANCELLED"}
            with _state_lock:
                bridge = _bridge
            if bridge is None or not bridge.is_connected():
                self.report({"ERROR"}, "bridge not connected")
                return {"CANCELLED"}

            cam = cam_obj.data
            # Vertical FOV in degrees. Blender exposes both `angle`
            # (horizontal for orthographic / generic) and `angle_y`
            # (vertical for perspective). Vertical is what glm
            # perspective expects.
            import math
            fov_rad = float(getattr(cam, "angle_y", cam.angle))
            fov_deg = math.degrees(fov_rad)

            near_plane = float(getattr(cam, "clip_start", 0.1))
            far_plane  = float(getattr(cam, "clip_end",  1000.0))
            # Aspect = 0 means "use window aspect on the viewer side".
            aspect = 0.0

            if not bridge.send_set_camera(
                    fov_deg, aspect, near_plane, far_plane):
                self.report({"ERROR"},
                            f"send_set_camera failed: {bridge.last_error or 'unknown'}")
                return {"CANCELLED"}

            _cameras_sent += 1
            self.report({"INFO"},
                        f"pushed camera '{cam_obj.name}' fov={fov_deg:.1f} "
                        f"near={near_plane:.2f} far={far_plane:.1f}")
            return {"FINISHED"}

    class STRATUMV_OT_link_push_material(bpy.types.Operator):
        """Push the active object's first material color as a
        MaterialComponent SetField with a hard-coded strength of 1.0.

        Reads `context.active_object.material_slots[0]`
        and pulls the Principled BSDF basecolor (or, if no node tree,
        falls back to `material.diffuse_color`). Strength is forced to
        1.0 — the artist can dial it down via a follow-up Push call
        with a custom strength once the override is live.

        The override is a single global SceneUBO slot, NOT a per-entity
        descriptor set 2 patch. The single-demo scope is documented in
        MaterialComponent.h.
        """

        bl_idname = "stratumv.link_push_material"
        bl_label  = "Push Material"

        @classmethod
        def poll(cls, context):
            with _state_lock:
                alive = _bridge is not None and _bridge.is_connected()
            if not alive:
                return False
            obj = context.active_object
            if obj is None or not getattr(obj, "material_slots", None):
                return False
            return len(obj.material_slots) > 0

        def execute(self, context):
            global _materials_sent
            obj = context.active_object
            if obj is None:
                self.report({"ERROR"}, "no active object")
                return {"CANCELLED"}
            if not obj.material_slots or obj.material_slots[0].material is None:
                self.report({"ERROR"}, "active object has no material slot 0")
                return {"CANCELLED"}
            with _state_lock:
                bridge = _bridge
            if bridge is None or not bridge.is_connected():
                self.report({"ERROR"}, "bridge not connected")
                return {"CANCELLED"}

            mat = obj.material_slots[0].material
            color = (1.0, 1.0, 1.0)
            # Try the Principled BSDF first; fall back to diffuse_color.
            if mat.use_nodes and mat.node_tree is not None:
                for node in mat.node_tree.nodes:
                    if node.type == "BSDF_PRINCIPLED":
                        bc = node.inputs.get("Base Color")
                        if bc is not None and not bc.is_linked:
                            v = bc.default_value
                            color = (float(v[0]), float(v[1]), float(v[2]))
                            break
            else:
                v = getattr(mat, "diffuse_color", None)
                if v is not None:
                    color = (float(v[0]), float(v[1]), float(v[2]))

            strength = 1.0   # full demo override

            if not bridge.send_set_material(
                    color[0], color[1], color[2], strength):
                self.report({"ERROR"},
                            f"send_set_material failed: {bridge.last_error or 'unknown'}")
                return {"CANCELLED"}

            _materials_sent += 1
            self.report({"INFO"},
                        f"pushed material '{mat.name}' rgb=({color[0]:.2f}, "
                        f"{color[1]:.2f}, {color[2]:.2f}) strength={strength:.2f}")
            return {"FINISHED"}

    class STRATUMV_OT_link_push_light(bpy.types.Operator):
        """Push the active Blender Light as a LightComponent SetField.

        Reads `context.active_object` (must be a
        Blender light) and translates its type + color + energy into
        a LightComponent snapshot delivered through the bridge. The
        bridge rewraps the frame as a full-mask SetField targeting
        the bridge's own avatar entity, so the light visually rides
        on the avatar until a future session adds standalone light
        entities or ParentLink-based mounting.
        """

        bl_idname = "stratumv.link_push_light"
        bl_label  = "Push Light"

        @classmethod
        def poll(cls, context):
            obj = context.active_object
            with _state_lock:
                alive = _bridge is not None and _bridge.is_connected()
            return (alive and obj is not None
                    and obj.type == "LIGHT" and obj.data is not None)

        def execute(self, context):
            global _lights_sent
            obj = context.active_object
            if obj is None or obj.type != "LIGHT" or obj.data is None:
                self.report({"ERROR"}, "active object is not a light")
                return {"CANCELLED"}
            with _state_lock:
                bridge = _bridge
            if bridge is None or not bridge.is_connected():
                self.report({"ERROR"}, "bridge not connected")
                return {"CANCELLED"}

            light = obj.data
            # Blender light types: "POINT" / "SUN" / "SPOT" / "AREA".
            # StratumV LightComponent only knows Disabled/Directional/
            # Point/Spot. Map AREA to Point as a reasonable fallback
            # — area lights won't look right visually but at least the
            # intensity + color carry over, which is enough for the
            # visual checkpoint.
            btype = light.type
            if btype == "SUN":
                sv_type = net_client.LIGHT_TYPE_DIRECTIONAL
            elif btype == "POINT" or btype == "AREA":
                sv_type = net_client.LIGHT_TYPE_POINT
            elif btype == "SPOT":
                sv_type = net_client.LIGHT_TYPE_SPOT
            else:
                sv_type = net_client.LIGHT_TYPE_POINT

            # Blender `light.color` is a Color (RGB 0..1).
            color = tuple(light.color) if light.color is not None else (1.0, 1.0, 1.0)

            # `light.energy` is Blender's physical unit (Watts for
            # point/spot, Lux/1000 for sun). For the demo we pass it
            # through directly — artists can dial in values that look
            # right in the StratumV viewport without worrying about
            # unit conversion.
            intensity = float(light.energy)

            # Range: Blender no longer exposes a hard cutoff range;
            # fall back to a sensible default for point/spot and
            # ignore it for directional.
            range_units = 25.0
            if hasattr(light, "cutoff_distance") and light.cutoff_distance > 0:
                range_units = float(light.cutoff_distance)

            # Cone angles: spot only. `light.spot_size` is the full
            # cone angle in radians. `spot_blend` is a 0..1 softness
            # ratio that controls the inner cone.
            cone_outer_deg = 45.0
            cone_inner_deg = 30.0
            if btype == "SPOT":
                import math
                outer_rad = float(getattr(light, "spot_size", math.radians(45.0)))
                blend     = float(getattr(light, "spot_blend", 0.15))
                cone_outer_deg = math.degrees(outer_rad) * 0.5
                cone_inner_deg = cone_outer_deg * max(0.0, 1.0 - blend)

            if not bridge.send_set_light(
                    sv_type,
                    color[0], color[1], color[2],
                    intensity,
                    range_units,
                    cone_inner_deg, cone_outer_deg):
                self.report({"ERROR"},
                            f"send_set_light failed: {bridge.last_error or 'unknown'}")
                return {"CANCELLED"}

            _lights_sent += 1
            self.report({"INFO"},
                        f"pushed light '{obj.name}' ({btype}) "
                        f"I={intensity:.2f} r={range_units:.1f}")
            return {"FINISHED"}

    class STRATUMV_PT_live_link(bpy.types.Panel):
        """Live-link sidebar panel."""

        bl_label       = "StratumV Live Link"
        bl_idname      = "STRATUMV_PT_live_link"
        bl_space_type  = "VIEW_3D"
        bl_region_type = "UI"
        bl_category    = "StratumV"

        def draw(self, context):
            layout = self.layout
            scene  = context.scene

            with _state_lock:
                hello = _last_hello
                entities = list(_entities.values())
                status_line   = _status_line
                status_detail = _status_detail
                bridge_alive  = _bridge is not None and _bridge.is_connected()

            box = layout.box()
            box.label(text="Connection", icon="WORLD_DATA")
            col = box.column(align=True)
            col.prop(scene, "stratumv_link_host", text="Host")
            col.prop(scene, "stratumv_link_port", text="Port")
            row = box.row(align=True)
            if bridge_alive:
                row.operator(STRATUMV_OT_link_disconnect.bl_idname,
                             icon="UNLINKED")
            else:
                row.operator(STRATUMV_OT_link_connect.bl_idname,
                             icon="LINKED")
            row = box.row()
            if bridge_alive:
                row.label(text=status_line, icon="CHECKMARK")
            else:
                row.label(text=status_line, icon="ERROR")
            if status_detail:
                box.label(text=status_detail, icon="INFO")

            if hello is not None:
                info = layout.box()
                info.label(text="Bridge Identity", icon="USER")
                col = info.column(align=True)
                col.label(text=f"Client: {hello.bridge_client_id}")
                col.label(text=f"Avatar: {hello.bridge_avatar_entity}")
                col.label(text=f"Scope: {_fmt_scope(hello.scope)}")
                col.label(text=f"Server: {_fmt_semver(hello.server_semver)}")
                col.label(text=f"Schema: 0x{hello.net_transform_schema:04X}")
                col.label(text=f"State: {_fmt_server_state(hello.server_state)}")
                col.label(text=f"Moves sent: {_moves_sent}")
                col.label(text=f"Assets pushed: {_assets_pushed}")
                col.label(text=f"Parents sent: {_parents_sent}")
                col.label(text=f"Lights sent: {_lights_sent}")
                col.label(text=f"Cameras sent: {_cameras_sent}")
                col.label(text=f"Materials sent: {_materials_sent}")

            # Asset push panel
            push_box = layout.box()
            push_box.label(text="Assets", icon="OUTLINER_OB_MESH")
            active = context.active_object
            if active is not None:
                push_box.label(text=f"Active: {active.name}")
            else:
                push_box.label(text="(no active object)")
            row = push_box.row()
            row.enabled = (bridge_alive and active is not None
                           and active.type in {"MESH", "EMPTY"})
            row.operator(STRATUMV_OT_link_push_asset.bl_idname,
                         icon="EXPORT")

            # Light push panel
            light_box = layout.box()
            light_box.label(text="Lights", icon="LIGHT")
            active_is_light = (active is not None
                               and active.type == "LIGHT"
                               and active.data is not None)
            if active_is_light:
                light_box.label(text=f"Active: {active.name} ({active.data.type})")
            else:
                light_box.label(text="(select a Blender light)")
            row = light_box.row()
            row.enabled = bridge_alive and active_is_light
            row.operator(STRATUMV_OT_link_push_light.bl_idname,
                         icon="LIGHT_SUN")

            # Camera push panel
            cam_box = layout.box()
            cam_box.label(text="Camera", icon="CAMERA_DATA")
            scene_cam = context.scene.camera
            cam_obj = active if (active is not None
                                 and active.type == "CAMERA") else scene_cam
            if cam_obj is not None and cam_obj.type == "CAMERA":
                cam_box.label(text=f"Active: {cam_obj.name}")
            else:
                cam_box.label(text="(select a Blender camera)")
            row = cam_box.row()
            row.enabled = bridge_alive and (cam_obj is not None
                                            and cam_obj.type == "CAMERA")
            row.operator(STRATUMV_OT_link_push_camera.bl_idname,
                         icon="VIEW_CAMERA")

            # Material push panel
            mat_box = layout.box()
            mat_box.label(text="Material override", icon="MATERIAL")
            has_mat = (active is not None
                       and getattr(active, "material_slots", None)
                       and len(active.material_slots) > 0
                       and active.material_slots[0].material is not None)
            if has_mat:
                mat_box.label(
                    text=f"Active: {active.name} -> "
                         f"{active.material_slots[0].material.name}")
            else:
                mat_box.label(text="(select an object with a material)")
            row = mat_box.row()
            row.enabled = bridge_alive and bool(has_mat)
            row.operator(STRATUMV_OT_link_push_material.bl_idname,
                         icon="NODE_MATERIAL")

            list_box = layout.box()
            list_box.label(text=f"Entities ({len(entities)})",
                           icon="OUTLINER_OB_EMPTY")
            for ent in sorted(entities, key=lambda e: e.entity_id):
                row = list_box.row(align=True)
                tag = "SELF" if ent.is_self else f"c{ent.owner_client_id}"
                row.label(text=f"#{ent.entity_id} {tag} {ent.label}")

    _CLASSES = (
        STRATUMV_OT_link_connect,
        STRATUMV_OT_link_disconnect,
        STRATUMV_OT_link_push_asset,
        STRATUMV_OT_link_push_light,
        STRATUMV_OT_link_push_camera,
        STRATUMV_OT_link_push_material,
        STRATUMV_PT_live_link,
    )

    def _register_handlers():
        # Idempotent — Blender's handler list tolerates duplicates,
        # but we clean up first so "Reload Scripts" doesn't pile on.
        _unregister_handlers()
        bpy.app.handlers.depsgraph_update_post.append(
            _depsgraph_update_post)
        bpy.app.timers.register(_pump_timer,
                                first_interval=_PUMP_INTERVAL_SEC,
                                persistent=True)

    def _unregister_handlers():
        # Remove any copy of our handler by identity.
        for h in list(bpy.app.handlers.depsgraph_update_post):
            if h is _depsgraph_update_post:
                bpy.app.handlers.depsgraph_update_post.remove(h)
        try:
            bpy.app.timers.unregister(_pump_timer)
        except ValueError:
            pass

    def register():
        bpy.types.Scene.stratumv_link_host = StringProperty(
            name="Host",
            description="StratumV bridge host (loopback only today)",
            default="127.0.0.1")
        bpy.types.Scene.stratumv_link_port = IntProperty(
            name="Port",
            description="StratumV bridge TCP port",
            default=net_client.DEFAULT_BRIDGE_PORT,
            min=1, max=65535)
        for cls in _CLASSES:
            bpy.utils.register_class(cls)
        _register_handlers()

    def unregister():
        _disconnect()
        _unregister_handlers()
        for cls in reversed(_CLASSES):
            bpy.utils.unregister_class(cls)
        if hasattr(bpy.types.Scene, "stratumv_link_host"):
            del bpy.types.Scene.stratumv_link_host
        if hasattr(bpy.types.Scene, "stratumv_link_port"):
            del bpy.types.Scene.stratumv_link_port

else:
    # Running under plain Python — expose no-op register/unregister
    # so the addon package import doesn't explode.
    def register():
        pass

    def unregister():
        pass
