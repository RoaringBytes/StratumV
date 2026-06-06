// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── UndoLog ────────────────────────────────────────────────────────
// Server-side linear history of applied SetField transactions on
// replicated components. Each entry records the pre-state and
// post-state needed to roll forward / roll back on request.
//
// ── Semantics ──────────────────────────────────────────────────────
//
// Undo/redo is per-client, LIFO, O(N) walk-back. The log is a
// single shared vector of entries from every client; the per-client
// split is maintained implicitly via the `originClientId` filter on
// the walk-back helpers. This is the conservative design choice:
//
//   "per-client history cursor" — each client has its own view of
//   the log, but the underlying storage is shared so late joiners
//   can still see a coherent snapshot if a future revision adds
//   join-with-snapshot.
//
// `findLatestUndoable(clientId)` walks the history backward and
// returns the most recent entry authored by `clientId` that is NOT
// yet marked `undone`. `findLatestRedoable(clientId)` does the
// mirror walk for entries that ARE marked `undone`.
//
// The log does NOT prune stale redo state when a new SetField is
// appended — this accepts the mild UX quirk (a client that does
// "edit, undo, edit" can still redo the first undo) in exchange for
// simpler semantics. The branch-pruning editor convention is a
// later refinement; it requires per-client cursors and a
// tree data structure, and is explicitly out of scope here:
// there is NO explicit locking for concurrent edits, and
// last-write-wins is the conflict policy.
//
// Scope gating is the CALLER's responsibility. The log itself is
// authoritatively untyped about scope — it just holds the entries.
// The server-side dispatcher in stratumv_server/main.cpp checks
// `clientState.scope >= PermissionScope::Editor` before invoking
// any mutation on the log.
//
// ── Header-only ────────────────────────────────────────────────────
//
// The implementation is short enough that a .cpp translation unit
// would just be boilerplate. Placed in the core subset via
// inclusion from stratumv_server/main.cpp + tests/test_EditTransaction.cpp;
// no separate entry in cmake/stratumv_core_sources.cmake is needed.

#include "EditTransaction.h"
#include "NetTransform.h"

#include <cstdint>
#include <vector>

namespace sv {

// One applied SetField transaction, kept server-side so the server
// can roll back on request. Current scope is NetTransform-only;
// later revisions can generalize the before/after payload via a
// serialized byte blob keyed by typeNameHash.
struct UndoEntry {
    uint64_t      txId           = 0;
    uint32_t      originClientId = 0;
    uint32_t      entityId       = 0;
    uint32_t      typeNameHash   = 0;
    NetTransform  beforeState    {};
    NetTransform  afterState     {};
    bool          undone         = false;  // toggled by markUndone/markRedone
};

class UndoLog {
public:
    // Append an applied transaction to the log. The entry is
    // initially in the "applied" state (undone=false). Callers are
    // expected to set every field of `entry` before calling.
    void recordApplied(const UndoEntry& entry) {
        m_entries.push_back(entry);
    }

    // Walk back and return the most recent entry authored by
    // `clientId` that is NOT yet marked undone. Returns nullptr if
    // none. The returned pointer is valid until the next mutation
    // on the log (recordApplied / clear).
    const UndoEntry* findLatestUndoable(uint32_t clientId) const {
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            if (it->originClientId == clientId && !it->undone) {
                return &(*it);
            }
        }
        return nullptr;
    }

    // Walk back and return the most recent entry authored by
    // `clientId` that IS currently marked undone. Used by Redo.
    const UndoEntry* findLatestRedoable(uint32_t clientId) const {
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            if (it->originClientId == clientId && it->undone) {
                return &(*it);
            }
        }
        return nullptr;
    }

    // Mark the entry with the given txId as undone. Returns true if
    // found, false if no such txId exists. First-match semantics —
    // txIds are server-assigned monotonic so collision is not
    // possible within a single server process lifetime.
    bool markUndone(uint64_t txId) {
        for (UndoEntry& e : m_entries) {
            if (e.txId == txId) {
                e.undone = true;
                return true;
            }
        }
        return false;
    }

    // Inverse of markUndone — clears the undone flag.
    bool markRedone(uint64_t txId) {
        for (UndoEntry& e : m_entries) {
            if (e.txId == txId) {
                e.undone = false;
                return true;
            }
        }
        return false;
    }

    size_t size()  const { return m_entries.size(); }
    bool   empty() const { return m_entries.empty(); }
    void   clear()       { m_entries.clear(); }

    // Count how many entries authored by `clientId` are currently
    // undoable (originClientId matches AND !undone). Diagnostic
    // helper for the server heartbeat log and the AdminPanel Edit
    // tab.
    size_t undoableCount(uint32_t clientId) const {
        size_t n = 0;
        for (const UndoEntry& e : m_entries) {
            if (e.originClientId == clientId && !e.undone) ++n;
        }
        return n;
    }

    size_t redoableCount(uint32_t clientId) const {
        size_t n = 0;
        for (const UndoEntry& e : m_entries) {
            if (e.originClientId == clientId && e.undone) ++n;
        }
        return n;
    }

    // Read-only access for tests + diagnostic output.
    const std::vector<UndoEntry>& entries() const { return m_entries; }

private:
    std::vector<UndoEntry> m_entries;
};

} // namespace sv
