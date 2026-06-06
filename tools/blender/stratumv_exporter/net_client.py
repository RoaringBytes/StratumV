# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 RoaringBytes
"""StratumV editor-bridge client.

Pure-stdlib TCP client that speaks the EditorBridge protocol from
`src/engine/net/EditorBridge.h`. Used both by the Blender addon
(`live_link.py`) and by the headless proxy test
(`tools/blender/test_live_link.py`).

The runtime path is:

    Blender (this module, plain socket)
        -> TCP 127.0.0.1:<port>
        -> skinned_test (EditorBridge mode)
        -> MsQuic / QUIC reliable stream
        -> stratumv_server

The bridge client knows NOTHING about QUIC, MsQuic, or the
ReplicationRegistry — it just exchanges length-prefixed frames with
skinned_test, which does the real translation. Wire layout is
documented in `src/engine/net/EditorBridge.h`.

This module exposes:

- `BridgeClient`    — blocking connect/disconnect + background reader
                       thread + thread-safe send helpers
- `EntityState`     — POD snapshot of one replicated entity as seen
                       by the bridge
- `HelloMessage`    — POD snapshot of the bridge's own identity
- Message type constants (MSG_*)
- `build_move_self` — encode a MoveSelf frame without a BridgeClient
                       instance, useful for unit tests

The module deliberately avoids any Blender-specific imports so it
can be re-used from a plain `python tools/blender/test_live_link.py`.
"""

from __future__ import annotations

import hashlib
import os
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

# ── Message types (must match `src/engine/net/EditorBridge.h`) ──────

MSG_HELLO          = 0x01
MSG_ENTITY_STATE   = 0x02
MSG_ENTITY_GONE    = 0x03
MSG_SERVER_STATE   = 0x04
MSG_MOVE_SELF      = 0x81
MSG_PING           = 0x82
MSG_ASSET_ANNOUNCE = 0x83
MSG_ASSET_CHUNK    = 0x84
MSG_SET_PARENT     = 0x86
MSG_SET_LIGHT      = 0x87
MSG_SET_CAMERA     = 0x88
MSG_SET_MATERIAL   = 0x89

# ── LightComponent type enum (must match src/engine/LightComponent.h) ──
LIGHT_TYPE_DISABLED    = 0
LIGHT_TYPE_DIRECTIONAL = 1
LIGHT_TYPE_POINT       = 2
LIGHT_TYPE_SPOT        = 3

# Bridge server-state enum values (kBridgeServer*).
SERVER_PENDING  = 0
SERVER_TLS_OK   = 1
SERVER_WELCOMED = 2

# Frame-length cap: must match kMaxFrameBytes in EditorBridge.cpp.
# Sized at 256 KiB so a 64 KiB asset chunk
# payload + 45-byte chunk header + 4-byte length prefix fits
# comfortably inside one TCP frame.
MAX_FRAME_BYTES = 256 * 1024

# Chunk size for the AssetChunk stream. Must match kAssetChunkSize
# in src/engine/net/ReplicationProtocol.h so the Python producer and
# the C++ consumer agree on slice boundaries.
ASSET_CHUNK_SIZE = 65536

# Upper bound on a single asset — mirrors kAssetByteLimit.
ASSET_BYTE_LIMIT = 64 * 1024 * 1024


# ── Asset kind enum (must match sv::AssetKind byte layout) ─────────
# The enum lives in AssetBrowser.h. We only need the numeric values
# here; string formatting is the consumer's problem.
ASSET_KIND_OTHER     = 0
ASSET_KIND_MESH      = 1
ASSET_KIND_TEXTURE   = 2
ASSET_KIND_AUDIO     = 3
ASSET_KIND_SCENE     = 4
ASSET_KIND_SHADER    = 5
ASSET_KIND_MATERIAL  = 6
ASSET_KIND_ANIMATION = 7

# Default port — overridable via BridgeClient(port=...). Pick a
# port that's unlikely to collide with anything else on loopback.
DEFAULT_BRIDGE_PORT = 9401


# ── POD payloads ────────────────────────────────────────────────────

@dataclass
class HelloMessage:
    bridge_client_id:       int = 0
    bridge_avatar_entity:   int = 0
    scope:                  int = 0   # PermissionScope raw byte
    server_semver:          int = 0   # (major<<16)|(minor<<8)|patch
    net_transform_schema:   int = 0   # 16-bit fnv-fold schema version
    server_state:           int = SERVER_PENDING
    app_name:               str = ""


@dataclass
class EntityState:
    entity_id:       int = 0
    owner_client_id: int = 0
    is_self:         bool = False
    authority:       int = 0          # 0=Server, 1=Owner, 2=Editor, 3=None
    # NetTransform scalars in engine-space Y-up (posX, posY, posZ,
    # rotX, rotY, rotZ, rotW).
    pos_x: float = 0.0
    pos_y: float = 0.0
    pos_z: float = 0.0
    rot_x: float = 0.0
    rot_y: float = 0.0
    rot_z: float = 0.0
    rot_w: float = 1.0
    label: str = ""


# ── Framing helpers ─────────────────────────────────────────────────

def _pack_frame(msg_type: int, body: bytes) -> bytes:
    """Wrap [u8 type][body] into [u32 len][bytes]. Little-endian."""
    payload = bytes([msg_type & 0xFF]) + body
    header = struct.pack("<I", len(payload))
    return header + payload


def _read_exact(sock: socket.socket, n: int) -> Optional[bytes]:
    """Block until `n` bytes are received or the peer closes.

    Returns None on clean EOF, on any socket error, or on timeout.
    """
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except (OSError, socket.timeout):
            return None
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def _parse_utf8_string(body: bytes, offset: int):
    """Parse a `[u16 len][bytes]` string field. Returns (string, next_offset)."""
    if offset + 2 > len(body):
        return "", offset
    (length,) = struct.unpack_from("<H", body, offset)
    offset += 2
    end = offset + length
    if end > len(body):
        return "", len(body)
    return body[offset:end].decode("utf-8", errors="replace"), end


# ── Message encoders (public — used by the proxy test script) ───────

def build_move_self(pos_x: float, pos_y: float, pos_z: float,
                    rot_x: float = 0.0, rot_y: float = 0.0,
                    rot_z: float = 0.0, rot_w: float = 1.0) -> bytes:
    """Build a MoveSelf frame as raw bytes (ready for `sendall`).

    The payload is 28 bytes of little-endian IEEE-754 floats, in
    the same order as NetTransform: posX posY posZ rotX rotY rotZ rotW.
    """
    body = struct.pack("<7f", pos_x, pos_y, pos_z,
                       rot_x, rot_y, rot_z, rot_w)
    return _pack_frame(MSG_MOVE_SELF, body)


def build_ping() -> bytes:
    """Build a keepalive Ping frame (empty payload)."""
    return _pack_frame(MSG_PING, b"")


def build_set_parent(parent_entity_id: int) -> bytes:
    """Build a SetParent frame carrying a single u32 parent id.

    0 = unparented. The bridge rewraps this as a
    ParentLink SetField EditTransaction on its avatar.
    """
    body = struct.pack("<I", parent_entity_id & 0xFFFFFFFF)
    return _pack_frame(MSG_SET_PARENT, body)


def build_set_light(light_type:    int,
                    color_r:       float,
                    color_g:       float,
                    color_b:       float,
                    intensity:     float,
                    range_units:   float,
                    cone_inner_deg: float = 30.0,
                    cone_outer_deg: float = 45.0) -> bytes:
    """Build a SetLight bridge frame.

    32-byte body matching the C++
    `kBridgeMsgSetLight` layout documented in EditorBridge.h:

      [u32 type]
      [f32 colorR][f32 colorG][f32 colorB]
      [f32 intensity]
      [f32 range]
      [f32 coneInnerDeg][f32 coneOuterDeg]

    The bridge rewraps this as a LightComponent SetField
    EditTransaction targeting the bridge's own avatar entity.
    `light_type` is one of `LIGHT_TYPE_*`; disabled (0) leaves the
    light effectively off even if `intensity > 0`.
    """
    body = struct.pack(
        "<I7f",
        light_type & 0xFFFFFFFF,
        float(color_r),
        float(color_g),
        float(color_b),
        float(intensity),
        float(range_units),
        float(cone_inner_deg),
        float(cone_outer_deg),
    )
    return _pack_frame(MSG_SET_LIGHT, body)


def build_set_camera(fov_deg:    float,
                     aspect:     float,
                     near_plane: float,
                     far_plane:  float) -> bytes:
    """Build a SetCamera bridge frame.

    16-byte body matching the C++
    `kBridgeMsgSetCamera` layout documented in EditorBridge.h:

      [f32 fovDeg]
      [f32 aspect]
      [f32 nearPlane]
      [f32 farPlane]

    The bridge rewraps this as a CameraComponent SetField
    EditTransaction targeting the bridge's own avatar entity.
    Setting `fov_deg = 0` (or `far_plane <= near_plane`) is the
    convention for "no override" — the local renderer falls back to
    its own camera mode in that case.
    """
    body = struct.pack(
        "<4f",
        float(fov_deg),
        float(aspect),
        float(near_plane),
        float(far_plane),
    )
    return _pack_frame(MSG_SET_CAMERA, body)


def build_set_material(base_color_r:      float,
                       base_color_g:      float,
                       base_color_b:      float,
                       override_strength: float) -> bytes:
    """Build a SetMaterial bridge frame.

    16-byte body matching the C++
    `kBridgeMsgSetMaterial` layout documented in EditorBridge.h:

      [f32 baseColorR][f32 baseColorG][f32 baseColorB]
      [f32 overrideStrength]

    The bridge rewraps this as a MaterialComponent SetField
    EditTransaction targeting the bridge's own avatar entity.
    `override_strength = 0` (the default on the C++ side) means
    "no effect" — the lab harness will skip the SceneUBO override
    pack and the fragment shader leaves the per-mesh basecolor
    untouched.
    """
    body = struct.pack(
        "<4f",
        float(base_color_r),
        float(base_color_g),
        float(base_color_b),
        float(override_strength),
    )
    return _pack_frame(MSG_SET_MATERIAL, body)


def _asset_chunk_count(byte_size: int, chunk_size: int) -> int:
    """Mirror of sv::assetChunkCount — empty asset → 1 empty chunk."""
    if chunk_size <= 0:
        return 0
    if byte_size == 0:
        return 1
    return (byte_size + chunk_size - 1) // chunk_size


def build_asset_announce(sha256_digest: bytes,
                         byte_size: int,
                         asset_kind: int,
                         name: str) -> bytes:
    """Encode an AssetAnnounce bridge frame.

    Wire layout (matches the C++ `kBridgeMsgAssetAnnounce` payload):

      [32 bytes sha256]
      [u32 byteSize]
      [u8  assetKind]
      [u16 nameLen][name bytes]
    """
    if len(sha256_digest) != 32:
        raise ValueError(f"sha256 must be 32 bytes, got {len(sha256_digest)}")
    name_bytes = name.encode("utf-8", errors="replace")
    if len(name_bytes) > 0xFFFF:
        raise ValueError(f"asset name too long ({len(name_bytes)} > 65535)")
    body = (
        sha256_digest
        + struct.pack("<I", byte_size & 0xFFFFFFFF)
        + struct.pack("<B", asset_kind & 0xFF)
        + struct.pack("<H", len(name_bytes))
        + name_bytes
    )
    return _pack_frame(MSG_ASSET_ANNOUNCE, body)


def build_asset_chunks(sha256_digest: bytes,
                       data: bytes,
                       chunk_size: int = ASSET_CHUNK_SIZE) -> List[bytes]:
    """Slice `data` into a list of AssetChunk bridge frames.

    Wire layout per chunk (matches the C++ `kBridgeMsgAssetChunk`
    payload):

      [32 bytes sha256]
      [u32 chunkIndex]
      [u32 chunkCount]
      [u32 chunkLen]
      [chunkLen bytes]
    """
    if len(sha256_digest) != 32:
        raise ValueError(f"sha256 must be 32 bytes, got {len(sha256_digest)}")
    if chunk_size <= 0:
        raise ValueError(f"chunk_size must be > 0, got {chunk_size}")
    if len(data) > ASSET_BYTE_LIMIT:
        raise ValueError(
            f"asset size {len(data)} > ASSET_BYTE_LIMIT ({ASSET_BYTE_LIMIT})")

    chunk_count = _asset_chunk_count(len(data), chunk_size)
    frames: List[bytes] = []
    if chunk_count == 0:
        return frames

    for i in range(chunk_count):
        start = i * chunk_size
        end   = min(start + chunk_size, len(data))
        chunk = data[start:end]
        body = (
            sha256_digest
            + struct.pack("<I", i)
            + struct.pack("<I", chunk_count)
            + struct.pack("<I", len(chunk))
            + chunk
        )
        frames.append(_pack_frame(MSG_ASSET_CHUNK, body))
    return frames


# ── Message parsers ─────────────────────────────────────────────────

def _parse_hello(body: bytes) -> HelloMessage:
    hello = HelloMessage()
    if len(body) < 4 + 4 + 1 + 4 + 2 + 1:
        return hello
    (hello.bridge_client_id,
     hello.bridge_avatar_entity,
     hello.scope,
     hello.server_semver,
     hello.net_transform_schema,
     hello.server_state) = struct.unpack_from("<IIBIHB", body, 0)
    offset = 4 + 4 + 1 + 4 + 2 + 1
    hello.app_name, _ = _parse_utf8_string(body, offset)
    return hello


def _parse_entity_state(body: bytes) -> Optional[EntityState]:
    # 4 + 4 + 1 + 1 + 7*4 = 38, then [u16 len][str]
    if len(body) < 38 + 2:
        return None
    (entity_id, owner, is_self, authority,
     px, py, pz, rx, ry, rz, rw) = struct.unpack_from("<IIBB7f", body, 0)
    state = EntityState(
        entity_id=entity_id,
        owner_client_id=owner,
        is_self=bool(is_self),
        authority=authority,
        pos_x=px, pos_y=py, pos_z=pz,
        rot_x=rx, rot_y=ry, rot_z=rz, rot_w=rw,
    )
    state.label, _ = _parse_utf8_string(body, 38)
    return state


def _parse_entity_gone(body: bytes) -> Optional[int]:
    if len(body) < 4:
        return None
    (entity_id,) = struct.unpack_from("<I", body, 0)
    return entity_id


def _parse_server_state(body: bytes) -> Optional[int]:
    if len(body) < 1:
        return None
    return body[0]


# ── BridgeClient ────────────────────────────────────────────────────

class BridgeClient:
    """Thread-safe blocking+reader-thread bridge client.

    Usage:

        client = BridgeClient()
        if client.connect("127.0.0.1", 9401, timeout=5.0):
            client.send_move_self(10.0, 0.0, 0.0)
            ...
            client.disconnect()

    Callbacks (`on_hello`, `on_entity_state`, `on_entity_gone`,
    `on_server_state`, `on_disconnect`) run on the background reader
    thread. They should be cheap or marshal the work onto the caller's
    own main thread.
    """

    def __init__(self) -> None:
        self._sock: Optional[socket.socket] = None
        self._reader: Optional[threading.Thread] = None
        self._alive = False
        self._send_lock = threading.Lock()

        # Latest cached snapshots — populated by the reader thread,
        # read by the main thread via `get_hello` / `get_entities`.
        self._state_lock = threading.Lock()
        self._hello: HelloMessage = HelloMessage()
        self._entities: Dict[int, EntityState] = {}
        self._server_state: int = SERVER_PENDING

        # User callbacks (thread: reader thread).
        self.on_hello:        Optional[Callable[[HelloMessage], None]] = None
        self.on_entity_state: Optional[Callable[[EntityState], None]]  = None
        self.on_entity_gone:  Optional[Callable[[int], None]]          = None
        self.on_server_state: Optional[Callable[[int], None]]          = None
        self.on_disconnect:   Optional[Callable[[], None]]             = None

        # Observability counters.
        self.messages_inbound  = 0
        self.messages_outbound = 0
        self.assets_pushed     = 0
        self.parents_sent      = 0
        self.lights_sent       = 0
        self.cameras_sent      = 0
        self.materials_sent    = 0
        self.last_error: Optional[str] = None

    # ── Lifecycle ───────────────────────────────────────────────────

    def connect(self, host: str = "127.0.0.1",
                port: int = DEFAULT_BRIDGE_PORT,
                timeout: float = 5.0) -> bool:
        """Open the TCP connection and spawn the reader thread.

        Returns True on success. On failure, stores a human-readable
        message in `self.last_error` and leaves the client usable for
        a retry.
        """
        if self._alive:
            self.last_error = "already connected"
            return False
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect((host, port))
            # Drop the timeout back to None once we're past the
            # handshake — recv in the reader thread should block
            # forever until either the peer closes or we stop.
            s.settimeout(None)
            # Disable Nagle so short MoveSelf frames go out immediately.
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError as exc:
            self.last_error = f"connect {host}:{port} failed: {exc}"
            return False
        self._sock = s
        self._alive = True
        self.last_error = None
        self._reader = threading.Thread(
            target=self._reader_loop,
            name="sv-bridge-reader",
            daemon=True)
        self._reader.start()
        return True

    def disconnect(self) -> None:
        if not self._alive:
            return
        self._alive = False
        if self._sock is not None:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        if self._reader is not None and self._reader.is_alive():
            # Don't join from within the reader thread.
            if threading.current_thread() is not self._reader:
                self._reader.join(timeout=1.5)
        self._reader = None

    def is_connected(self) -> bool:
        return self._alive and self._sock is not None

    # ── Sending ─────────────────────────────────────────────────────

    def _send_frame(self, frame: bytes) -> bool:
        if not self._alive or self._sock is None:
            return False
        with self._send_lock:
            try:
                self._sock.sendall(frame)
            except OSError as exc:
                self.last_error = f"send failed: {exc}"
                self.disconnect()
                return False
        self.messages_outbound += 1
        return True

    def send_move_self(self, pos_x: float, pos_y: float, pos_z: float,
                       rot_x: float = 0.0, rot_y: float = 0.0,
                       rot_z: float = 0.0, rot_w: float = 1.0) -> bool:
        """Send a MoveSelf frame targeting the bridge's own avatar.

        The 7 floats are interpreted directly as the target
        NetTransform — the server will apply them via a full-mask
        SetField transaction.
        """
        return self._send_frame(build_move_self(
            pos_x, pos_y, pos_z, rot_x, rot_y, rot_z, rot_w))

    def send_ping(self) -> bool:
        return self._send_frame(build_ping())

    # ── Asset push ──────────────────────────────────────────────────

    def send_asset(self,
                   absolute_path: str,
                   asset_kind: int,
                   relative_name: Optional[str] = None,
                   chunk_size: int = ASSET_CHUNK_SIZE) -> bool:
        """Hash + announce + stream an asset file over the bridge.

        Reads `absolute_path` from disk, computes SHA-256, builds one
        Announce + N Chunks, and pumps them under the shared send lock
        so the frames stay adjacent on the wire. Returns True if every
        frame was written successfully; False on any failure with
        `self.last_error` set.
        """
        try:
            with open(absolute_path, "rb") as f:
                data = f.read()
        except OSError as exc:
            self.last_error = f"asset open failed: {exc}"
            return False
        if len(data) > ASSET_BYTE_LIMIT:
            self.last_error = (
                f"asset {os.path.basename(absolute_path)} too large "
                f"({len(data)} > {ASSET_BYTE_LIMIT})")
            return False
        if relative_name is None:
            relative_name = os.path.basename(absolute_path)

        digest = hashlib.sha256(data).digest()
        try:
            announce = build_asset_announce(
                digest, len(data), asset_kind, relative_name)
            chunks = build_asset_chunks(digest, data, chunk_size=chunk_size)
        except ValueError as exc:
            self.last_error = f"asset frame build failed: {exc}"
            return False

        if not self._alive or self._sock is None:
            self.last_error = "send_asset: not connected"
            return False

        with self._send_lock:
            try:
                self._sock.sendall(announce)
                for c in chunks:
                    self._sock.sendall(c)
            except OSError as exc:
                self.last_error = f"send_asset failed: {exc}"
                self.disconnect()
                return False
        self.messages_outbound += 1 + len(chunks)
        self.assets_pushed     += 1
        return True

    def send_asset_bytes(self,
                         data: bytes,
                         asset_kind: int,
                         relative_name: str,
                         chunk_size: int = ASSET_CHUNK_SIZE) -> bool:
        """In-memory variant of `send_asset` — useful when the asset
        bytes come from a `bpy.ops.export_scene.gltf` → read → unlink
        temp-file cycle rather than a persistent on-disk path.
        """
        if len(data) > ASSET_BYTE_LIMIT:
            self.last_error = (
                f"asset '{relative_name}' too large "
                f"({len(data)} > {ASSET_BYTE_LIMIT})")
            return False

        digest = hashlib.sha256(data).digest()
        try:
            announce = build_asset_announce(
                digest, len(data), asset_kind, relative_name)
            chunks = build_asset_chunks(digest, data, chunk_size=chunk_size)
        except ValueError as exc:
            self.last_error = f"asset frame build failed: {exc}"
            return False

        if not self._alive or self._sock is None:
            self.last_error = "send_asset_bytes: not connected"
            return False

        with self._send_lock:
            try:
                self._sock.sendall(announce)
                for c in chunks:
                    self._sock.sendall(c)
            except OSError as exc:
                self.last_error = f"send_asset_bytes failed: {exc}"
                self.disconnect()
                return False
        self.messages_outbound += 1 + len(chunks)
        self.assets_pushed     += 1
        return True

    # ── Parent sync ─────────────────────────────────────────────────

    def send_set_parent(self, parent_entity_id: int) -> bool:
        """Tell the bridge to re-parent its avatar to the given entity.

        `parent_entity_id = 0` means "no parent" (root).
        """
        if self._send_frame(build_set_parent(parent_entity_id)):
            self.parents_sent += 1
            return True
        return False

    # ── Light sync ──────────────────────────────────────────────────

    def send_set_light(self,
                       light_type:    int,
                       color_r:       float,
                       color_g:       float,
                       color_b:       float,
                       intensity:     float,
                       range_units:   float,
                       cone_inner_deg: float = 30.0,
                       cone_outer_deg: float = 45.0) -> bool:
        """Push a LightComponent SetField through the bridge.

        `light_type` is one of `LIGHT_TYPE_*`. The bridge rewraps this
        as a LightComponent SetField EditTransaction targeting its own
        avatar entity — same pattern as `send_set_parent`.
        """
        frame = build_set_light(
            light_type, color_r, color_g, color_b,
            intensity, range_units,
            cone_inner_deg, cone_outer_deg,
        )
        if self._send_frame(frame):
            self.lights_sent += 1
            return True
        return False

    # ── Camera + material sync ──────────────────────────────────────

    def send_set_camera(self,
                        fov_deg:    float,
                        aspect:     float,
                        near_plane: float,
                        far_plane:  float) -> bool:
        """Push a CameraComponent SetField through the bridge.

        Setting fov_deg = 0 (or far_plane <= near_plane) is the
        documented "no override" sentinel — the lab renderer falls
        back to its own camera mode in that case.
        """
        if self._send_frame(build_set_camera(
                fov_deg, aspect, near_plane, far_plane)):
            self.cameras_sent += 1
            return True
        return False

    def send_set_material(self,
                          base_color_r:      float,
                          base_color_g:      float,
                          base_color_b:      float,
                          override_strength: float) -> bool:
        """Push a MaterialComponent SetField through the bridge.

        Setting override_strength = 0 means the override is inert
        even if the color is non-default. The fragment shader skips
        the mix entirely, so rendering matches the 1.3.9 baseline.
        """
        if self._send_frame(build_set_material(
                base_color_r, base_color_g, base_color_b,
                override_strength)):
            self.materials_sent += 1
            return True
        return False

    # ── Snapshot accessors ──────────────────────────────────────────

    def get_hello(self) -> HelloMessage:
        with self._state_lock:
            return HelloMessage(
                bridge_client_id=self._hello.bridge_client_id,
                bridge_avatar_entity=self._hello.bridge_avatar_entity,
                scope=self._hello.scope,
                server_semver=self._hello.server_semver,
                net_transform_schema=self._hello.net_transform_schema,
                server_state=self._hello.server_state,
                app_name=self._hello.app_name,
            )

    def get_entities(self) -> List[EntityState]:
        with self._state_lock:
            return list(self._entities.values())

    def get_entity(self, entity_id: int) -> Optional[EntityState]:
        with self._state_lock:
            ent = self._entities.get(entity_id)
            if ent is None:
                return None
            # Return a copy so the caller can safely compare across
            # frames without tripping on mid-update mutations.
            return EntityState(**ent.__dict__)

    def get_server_state(self) -> int:
        with self._state_lock:
            return self._server_state

    # ── Reader thread ───────────────────────────────────────────────

    def _reader_loop(self) -> None:
        while self._alive and self._sock is not None:
            header = _read_exact(self._sock, 4)
            if header is None:
                break
            (length,) = struct.unpack("<I", header)
            if length == 0 or length > MAX_FRAME_BYTES:
                self.last_error = f"invalid frame length {length}"
                break
            body = _read_exact(self._sock, length)
            if body is None:
                break
            self.messages_inbound += 1
            if not body:
                continue
            msg_type = body[0]
            payload = body[1:]

            if msg_type == MSG_HELLO:
                hello = _parse_hello(payload)
                with self._state_lock:
                    self._hello = hello
                    self._server_state = hello.server_state
                if self.on_hello:
                    try:
                        self.on_hello(hello)
                    except Exception:
                        pass
            elif msg_type == MSG_ENTITY_STATE:
                state = _parse_entity_state(payload)
                if state is not None:
                    with self._state_lock:
                        self._entities[state.entity_id] = state
                    if self.on_entity_state:
                        try:
                            self.on_entity_state(state)
                        except Exception:
                            pass
            elif msg_type == MSG_ENTITY_GONE:
                entity_id = _parse_entity_gone(payload)
                if entity_id is not None:
                    with self._state_lock:
                        self._entities.pop(entity_id, None)
                    if self.on_entity_gone:
                        try:
                            self.on_entity_gone(entity_id)
                        except Exception:
                            pass
            elif msg_type == MSG_SERVER_STATE:
                state_byte = _parse_server_state(payload)
                if state_byte is not None:
                    with self._state_lock:
                        self._server_state = state_byte
                    if self.on_server_state:
                        try:
                            self.on_server_state(state_byte)
                        except Exception:
                            pass
            # Other types are ignored — the bridge only sends the
            # four server->client message types above.

        self._alive = False
        if self.on_disconnect:
            try:
                self.on_disconnect()
            except Exception:
                pass


# ── Convenience: one-shot blocking send ────────────────────────────

def send_move_once(host: str, port: int,
                   pos_x: float, pos_y: float, pos_z: float,
                   rot_x: float = 0.0, rot_y: float = 0.0,
                   rot_z: float = 0.0, rot_w: float = 1.0,
                   timeout: float = 5.0) -> bool:
    """Open a short-lived connection, send one MoveSelf, close.

    Useful for scripting: `python -c "from net_client import
    send_move_once; send_move_once('127.0.0.1', 9401, 12, 0, -5)"`.
    Returns True if the frame was written to the socket — does NOT
    wait for an entity-state echo, since the bridge forwards edits
    to the server asynchronously.
    """
    client = BridgeClient()
    if not client.connect(host=host, port=port, timeout=timeout):
        return False
    try:
        # Give the reader thread a moment to pull the Hello + cached
        # entity states so the caller can inspect client.get_entities()
        # if they want to.
        time.sleep(0.15)
        return client.send_move_self(pos_x, pos_y, pos_z,
                                     rot_x, rot_y, rot_z, rot_w)
    finally:
        client.disconnect()
