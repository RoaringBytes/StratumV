// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── PermissionScope ──────────────────────────────────────
// Per-client authorization level for the collaborative editing
// substrate. Every connection is assigned one of these by the
// server on accept; the client receives its own scope in the
// kFrameWelcome message that follows the schema handshake preamble.
//
// Scope is a MONOTONIC ladder — a higher value implies every lower
// privilege. Rule of thumb for checking: `scope >= required`.
//
//   Spectator (0): read-only. Can decode snapshots and see other
//                  clients, but cannot issue any edit transactions.
//   Player    (1): can issue SetField transactions against components
//                  tagged Authority::Owner that the client owns (its
//                  own avatar's NetTransform, etc.).
//   Editor    (2): can issue SetField transactions against components
//                  tagged Authority::Editor, plus Undo/Redo of their
//                  own applied transactions. This is the default scope.
//   Admin     (3): everything Editor can do, plus Spawn/Despawn of
//                  server-authoritative entities. The current build does
//                  not expose any additional Admin operations — the
//                  server still owns entity lifecycle — but the
//                  enum is in the ladder so later work can
//                  layer Admin-only actions (kick, scope change,
//                  world reset) without a wire format bump.
//
// The ladder is intentionally flat. Finer categories (Asset-only,
// Transform-only, Script-only) can be layered later without
// changing the wire format — the server just keeps a per-category
// bitmask alongside the scope value. The flat ladder is used
// because there is NO explicit locking for concurrent edits
// (last-write-wins is the conflict policy).
//
// See REPLICATION_CONTRACT.md §2 (Authority), NETWORK_DESIGN.md §5
// (permissions), and COLLAB_EDITING.md for the full design
// rationale.
//
// Header-only: the inline body is short enough that a .cpp TU
// would just be boilerplate. Kept in the core subset (see
// cmake/stratumv_core_sources.cmake) so that both the headless
// dedicated server and the graphics-facing lab harness can include
// this without pulling anything extra.

#include <cstdint>

namespace sv {

enum class PermissionScope : uint8_t {
    Spectator = 0,
    Player    = 1,
    Editor    = 2,
    Admin     = 3,
};

inline const char* permissionScopeToString(PermissionScope s) {
    switch (s) {
        case PermissionScope::Spectator: return "Spectator";
        case PermissionScope::Player:    return "Player";
        case PermissionScope::Editor:    return "Editor";
        case PermissionScope::Admin:     return "Admin";
    }
    return "Unknown";
}

// Scoped enums do not auto-convert, so explicit comparison
// operators are required for the `scope >= required` idiom.
inline constexpr bool operator>=(PermissionScope a, PermissionScope b) {
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b);
}
inline constexpr bool operator> (PermissionScope a, PermissionScope b) {
    return static_cast<uint8_t>(a) >  static_cast<uint8_t>(b);
}
inline constexpr bool operator<=(PermissionScope a, PermissionScope b) {
    return static_cast<uint8_t>(a) <= static_cast<uint8_t>(b);
}
inline constexpr bool operator< (PermissionScope a, PermissionScope b) {
    return static_cast<uint8_t>(a) <  static_cast<uint8_t>(b);
}

// Safe narrowing — returns Spectator (the least-privileged level)
// on any out-of-range input so a corrupted wire byte cannot elevate
// a client to Admin by accident.
inline PermissionScope permissionScopeFromByte(uint8_t b) {
    switch (b) {
        case 0: return PermissionScope::Spectator;
        case 1: return PermissionScope::Player;
        case 2: return PermissionScope::Editor;
        case 3: return PermissionScope::Admin;
        default: return PermissionScope::Spectator;
    }
}

} // namespace sv
