# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""Headless live-link proxy test.

Spawns no Blender. Instead, it acts as a Blender stand-in: it opens
a TCP connection to a running `skinned_test --editor-bridge-port N`
instance and drives the bridge like a real Blender live-link client
would. Used by the visual checkpoint rig and as a smoke test during
the live-link dev loop.

Runtime path under test:

    this script (net_client.BridgeClient)
        → TCP 127.0.0.1:<port>
        → skinned_test (bridge mode)
        → MsQuic
        → stratumv_server
        → MsQuic broadcast
        → other skinned_test instances (render the moved cube)

Usage:

    python tools/blender/test_live_link.py \\
        --host 127.0.0.1 --port 9401 \\
        --dwell 4.0 --count 6 --step 12.0 [--dry-run]

Flags:

    --host HOST          bridge TCP host (default 127.0.0.1)
    --port N             bridge TCP port (default 9401)
    --dwell SEC          seconds to wait after connect before the first
                         move (default 4.0 — gives the bridge time to
                         finish its welcome + world sync)
    --count N            number of MoveSelf frames to emit (default 6)
    --step UNITS         world-space distance per step (default 12.0)
    --interval SEC       wall-clock gap between MoveSelf frames
                         (default 0.30)
    --direction AXIS     x|z|diag (default diag)
    --dry-run            connect + print the cached world, but do NOT
                         send any MoveSelf frames

Returns 0 on clean success, 1 if the bridge connection could not be
opened or if no Hello arrived before the dwell timer expired.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

# Re-use the addon's module without installing the addon. The addon
# layout (`tools/blender/stratumv_exporter/net_client.py`) is not on
# sys.path by default so we bolt it on here.
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_ADDON_DIR = os.path.join(_THIS_DIR, "stratumv_exporter")
if _ADDON_DIR not in sys.path:
    sys.path.insert(0, _ADDON_DIR)

import net_client  # noqa: E402  (late import — see sys.path hack above)


def _permission_scope_label(raw: int) -> str:
    return {
        0: "Spectator",
        1: "Player",
        2: "Editor",
        3: "Admin",
    }.get(raw, f"raw={raw}")


def _server_state_label(raw: int) -> str:
    return {
        net_client.SERVER_PENDING:  "pending",
        net_client.SERVER_TLS_OK:   "tls-ok",
        net_client.SERVER_WELCOMED: "welcomed",
    }.get(raw, f"raw={raw}")


def _print_hello(hello: net_client.HelloMessage) -> None:
    major = (hello.server_semver >> 16) & 0xFF
    minor = (hello.server_semver >>  8) & 0xFF
    patch = (hello.server_semver       ) & 0xFF
    print(f"[proxy] bridge hello: client={hello.bridge_client_id} "
          f"avatar={hello.bridge_avatar_entity} "
          f"scope={_permission_scope_label(hello.scope)} "
          f"server={major}.{minor}.{patch} "
          f"schema=0x{hello.net_transform_schema:04X} "
          f"state={_server_state_label(hello.server_state)} "
          f"app='{hello.app_name}'")


def _print_entities(client: net_client.BridgeClient) -> None:
    entities = sorted(client.get_entities(), key=lambda e: e.entity_id)
    if not entities:
        print("[proxy] no entities yet")
        return
    print(f"[proxy] cached entities ({len(entities)}):")
    for ent in entities:
        tag = "SELF" if ent.is_self else f"owner={ent.owner_client_id}"
        print(f"  [{ent.entity_id:>4}] {tag:<12} "
              f"pos=({ent.pos_x:8.2f},{ent.pos_y:8.2f},{ent.pos_z:8.2f}) "
              f"'{ent.label}'")


def _direction_delta(axis: str, step: float, i: int) -> tuple:
    # Walk in a consistent direction so cumulative displacement is
    # visible on the top-down canvas. The starting avatar position
    # sits on a ~60-unit radius circle and the canvas spans ±150
    # units, so ~5 steps of 12-14 units stay comfortably on-screen.
    if axis == "x":
        return (step, 0.0, 0.0)
    if axis == "z":
        return (0.0, 0.0, step)
    return (step * 0.7071, 0.0, step * 0.7071)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Headless bridge proxy test")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int,
                        default=net_client.DEFAULT_BRIDGE_PORT)
    parser.add_argument("--dwell",    type=float, default=4.0)
    parser.add_argument("--count",    type=int,   default=6)
    parser.add_argument("--step",     type=float, default=12.0)
    parser.add_argument("--interval", type=float, default=0.30)
    parser.add_argument("--direction", choices=("x", "z", "diag"),
                        default="diag")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--retry-seconds", type=float, default=0.0,
                        help="retry connect for N seconds until a valid "
                             "Hello with a non-zero avatar arrives")
    parser.add_argument("--push-asset", default=None,
                        help="absolute path of a file "
                             "to push as a mesh asset through the bridge "
                             "before/after the moves")
    parser.add_argument("--push-asset-name", default=None,
                        help="forward-slash name the "
                             "asset is announced as (default: basename of "
                             "--push-asset)")
    parser.add_argument("--push-asset-kind", type=int, default=1,
                        help="asset kind byte "
                             "(1=Mesh, 2=Texture, ... default 1)")
    parser.add_argument("--set-parent", type=int, default=None,
                        help="parent entityId to set via "
                             "a SetParent bridge frame (0 = unparent)")
    parser.add_argument("--set-light-type", type=int, default=None,
                        help="LightComponent type "
                             "(0=Disabled 1=Directional 2=Point 3=Spot). "
                             "Pushing any --set-light-* flag fires one "
                             "SetLight bridge frame.")
    parser.add_argument("--set-light-color", default="1.0,1.0,1.0",
                        help="linear-space light color as "
                             "'r,g,b' (default 1.0,1.0,1.0)")
    parser.add_argument("--set-light-intensity", type=float, default=1.0,
                        help="light intensity multiplier "
                             "(default 1.0)")
    parser.add_argument("--set-light-range", type=float, default=25.0,
                        help="point/spot attenuation "
                             "range in world units (default 25.0)")
    parser.add_argument("--set-light-cone-inner", type=float, default=30.0,
                        help="spot cone inner angle in "
                             "degrees (default 30.0)")
    parser.add_argument("--set-light-cone-outer", type=float, default=45.0,
                        help="spot cone outer angle in "
                             "degrees (default 45.0)")
    # Camera + material override flags. The camera +
    # material pushes only fire when the corresponding --set-camera-fov
    # / --set-material-strength enable trigger is set, mirroring how
    # --set-light-type gates the light push.
    parser.add_argument("--set-camera-fov", type=float, default=None,
                        help="vertical FOV in degrees. "
                             "Setting this fires one SetCamera bridge frame.")
    parser.add_argument("--set-camera-aspect", type=float, default=0.0,
                        help="aspect override (0 = use "
                             "viewer window aspect)")
    parser.add_argument("--set-camera-near", type=float, default=0.1,
                        help="near plane (default 0.1)")
    parser.add_argument("--set-camera-far", type=float, default=2000.0,
                        help="far plane (default 2000.0)")
    parser.add_argument("--set-material-strength", type=float, default=None,
                        help="override strength in [0,1]. "
                             "Setting this fires one SetMaterial bridge frame.")
    parser.add_argument("--set-material-color", default="1.0,1.0,1.0",
                        help="override basecolor as "
                             "'r,g,b' (default 1.0,1.0,1.0)")
    args = parser.parse_args(argv)

    # Retry loop — the bridge may take up to ~15s to finish init
    # and send its Hello after the TCP listener opens. Keep trying
    # until we see a Hello with a non-zero avatar (meaning the
    # bridge has been welcomed by the server and is ready to
    # forward edits upstream).
    retry_deadline = time.monotonic() + max(args.retry_seconds, args.dwell)
    client = None
    hello = None
    attempts = 0
    while True:
        attempts += 1
        candidate = net_client.BridgeClient()
        last_hello = [None]

        def on_hello(h, _cell=last_hello):
            _cell[0] = h

        candidate.on_hello = on_hello
        if candidate.connect(host=args.host, port=args.port, timeout=3.0):
            deadline = time.monotonic() + args.dwell
            while time.monotonic() < deadline and last_hello[0] is None:
                time.sleep(0.05)
            if last_hello[0] is not None and \
               last_hello[0].bridge_avatar_entity != 0:
                client = candidate
                hello = client.get_hello()
                print(f"[proxy] bridge ready after {attempts} attempt(s)")
                break
            candidate.disconnect()
        else:
            err = candidate.last_error

        if time.monotonic() >= retry_deadline:
            msg = candidate.last_error if candidate.last_error else \
                (f"no welcomed Hello before "
                 f"retry_deadline ({args.retry_seconds}s)")
            print(f"[proxy] ERROR: {msg}", file=sys.stderr)
            return 1
        time.sleep(0.5)

    _print_hello(hello)
    _print_entities(client)

    # Asset push + set-parent are issued upfront so
    # they land before the MoveSelf burst. The bridge assembles the
    # asset on arrival and the main thread uploads it to the server
    # via the same path the in-engine button uses.
    if args.push_asset:
        asset_name = args.push_asset_name or os.path.basename(args.push_asset)
        if client.send_asset(args.push_asset, args.push_asset_kind, asset_name):
            size = os.path.getsize(args.push_asset)
            print(f"[proxy] pushed asset '{asset_name}' ({size} bytes) "
                  f"kind={args.push_asset_kind}")
        else:
            print(f"[proxy] WARN: push_asset failed: {client.last_error}")

    if args.set_parent is not None:
        if client.send_set_parent(args.set_parent):
            print(f"[proxy] sent SetParent({args.set_parent})")
        else:
            print(f"[proxy] WARN: send_set_parent failed: {client.last_error}")

    # Light push. Any --set-light-* flag fires one
    # SetLight bridge frame. --set-light-type is the enable trigger —
    # omitting it keeps the light disabled and skips the push entirely.
    if args.set_light_type is not None:
        try:
            color_parts = [float(p.strip())
                           for p in args.set_light_color.split(",")]
            if len(color_parts) != 3:
                raise ValueError("need three comma-separated components")
        except ValueError as exc:
            print(f"[proxy] ERROR: --set-light-color: {exc}", file=sys.stderr)
            client.disconnect()
            return 1
        ok = client.send_set_light(
            args.set_light_type,
            color_parts[0], color_parts[1], color_parts[2],
            args.set_light_intensity,
            args.set_light_range,
            args.set_light_cone_inner,
            args.set_light_cone_outer,
        )
        if ok:
            print(f"[proxy] sent SetLight(type={args.set_light_type} "
                  f"color={tuple(color_parts)} "
                  f"I={args.set_light_intensity} r={args.set_light_range})")
        else:
            print(f"[proxy] WARN: send_set_light failed: {client.last_error}")

    # Camera push.
    if args.set_camera_fov is not None:
        ok = client.send_set_camera(
            args.set_camera_fov,
            args.set_camera_aspect,
            args.set_camera_near,
            args.set_camera_far,
        )
        if ok:
            print(f"[proxy] sent SetCamera(fov={args.set_camera_fov} "
                  f"aspect={args.set_camera_aspect} "
                  f"near={args.set_camera_near} far={args.set_camera_far})")
        else:
            print(f"[proxy] WARN: send_set_camera failed: {client.last_error}")

    # Material push.
    if args.set_material_strength is not None:
        try:
            mat_parts = [float(p.strip())
                         for p in args.set_material_color.split(",")]
            if len(mat_parts) != 3:
                raise ValueError("need three comma-separated components")
        except ValueError as exc:
            print(f"[proxy] ERROR: --set-material-color: {exc}", file=sys.stderr)
            client.disconnect()
            return 1
        ok = client.send_set_material(
            mat_parts[0], mat_parts[1], mat_parts[2],
            args.set_material_strength,
        )
        if ok:
            print(f"[proxy] sent SetMaterial(rgb={tuple(mat_parts)} "
                  f"strength={args.set_material_strength})")
        else:
            print(f"[proxy] WARN: send_set_material failed: {client.last_error}")

    if args.dry_run:
        print("[proxy] --dry-run, skipping MoveSelf burst")
        # Give the reader thread a beat to catch any echo frames the
        # bridge may push back after the asset / parent messages.
        time.sleep(0.5)
        _print_entities(client)
        print(f"[proxy] inbound={client.messages_inbound} "
              f"outbound={client.messages_outbound} "
              f"assets_pushed={client.assets_pushed} "
              f"parents_sent={client.parents_sent} "
              f"lights_sent={client.lights_sent} "
              f"cameras_sent={client.cameras_sent} "
              f"materials_sent={client.materials_sent}")
        client.disconnect()
        return 0

    if hello.bridge_avatar_entity == 0:
        print("[proxy] WARN: bridge reports no avatar (not yet "
              "welcomed by server?) — sending moves anyway")

    # Read the bridge's own avatar as the starting state so every
    # move is an incremental nudge, not a teleport. If no avatar is
    # known (shouldn't happen in practice), fall back to zero.
    start = net_client.EntityState()
    if hello.bridge_avatar_entity:
        existing = client.get_entity(hello.bridge_avatar_entity)
        if existing is not None:
            start = existing

    pos = [start.pos_x, start.pos_y, start.pos_z]
    rot = [start.rot_x, start.rot_y, start.rot_z, start.rot_w]

    print(f"[proxy] sending {args.count} MoveSelf frames, "
          f"step={args.step}, direction={args.direction}")
    for i in range(args.count):
        dx, dy, dz = _direction_delta(args.direction, args.step, i)
        pos[0] += dx
        pos[1] += dy
        pos[2] += dz
        ok = client.send_move_self(pos[0], pos[1], pos[2],
                                   rot[0], rot[1], rot[2], rot[3])
        tag = "sent" if ok else "FAIL"
        print(f"  [{i+1:>2}/{args.count}] {tag} "
              f"pos=({pos[0]:7.2f},{pos[1]:7.2f},{pos[2]:7.2f})")
        if not ok:
            client.disconnect()
            return 1
        time.sleep(args.interval)

    # Hang around briefly so the reader thread can pick up the echo
    # states the server will broadcast back through the bridge.
    time.sleep(0.5)
    _print_entities(client)

    print(f"[proxy] inbound={client.messages_inbound} "
          f"outbound={client.messages_outbound}")
    client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
