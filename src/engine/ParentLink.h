// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── ParentLink ─────────────────────────────────────
// Second replicated component. Expresses a parent/child relationship
// between replicated entities as a single u32 pointing at the parent's
// entityId. Default 0 means "no parent" (root-level entity).
//
// ── Why a separate component instead of a NetTransform field? ────────
//
// Parenting is a RELATIONSHIP, not part of the transform. Merging the
// parentEntityId into NetTransform would:
//   (a) bump NetTransform's schemaVersion — which would invalidate
//       every persisted `world.svbin` file from earlier
//       sessions (the persistence format is schema-gated)
//   (b) grow every entity's per-tick datagram snapshot by 4 bytes
//       whether or not it has a parent (most don't)
//   (c) conflate two independent concepts into one wire payload
//
// A separate SV_REPLICATE'd component avoids all three and lets the
// ParentLink flow on its own wire path — SetField transactions only,
// never datagram snapshots. Parenting changes are rare compared to
// transform updates, so the reliable-stream cost is trivial.
//
// ── SetField-only (never Spawn) ──────────────────────────────────────
//
// ParentLink is never part of the Spawn transaction flow. New entities
// appear with an implicit `parentEntityId = 0` (root). If a client
// wants to re-parent an entity, it issues an Editor-scope SetField
// transaction targeting the ParentLink component on that entity. The
// server validates scope + ownership, applies the new parent, and
// rebroadcasts the SetField to every welcomed client (same wire log
// path NetTransform SetFields use).
//
// Why no Spawn? Because persistence only serialises
// Authority::Server entities, and the only server-owned entity in the
// current demo (the orbiting cube) has no parent. Keeping ParentLink
// out of the Spawn path means the `world.svbin` layout is
// byte-identical across the 1.3.7 → 1.3.8 bump. A later session that
// needs server-owned parented entities can add a second generic
// Spawn path keyed by component typeNameHash.
//
// ── Authority ────────────────────────────────────────────────────────
//
// Authority::Owner — a client may only re-parent entities it owns,
// matching the NetTransform owner-authority contract.
// The server enforces the same owner-authority gate already used for
// NetTransform SetField: `ent.authority != Server && (ent.authority !=
// Owner || ent.ownerClientId == cs.clientId)`.
//
// ── Wire layout (by way of encodeSnapshot) ───────────────────────────
//
//   [u16 schemaVersion]
//   [1 byte packed DirtyMask]
//   [u32 parentEntityId]         — only present when the dirty bit is set
//
// Total wire size with the single field marked dirty: 2 (schema) + 1
// (mask) + 4 (u32) = 7 bytes inside the EditTransaction payload.

#include "ReplicationRegistry.h"

namespace sv {

struct ParentLink {
    // entityId of the parent, or 0 for "no parent" (root-level).
    // The server does NOT enforce acyclic parenting today — a client
    // that sets A's parent to B while B's parent is A will produce a
    // cycle that downstream code has to tolerate. Cycle detection is
    // a later-session refinement when renderer uses ParentLink to
    // compose world matrices.
    uint32_t parentEntityId = 0;
};

// ADL helper forward declaration — the .cpp holds the SV_REPLICATE
// macro expansion, which defines this function in the sv:: namespace.
const ReplicationMeta& sv_buildReplicationMetaFor(ParentLink*);

// ── Explicit registration entry point ────────────────────────────────
// Same anchor pattern as `ensureNetTransformRegistered` in
// NetTransform.h. stratumv.lib + stratumv_core.lib are both STATIC
// archives, so the SV_REPLICATE file-scope static in ParentLink.cpp
// gets dead-stripped unless a non-inline symbol in that TU is
// referenced from the linking exe. stratumv_server and skinned_test
// call this from main() before any code that walks the registry by
// name.
const ReplicationMeta& ensureParentLinkRegistered();

// Wire-size constant mirroring kNetTransformFieldCount.
constexpr uint32_t kParentLinkFieldCount = 1;

} // namespace sv
