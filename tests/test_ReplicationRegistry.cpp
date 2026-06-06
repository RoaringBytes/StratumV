// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── /ReplicationRegistry unit tests ─────────────────
// Tests for sv::ReplicationRegistry, sv::ReplicationMeta,
// sv::FieldDesc, sv::DirtyMask, the SV_REPLICATE / SV_FIELD /
// SV_FIELD_QUANT / SV_DIRTY / SV_COMPONENT_AUTHORITY macro surface,
// and the SnapshotWriter / SnapshotReader / encodeSnapshot /
// decodeSnapshot primitives (src/engine/ReplicationRegistry.h/.cpp).
//
// coverage:
//   - Static-init registration via SV_REPLICATE
//   - FieldDesc offset/size/type/dirtyBit/quantStep correctness
//   - ReplicationMeta::findField / findFieldByHash / fieldIndex
//   - ReplicationRegistry::find / findByHash / size / resetForTests
//   - Schema version stability + sensitivity to rename + type change
//   - DirtyMask set/clear/test/any/count/packed/spilled/resize/setAll/reset
//   - Overflow path for components with >64 fields
//   - SV_DIRTY returns the correct bit index
//
// coverage (this file, added):
//   - Authority enum default + authorityToString
//   - SV_COMPONENT_AUTHORITY macro program-start effect
//   - ReplicationRegistry::setAuthority: patch + unknown-type reject
//   - SnapshotWriter/Reader primitives (raw, varint, zigzag, float,
//     double, bool, quantized float)
//   - encodeSnapshot + decodeSnapshot full-mask round-trip
//   - encodeSnapshot + decodeSnapshot partial-mask round-trip
//   - encodeSnapshot + decodeSnapshot empty-mask round-trip
//   - encodeSnapshot + decodeSnapshot quantized component
//   - decodeSnapshot rejects schema mismatch + truncated buffers
//   - encodeSnapshot rejects DirtyMask size mismatch

#include "ReplicationRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using sv::Authority;
using sv::DirtyMask;
using sv::FieldDesc;
using sv::FieldType;
using sv::ReplicationMeta;
using sv::ReplicationRegistry;
using sv::SnapshotReader;
using sv::SnapshotWriter;
using sv::authorityToString;
using sv::decodeSnapshot;
using sv::encodeSnapshot;
using sv::fieldTypeFor;
using sv::fieldTypeToString;
using sv::fnv1a32;
using Catch::Matchers::WithinAbs;

// ── Synthetic test components ─────────────────────────────────────
// Defined at file scope so SV_REPLICATE's static-init trigger fires
// when sv_tests.exe starts. Tests rely on the helper rebuildTestRegistry()
// to restore this state after any resetForTests() call.

struct ReplTestScalar {
    int32_t id;
    float   x;
    float   y;
    float   z;
    uint8_t health;
    bool    alive;
};

SV_REPLICATE(ReplTestScalar,
    SV_FIELD(id),
    SV_FIELD(x),
    SV_FIELD(y),
    SV_FIELD(z),
    SV_FIELD(health),
    SV_FIELD(alive)
);

struct ReplTestQuant {
    float position;
    float yaw;
};

SV_REPLICATE(ReplTestQuant,
    SV_FIELD_QUANT(position, 0.001f),
    SV_FIELD_QUANT(yaw,      0.01f)
);

// Synthetic rename variant — same fields as ReplTestScalar but with
// one field renamed, used to verify schema version sensitivity.
struct ReplTestScalarRenamed {
    int32_t id;
    float   px;  // renamed from x
    float   y;
    float   z;
    uint8_t health;
    bool    alive;
};

// Not registered via SV_REPLICATE — tests build its meta manually to
// avoid polluting the runtime registry with a near-duplicate type.

// Synthetic type-changed variant — same names as ReplTestScalar but
// one field changes type, used to verify schema version sensitivity.
struct ReplTestScalarRetyped {
    int32_t id;
    int32_t x;  // was float
    float   y;
    float   z;
    uint8_t health;
    bool    alive;
};

// ── Big component for overflow test ───────────────────────────────
// 72 uint8 fields → exceeds the 64-bit packed fast path. Only its
// FIELD COUNT matters to the DirtyMask test; the registry side is
// covered by ReplTestScalar.
struct ReplTestBig {
    uint8_t f[72];
};

// A completely opaque test struct used to verify the Unknown
// fallback of the primary fieldTypeFor<T>() template.
struct ReplTestOpaque { int x; float y; };

// ── Authority + snapshot fixture ──────────────────────────
// ReplTestTagged exercises SV_COMPONENT_AUTHORITY and serves as a
// secondary snapshot fixture (smaller + quantized) alongside
// ReplTestScalar. The Authority::Owner tag is captured at program
// start into g_initialTaggedAuthority below so the macro smoke test
// can verify its effect without relying on test ordering.

struct ReplTestTagged {
    int32_t id;
    float   speed;
};

SV_REPLICATE(ReplTestTagged,
    SV_FIELD(id),
    SV_FIELD_QUANT(speed, 0.01f)
);

SV_COMPONENT_AUTHORITY(ReplTestTagged, sv::Authority::Owner);

namespace {

// Captured during static init, AFTER the SV_COMPONENT_AUTHORITY
// trigger above runs (C++ guarantees file-scope initializers in a
// single TU run in declaration order). Used by the program-start
// smoke test to verify the macro actually took effect, without
// relying on any test-side helper that could mask a regression.
const Authority g_initialTaggedAuthority = []() {
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestTagged");
    return meta ? meta->authority : Authority::None;
}();

} // anonymous

// ── Helpers ───────────────────────────────────────────────────────

namespace {

// Rebuild the registry to a known state by wiping it and re-invoking
// each test component's sv_buildReplicationMetaFor() helper. Call at
// the top of any test that reads from the registry.
//
// Note: SV_COMPONENT_AUTHORITY's file-scope trigger only runs once
// at program start, so after resetForTests() the tagged authority
// is back to the default (Server). rebuildTestRegistry() re-applies
// the tag explicitly — tests that want to verify the macro's
// program-start behaviour use g_initialTaggedAuthority instead.
void rebuildTestRegistry() {
    ReplicationRegistry::get().resetForTests();
    sv_buildReplicationMetaFor(static_cast<ReplTestScalar*>(nullptr));
    sv_buildReplicationMetaFor(static_cast<ReplTestQuant*>(nullptr));
    sv_buildReplicationMetaFor(static_cast<ReplTestTagged*>(nullptr));
    ReplicationRegistry::get().setAuthority("ReplTestTagged", Authority::Owner);
}

} // anonymous

// ── fnv1a32 hash ──────────────────────────────────────────────────

TEST_CASE("ReplicationRegistry: fnv1a32 empty string is FNV offset basis",
          "[replication][hash]") {
    // FNV-1a 32-bit offset basis
    REQUIRE(fnv1a32("") == 2166136261u);
}

TEST_CASE("ReplicationRegistry: fnv1a32 known values are stable",
          "[replication][hash]") {
    // Spot-check a handful of inputs so a silent regression in the
    // hash implementation is caught immediately. Values precomputed
    // via a reference FNV-1a 32-bit encoder.
    REQUIRE(fnv1a32("a")                == 0xe40c292cu);
    REQUIRE(fnv1a32("foobar")           == 0xbf9cf968u);
    REQUIRE(fnv1a32("ReplTestScalar")  != 0u);
    REQUIRE(fnv1a32("ReplTestScalar")  != fnv1a32("ReplTestQuant"));
}

// ── fieldTypeFor<T>() dispatch ────────────────────────────────────

TEST_CASE("ReplicationRegistry: fieldTypeFor maps scalars to enum",
          "[replication][fieldtype]") {
    REQUIRE(fieldTypeFor<bool>()     == FieldType::Bool);
    REQUIRE(fieldTypeFor<int8_t>()   == FieldType::Int8);
    REQUIRE(fieldTypeFor<int16_t>()  == FieldType::Int16);
    REQUIRE(fieldTypeFor<int32_t>()  == FieldType::Int32);
    REQUIRE(fieldTypeFor<int64_t>()  == FieldType::Int64);
    REQUIRE(fieldTypeFor<uint8_t>()  == FieldType::UInt8);
    REQUIRE(fieldTypeFor<uint16_t>() == FieldType::UInt16);
    REQUIRE(fieldTypeFor<uint32_t>() == FieldType::UInt32);
    REQUIRE(fieldTypeFor<uint64_t>() == FieldType::UInt64);
    REQUIRE(fieldTypeFor<float>()    == FieldType::Float);
    REQUIRE(fieldTypeFor<double>()   == FieldType::Double);
}

TEST_CASE("ReplicationRegistry: fieldTypeFor<Unspecialized> returns Unknown",
          "[replication][fieldtype]") {
    // The primary template returns Unknown. Games add their own
    // specializations for glm::vec3 / custom enums / etc.
    REQUIRE(fieldTypeFor<ReplTestOpaque>() == FieldType::Unknown);
}

TEST_CASE("ReplicationRegistry: fieldTypeToString covers every enum value",
          "[replication][fieldtype]") {
    FieldType kinds[] = {
        FieldType::Unknown,  FieldType::Bool,      FieldType::Int8,
        FieldType::Int16,    FieldType::Int32,     FieldType::Int64,
        FieldType::UInt8,    FieldType::UInt16,    FieldType::UInt32,
        FieldType::UInt64,   FieldType::Float,     FieldType::Double,
        FieldType::Vec2,     FieldType::Vec3,      FieldType::Vec4,
        FieldType::Quat,     FieldType::EntityHandle,
        FieldType::StringFixed, FieldType::Enum,   FieldType::Blob
    };
    for (FieldType t : kinds) {
        const char* s = fieldTypeToString(t);
        REQUIRE(s != nullptr);
        REQUIRE(std::strlen(s) > 0);
    }
    REQUIRE(std::string(fieldTypeToString(FieldType::Float)) == "Float");
    REQUIRE(std::string(fieldTypeToString(FieldType::Int32)) == "Int32");
}

// ── SV_REPLICATE macro: field layout ──────────────────────────────

TEST_CASE("ReplicationRegistry: SV_REPLICATE registers ReplTestScalar",
          "[replication][macro]") {
    rebuildTestRegistry();

    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->typeName == "ReplTestScalar");
    REQUIRE(meta->typeSize == sizeof(ReplTestScalar));
    REQUIRE(meta->typeNameHash == fnv1a32("ReplTestScalar"));
    REQUIRE(meta->fields.size() == 6);
    REQUIRE(meta->fieldCount() == 6);
    REQUIRE(meta->schemaVersion != 0);
}

TEST_CASE("ReplicationRegistry: SV_FIELD offsets match offsetof",
          "[replication][macro]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    REQUIRE(meta->fields[0].offset == offsetof(ReplTestScalar, id));
    REQUIRE(meta->fields[1].offset == offsetof(ReplTestScalar, x));
    REQUIRE(meta->fields[2].offset == offsetof(ReplTestScalar, y));
    REQUIRE(meta->fields[3].offset == offsetof(ReplTestScalar, z));
    REQUIRE(meta->fields[4].offset == offsetof(ReplTestScalar, health));
    REQUIRE(meta->fields[5].offset == offsetof(ReplTestScalar, alive));
}

TEST_CASE("ReplicationRegistry: SV_FIELD sizes match sizeof",
          "[replication][macro]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    REQUIRE(meta->fields[0].size == sizeof(ReplTestScalar::id));
    REQUIRE(meta->fields[1].size == sizeof(ReplTestScalar::x));
    REQUIRE(meta->fields[4].size == sizeof(ReplTestScalar::health));
    REQUIRE(meta->fields[5].size == sizeof(ReplTestScalar::alive));
}

TEST_CASE("ReplicationRegistry: SV_FIELD types map via fieldTypeFor",
          "[replication][macro]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    REQUIRE(meta->fields[0].type == FieldType::Int32);   // id
    REQUIRE(meta->fields[1].type == FieldType::Float);   // x
    REQUIRE(meta->fields[2].type == FieldType::Float);   // y
    REQUIRE(meta->fields[3].type == FieldType::Float);   // z
    REQUIRE(meta->fields[4].type == FieldType::UInt8);   // health
    REQUIRE(meta->fields[5].type == FieldType::Bool);    // alive
}

TEST_CASE("ReplicationRegistry: dirty bit indices are sequential in declaration order",
          "[replication][macro]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    for (size_t i = 0; i < meta->fields.size(); ++i) {
        REQUIRE(meta->fields[i].dirtyBit == static_cast<uint8_t>(i));
    }
}

TEST_CASE("ReplicationRegistry: field name hashes are populated",
          "[replication][macro]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    REQUIRE(meta->fields[0].nameHash == fnv1a32("id"));
    REQUIRE(meta->fields[1].nameHash == fnv1a32("x"));
    REQUIRE(meta->fields[5].nameHash == fnv1a32("alive"));

    // No two fields share a hash in this fixture.
    for (size_t i = 0; i < meta->fields.size(); ++i) {
        for (size_t j = i + 1; j < meta->fields.size(); ++j) {
            REQUIRE(meta->fields[i].nameHash != meta->fields[j].nameHash);
        }
    }
}

TEST_CASE("ReplicationRegistry: SV_FIELD_QUANT carries quantStep",
          "[replication][macro][quant]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestQuant");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == 2);

    REQUIRE_THAT(meta->fields[0].quantStep, WithinAbs(0.001f, 1e-6f));
    REQUIRE_THAT(meta->fields[1].quantStep, WithinAbs(0.01f,  1e-6f));

    // Plain SV_FIELD without QUANT leaves step at 0 (no quantization).
    const ReplicationMeta* scalarMeta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(scalarMeta != nullptr);
    for (const FieldDesc& f : scalarMeta->fields) {
        REQUIRE_THAT(f.quantStep, WithinAbs(0.0f, 1e-9f));
    }
}

// ── ReplicationMeta lookups ───────────────────────────────────────

TEST_CASE("ReplicationRegistry: findField looks up by name",
          "[replication][lookup]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    const FieldDesc* idField = meta->findField("id");
    REQUIRE(idField != nullptr);
    REQUIRE(idField->name == "id");
    REQUIRE(idField->type == FieldType::Int32);

    const FieldDesc* aliveField = meta->findField("alive");
    REQUIRE(aliveField != nullptr);
    REQUIRE(aliveField->type == FieldType::Bool);
    REQUIRE(aliveField->dirtyBit == 5);
}

TEST_CASE("ReplicationRegistry: findField returns nullptr on miss",
          "[replication][lookup]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->findField("nonexistent") == nullptr);
    REQUIRE(meta->findField("")            == nullptr);
    REQUIRE(meta->fieldIndex("nonexistent") == -1);
}

TEST_CASE("ReplicationRegistry: findFieldByHash matches findField",
          "[replication][lookup]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    for (const FieldDesc& f : meta->fields) {
        const FieldDesc* byName = meta->findField(f.name);
        const FieldDesc* byHash = meta->findFieldByHash(f.nameHash);
        REQUIRE(byName == byHash);
        REQUIRE(byHash != nullptr);
        REQUIRE(byHash->name == f.name);
    }
}

TEST_CASE("ReplicationRegistry: fieldIndex returns declaration order",
          "[replication][lookup]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    REQUIRE(meta->fieldIndex("id")     == 0);
    REQUIRE(meta->fieldIndex("x")      == 1);
    REQUIRE(meta->fieldIndex("y")      == 2);
    REQUIRE(meta->fieldIndex("z")      == 3);
    REQUIRE(meta->fieldIndex("health") == 4);
    REQUIRE(meta->fieldIndex("alive")  == 5);
    REQUIRE(meta->fieldIndex("missing") == -1);
}

// ── ReplicationRegistry singleton ─────────────────────────────────

TEST_CASE("ReplicationRegistry: find returns nullptr for unknown type",
          "[replication][registry]") {
    rebuildTestRegistry();
    REQUIRE(ReplicationRegistry::get().find("NoSuchType")       == nullptr);
    REQUIRE(ReplicationRegistry::get().findByHash(0xdeadbeefu)  == nullptr);
}

TEST_CASE("ReplicationRegistry: findByHash matches find",
          "[replication][registry]") {
    rebuildTestRegistry();
    const ReplicationMeta* byName = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(byName != nullptr);
    const ReplicationMeta* byHash = ReplicationRegistry::get().findByHash(byName->typeNameHash);
    REQUIRE(byHash == byName);
}

TEST_CASE("ReplicationRegistry: two components register independently",
          "[replication][registry]") {
    rebuildTestRegistry();
    REQUIRE(ReplicationRegistry::get().size() >= 2);

    const ReplicationMeta* a = ReplicationRegistry::get().find("ReplTestScalar");
    const ReplicationMeta* b = ReplicationRegistry::get().find("ReplTestQuant");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);
    REQUIRE(a->typeNameHash != b->typeNameHash);
    REQUIRE(a->schemaVersion != b->schemaVersion);
}

TEST_CASE("ReplicationRegistry: resetForTests drains the registry",
          "[replication][registry]") {
    // Fresh state first so the test starts from a known baseline.
    rebuildTestRegistry();
    REQUIRE(ReplicationRegistry::get().size() >= 2);

    ReplicationRegistry::get().resetForTests();
    REQUIRE(ReplicationRegistry::get().empty());
    REQUIRE(ReplicationRegistry::get().size() == 0);
    REQUIRE(ReplicationRegistry::get().find("ReplTestScalar") == nullptr);

    // Re-invoking the macro-emitted helper repopulates the entry —
    // the inline function builds a fresh meta on every call.
    sv_buildReplicationMetaFor(static_cast<ReplTestScalar*>(nullptr));
    REQUIRE(ReplicationRegistry::get().find("ReplTestScalar") != nullptr);
    REQUIRE(ReplicationRegistry::get().size() == 1);
}

TEST_CASE("ReplicationRegistry: all() returns every registered meta",
          "[replication][registry]") {
    rebuildTestRegistry();
    auto v = ReplicationRegistry::get().all();
    REQUIRE(v.size() == ReplicationRegistry::get().size());
    REQUIRE(v.size() >= 2);

    // Each entry must be non-null and match the registry's find().
    for (const ReplicationMeta* m : v) {
        REQUIRE(m != nullptr);
        REQUIRE(ReplicationRegistry::get().find(m->typeName) == m);
    }
}

// ── Schema version sensitivity ────────────────────────────────────

TEST_CASE("ReplicationRegistry: schema version is stable across calls",
          "[replication][schema]") {
    rebuildTestRegistry();
    const ReplicationMeta* a = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(a != nullptr);
    uint16_t firstVersion = a->schemaVersion;

    // Re-register the same type — schema version must not drift.
    sv_buildReplicationMetaFor(static_cast<ReplTestScalar*>(nullptr));
    const ReplicationMeta* b = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(b != nullptr);
    REQUIRE(b->schemaVersion == firstVersion);
}

TEST_CASE("ReplicationRegistry: schema version changes when a field is renamed",
          "[replication][schema]") {
    // Build two metas manually so we don't pollute the singleton
    // with the "renamed" variant. Simulates what the macro would do.
    ReplicationMeta original;
    original.typeName = "ReplTestScalar";
    original.fields.push_back(FieldDesc{"id",     0, 0, 4, FieldType::Int32, 0, 0.0f});
    original.fields.push_back(FieldDesc{"x",      0, 0, 4, FieldType::Float, 1, 0.0f});

    ReplicationMeta renamed;
    renamed.typeName = "ReplTestScalar";
    renamed.fields.push_back(FieldDesc{"id",     0, 0, 4, FieldType::Int32, 0, 0.0f});
    renamed.fields.push_back(FieldDesc{"px",     0, 0, 4, FieldType::Float, 1, 0.0f});

    // Compute schema version the same way svRegisterReplicatedType does.
    auto computeSchemaVersion = [](const ReplicationMeta& m) -> uint16_t {
        uint32_t h = 2166136261u;
        for (const FieldDesc& f : m.fields) {
            for (char c : f.name) {
                h ^= static_cast<uint8_t>(c);
                h *= 16777619u;
            }
            h ^= static_cast<uint8_t>(f.type);
            h *= 16777619u;
        }
        return static_cast<uint16_t>((h ^ (h >> 16)) & 0xFFFFu);
    };

    REQUIRE(computeSchemaVersion(original) != computeSchemaVersion(renamed));
}

TEST_CASE("ReplicationRegistry: schema version changes when a field type changes",
          "[replication][schema]") {
    ReplicationMeta original;
    original.typeName = "ReplTestScalar";
    original.fields.push_back(FieldDesc{"id",     0, 0, 4, FieldType::Int32, 0, 0.0f});
    original.fields.push_back(FieldDesc{"x",      0, 0, 4, FieldType::Float, 1, 0.0f});

    ReplicationMeta retyped;
    retyped.typeName = "ReplTestScalar";
    retyped.fields.push_back(FieldDesc{"id",      0, 0, 4, FieldType::Int32, 0, 0.0f});
    retyped.fields.push_back(FieldDesc{"x",       0, 0, 4, FieldType::Int32, 1, 0.0f});  // was Float

    auto computeSchemaVersion = [](const ReplicationMeta& m) -> uint16_t {
        uint32_t h = 2166136261u;
        for (const FieldDesc& f : m.fields) {
            for (char c : f.name) {
                h ^= static_cast<uint8_t>(c);
                h *= 16777619u;
            }
            h ^= static_cast<uint8_t>(f.type);
            h *= 16777619u;
        }
        return static_cast<uint16_t>((h ^ (h >> 16)) & 0xFFFFu);
    };

    REQUIRE(computeSchemaVersion(original) != computeSchemaVersion(retyped));
}

// ── SV_DIRTY macro ────────────────────────────────────────────────

TEST_CASE("ReplicationRegistry: SV_DIRTY returns the correct bit index",
          "[replication][macro][dirty]") {
    rebuildTestRegistry();

    REQUIRE(SV_DIRTY(ReplTestScalar, id)     == 0);
    REQUIRE(SV_DIRTY(ReplTestScalar, x)      == 1);
    REQUIRE(SV_DIRTY(ReplTestScalar, y)      == 2);
    REQUIRE(SV_DIRTY(ReplTestScalar, z)      == 3);
    REQUIRE(SV_DIRTY(ReplTestScalar, health) == 4);
    REQUIRE(SV_DIRTY(ReplTestScalar, alive)  == 5);
}

// ── DirtyMask: default state ──────────────────────────────────────

TEST_CASE("ReplicationRegistry: DirtyMask default state is empty",
          "[replication][dirtymask]") {
    DirtyMask m;
    REQUIRE(m.size() == 0);
    REQUIRE(m.packed() == 0);
    REQUIRE_FALSE(m.spilled());
    REQUIRE(m.any() == false);
    REQUIRE(m.none() == true);
    REQUIRE(m.count() == 0);
    REQUIRE(m.test(0) == false);
}

// ── DirtyMask: packed (<=64 fields) ───────────────────────────────

TEST_CASE("ReplicationRegistry: DirtyMask set/test/clear single bit",
          "[replication][dirtymask]") {
    DirtyMask m(8);
    REQUIRE(m.size() == 8);
    REQUIRE_FALSE(m.spilled());

    m.set(3);
    REQUIRE(m.test(3));
    REQUIRE(m.any());
    REQUIRE_FALSE(m.none());
    REQUIRE(m.count() == 1);
    REQUIRE(m.packed() == (uint64_t{1} << 3));

    m.set(5);
    REQUIRE(m.test(3));
    REQUIRE(m.test(5));
    REQUIRE(m.count() == 2);

    m.clear(3);
    REQUIRE_FALSE(m.test(3));
    REQUIRE(m.test(5));
    REQUIRE(m.count() == 1);
}

TEST_CASE("ReplicationRegistry: DirtyMask out-of-range set/clear is a no-op",
          "[replication][dirtymask]") {
    DirtyMask m(4);
    m.set(10);
    REQUIRE(m.count() == 0);
    REQUIRE_FALSE(m.test(10));
    m.clear(10);
    REQUIRE(m.count() == 0);
    // Hitting exactly fieldCount is also out of range (bits are 0-indexed).
    m.set(4);
    REQUIRE(m.count() == 0);
}

TEST_CASE("ReplicationRegistry: DirtyMask setAll / reset",
          "[replication][dirtymask]") {
    DirtyMask m(12);
    m.setAll();
    REQUIRE(m.count() == 12);
    REQUIRE(m.any());
    for (size_t i = 0; i < 12; ++i) REQUIRE(m.test(i));
    REQUIRE_FALSE(m.test(12));  // out of range

    m.reset();
    REQUIRE(m.count() == 0);
    REQUIRE(m.none());
    for (size_t i = 0; i < 12; ++i) REQUIRE_FALSE(m.test(i));
}

TEST_CASE("ReplicationRegistry: DirtyMask packed fast path at exactly 64 fields",
          "[replication][dirtymask]") {
    DirtyMask m(64);
    REQUIRE_FALSE(m.spilled());
    m.setAll();
    REQUIRE(m.packed() == ~uint64_t{0});
    REQUIRE(m.count() == 64);
    REQUIRE(m.test(0));
    REQUIRE(m.test(63));
    REQUIRE_FALSE(m.test(64));
}

// ── DirtyMask: overflow (>64 fields) ──────────────────────────────

TEST_CASE("ReplicationRegistry: DirtyMask overflow for >64 fields (spilled)",
          "[replication][dirtymask][overflow]") {
    DirtyMask m(100);
    REQUIRE(m.size() == 100);
    REQUIRE(m.spilled());
    REQUIRE(m.count() == 0);

    m.set(0);
    m.set(63);
    m.set(64);   // first overflow bit
    m.set(99);   // last valid bit

    REQUIRE(m.test(0));
    REQUIRE(m.test(63));
    REQUIRE(m.test(64));
    REQUIRE(m.test(99));
    REQUIRE_FALSE(m.test(100));
    REQUIRE(m.count() == 4);

    m.clear(64);
    REQUIRE_FALSE(m.test(64));
    REQUIRE(m.count() == 3);
}

TEST_CASE("ReplicationRegistry: DirtyMask setAll covers overflow",
          "[replication][dirtymask][overflow]") {
    DirtyMask m(72);  // matches ReplTestBig
    REQUIRE(m.spilled());
    m.setAll();
    REQUIRE(m.count() == 72);
    for (size_t i = 0; i < 72; ++i) REQUIRE(m.test(i));
    REQUIRE_FALSE(m.test(72));
}

TEST_CASE("ReplicationRegistry: DirtyMask resize wipes state",
          "[replication][dirtymask]") {
    DirtyMask m(8);
    m.set(0);
    m.set(3);
    REQUIRE(m.count() == 2);

    m.resize(16);
    REQUIRE(m.size() == 16);
    REQUIRE(m.count() == 0);
    REQUIRE_FALSE(m.test(0));
    REQUIRE_FALSE(m.test(3));

    // Resize from packed-path to overflow-path also wipes.
    m.set(5);
    m.resize(200);
    REQUIRE(m.size() == 200);
    REQUIRE(m.spilled());
    REQUIRE(m.count() == 0);
}

// ── End-to-end: DirtyMask driven by SV_DIRTY ──────────────────────

TEST_CASE("ReplicationRegistry: DirtyMask + SV_DIRTY end-to-end",
          "[replication][macro][dirtymask]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    DirtyMask mask(meta->fieldCount());
    REQUIRE(mask.size() == 6);
    REQUIRE_FALSE(mask.spilled());

    // Simulate gameplay mutations: player takes damage + moves.
    mask.set(SV_DIRTY(ReplTestScalar, health));
    mask.set(SV_DIRTY(ReplTestScalar, x));
    mask.set(SV_DIRTY(ReplTestScalar, z));

    REQUIRE(mask.count() == 3);
    REQUIRE(mask.test(meta->fieldIndex("health")));
    REQUIRE(mask.test(meta->fieldIndex("x")));
    REQUIRE(mask.test(meta->fieldIndex("z")));
    REQUIRE_FALSE(mask.test(meta->fieldIndex("id")));
    REQUIRE_FALSE(mask.test(meta->fieldIndex("y")));
    REQUIRE_FALSE(mask.test(meta->fieldIndex("alive")));

    // Snapshot-generator-side: consume dirty bits, clear, next tick.
    mask.reset();
    REQUIRE(mask.none());
}

// ═══════════════════════════════════════════════════════════════════
// Authority + Snapshot encoding/decoding
// ═══════════════════════════════════════════════════════════════════

// ── Authority enum + setAuthority ─────────────────────────────────

TEST_CASE("ReplicationRegistry: Authority default on a replicated type is Server",
          "[replication][authority]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);
    // ReplTestScalar has no SV_COMPONENT_AUTHORITY, so the default
    // (Authority::Server) should apply after rebuild.
    REQUIRE(meta->authority == Authority::Server);
}

TEST_CASE("ReplicationRegistry: authorityToString covers every enum value",
          "[replication][authority]") {
    REQUIRE(std::string(authorityToString(Authority::Server)) == "Server");
    REQUIRE(std::string(authorityToString(Authority::Owner))  == "Owner");
    REQUIRE(std::string(authorityToString(Authority::Editor)) == "Editor");
    REQUIRE(std::string(authorityToString(Authority::None))   == "None");
}

TEST_CASE("ReplicationRegistry: SV_COMPONENT_AUTHORITY takes effect at program start",
          "[replication][authority]") {
    // This test does NOT call rebuildTestRegistry() — we're
    // checking the authority tag captured during static init,
    // after the SV_COMPONENT_AUTHORITY file-scope trigger ran at
    // program start. If the macro is broken, g_initialTaggedAuthority
    // will hold whatever the registry lookup returned at init time,
    // which is the default Server (or None on lookup miss).
    REQUIRE(g_initialTaggedAuthority == Authority::Owner);
}

TEST_CASE("ReplicationRegistry: setAuthority patches a registered type",
          "[replication][authority]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->authority == Authority::Server);

    REQUIRE(ReplicationRegistry::get().setAuthority("ReplTestScalar", Authority::Editor));
    // unordered_map stable-reference guarantee: the old pointer still
    // points at the now-patched entry.
    REQUIRE(meta->authority == Authority::Editor);

    REQUIRE(ReplicationRegistry::get().setAuthority("ReplTestScalar", Authority::None));
    REQUIRE(meta->authority == Authority::None);

    // Reset to default for hygiene (other tests assume Server).
    REQUIRE(ReplicationRegistry::get().setAuthority("ReplTestScalar", Authority::Server));
    REQUIRE(meta->authority == Authority::Server);
}

TEST_CASE("ReplicationRegistry: setAuthority returns false for unknown type",
          "[replication][authority]") {
    rebuildTestRegistry();
    REQUIRE_FALSE(ReplicationRegistry::get().setAuthority("NoSuchType", Authority::Owner));
    // Registry state unchanged.
    REQUIRE(ReplicationRegistry::get().find("NoSuchType") == nullptr);
}

TEST_CASE("ReplicationRegistry: resetForTests wipes authority along with metas",
          "[replication][authority]") {
    // Start from the program-start state, which has the tagged
    // authority applied via the macro trigger (even if a previous
    // test already called rebuildTestRegistry, the rebuild helper
    // also applies it).
    rebuildTestRegistry();
    const ReplicationMeta* before = ReplicationRegistry::get().find("ReplTestTagged");
    REQUIRE(before != nullptr);
    REQUIRE(before->authority == Authority::Owner);

    // Wipe without the rebuild helper re-applying anything.
    ReplicationRegistry::get().resetForTests();
    REQUIRE(ReplicationRegistry::get().find("ReplTestTagged") == nullptr);

    // Re-register through the macro-emitted helper only, without
    // calling setAuthority. The meta should come back with the
    // default Authority::Server, proving resetForTests wiped it and
    // that the macro-side trigger is the only source of the Owner tag.
    sv_buildReplicationMetaFor(static_cast<ReplTestTagged*>(nullptr));
    const ReplicationMeta* after = ReplicationRegistry::get().find("ReplTestTagged");
    REQUIRE(after != nullptr);
    REQUIRE(after->authority == Authority::Server);

    // Restore clean state for subsequent tests.
    rebuildTestRegistry();
}

// ── SnapshotWriter/Reader primitives ──────────────────────────────

TEST_CASE("ReplicationRegistry: SnapshotWriter/Reader raw primitives round-trip",
          "[replication][encode][decode]") {
    SnapshotWriter w;
    w.writeU8(0xA5u);
    w.writeU16(0xBEEFu);
    w.writeU32(0xDEADBEEFu);
    w.writeU64(0x0123456789ABCDEFull);
    w.writeBool(true);
    w.writeBool(false);
    w.writeFloatRaw(3.1415927f);
    w.writeDoubleRaw(2.7182818284590452);

    SnapshotReader r(w.buffer());

    uint8_t  u8;  REQUIRE(r.readU8(u8));   REQUIRE(u8  == 0xA5u);
    uint16_t u16; REQUIRE(r.readU16(u16)); REQUIRE(u16 == 0xBEEFu);
    uint32_t u32; REQUIRE(r.readU32(u32)); REQUIRE(u32 == 0xDEADBEEFu);
    uint64_t u64; REQUIRE(r.readU64(u64)); REQUIRE(u64 == 0x0123456789ABCDEFull);
    bool     bTrue  = false; REQUIRE(r.readBool(bTrue));  REQUIRE(bTrue  == true);
    bool     bFalse = true;  REQUIRE(r.readBool(bFalse)); REQUIRE(bFalse == false);
    float    f;  REQUIRE(r.readFloatRaw(f));
    REQUIRE_THAT(f, WithinAbs(3.1415927f, 0.0f));
    double   d;  REQUIRE(r.readDoubleRaw(d));
    REQUIRE_THAT(d, WithinAbs(2.7182818284590452, 0.0));
    REQUIRE(r.atEnd());
}

TEST_CASE("ReplicationRegistry: varint round-trip across byte-width boundaries",
          "[replication][encode][decode][varint]") {
    // Cover every LEB128 byte-width: 1 byte (<= 0x7F), 2 bytes
    // (<= 0x3FFF), 3 bytes (<= 0x1FFFFF), 4 bytes (<= 0x0FFFFFFF),
    // and 5 bytes (the 32-bit max).
    const uint32_t samples[] = {
        0u, 1u, 0x7Fu,
        0x80u, 0x1234u, 0x3FFFu,
        0x4000u, 0x1FFFFFu,
        0x200000u, 0x0FFFFFFFu,
        0x10000000u, 0xFFFFFFFFu
    };
    for (uint32_t v : samples) {
        SnapshotWriter w;
        w.writeVarintU32(v);
        SnapshotReader r(w.buffer());
        uint32_t out = 0;
        REQUIRE(r.readVarintU32(out));
        REQUIRE(out == v);
        REQUIRE(r.atEnd());
    }
}

TEST_CASE("ReplicationRegistry: varint u64 round-trip at max values",
          "[replication][encode][decode][varint]") {
    const uint64_t samples[] = {
        0ull, 0x7Full, 0x80ull,
        0xFFFFFFFFull,        // 32-bit max
        0x100000000ull,       // just over 32-bit
        0xFFFFFFFFFFFFFFFFull // 64-bit max
    };
    for (uint64_t v : samples) {
        SnapshotWriter w;
        w.writeVarintU64(v);
        SnapshotReader r(w.buffer());
        uint64_t out = 0;
        REQUIRE(r.readVarintU64(out));
        REQUIRE(out == v);
        REQUIRE(r.atEnd());
    }
}

TEST_CASE("ReplicationRegistry: zigzag round-trip for signed int32/int64",
          "[replication][encode][decode][zigzag]") {
    const int32_t samples32[] = {
        0, 1, -1, 2, -2,
        127, -128,
        32767, -32768,
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min()
    };
    for (int32_t v : samples32) {
        SnapshotWriter w;
        w.writeZigzagI32(v);
        SnapshotReader r(w.buffer());
        int32_t out = 0;
        REQUIRE(r.readZigzagI32(out));
        REQUIRE(out == v);
        REQUIRE(r.atEnd());
    }

    const int64_t samples64[] = {
        0, 1, -1, 42, -42,
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min()
    };
    for (int64_t v : samples64) {
        SnapshotWriter w;
        w.writeZigzagI64(v);
        SnapshotReader r(w.buffer());
        int64_t out = 0;
        REQUIRE(r.readZigzagI64(out));
        REQUIRE(out == v);
        REQUIRE(r.atEnd());
    }
}

TEST_CASE("ReplicationRegistry: float quantization round-trip within half-step tolerance",
          "[replication][encode][decode][quant]") {
    struct Case { float value; float step; };
    const Case cases[] = {
        {  0.0f,        0.001f },  // zero
        {  1.234f,      0.001f },  // 1 mm precision
        { -5.678f,      0.01f  },  // cm precision, negative
        { 17.0f,        1.0f   },  // integer HP
        { -100.5f,      0.5f   },  // half-unit
        {  1000000.0f,  0.001f }   // large magnitude, fine step
    };
    for (const Case& c : cases) {
        SnapshotWriter w;
        w.writeFloatQuantized(c.value, c.step);
        SnapshotReader r(w.buffer());
        float decoded = 0.0f;
        REQUIRE(r.readFloatQuantized(decoded, c.step));
        // Quantization guarantee is ±step/2 (round-to-nearest).
        REQUIRE_THAT(decoded, WithinAbs(c.value, c.step * 0.5f + 1e-4f));
        REQUIRE(r.atEnd());
    }
}

// ── encodeSnapshot / decodeSnapshot ───────────────────────────────

TEST_CASE("ReplicationRegistry: encodeSnapshot + decodeSnapshot full mask round-trip",
          "[replication][snapshot][roundtrip]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    ReplTestScalar src{};
    src.id     = -12345;
    src.x      =  1.5f;
    src.y      = -2.25f;
    src.z      =  1000.0f;
    src.health = 200;
    src.alive  = true;

    DirtyMask mask(meta->fieldCount());
    mask.setAll();
    REQUIRE(mask.count() == 6);

    SnapshotWriter w;
    REQUIRE(encodeSnapshot(*meta, &src, mask, w));
    REQUIRE(w.size() > 0);

    // Decode into a zero-initialized instance.
    ReplTestScalar dst{};
    DirtyMask      decodedMask;
    SnapshotReader r(w.buffer());
    REQUIRE(decodeSnapshot(*meta, &dst, decodedMask, r));
    REQUIRE(r.atEnd());

    REQUIRE(decodedMask.count() == 6);
    for (size_t i = 0; i < meta->fieldCount(); ++i) {
        REQUIRE(decodedMask.test(i));
    }

    REQUIRE(dst.id     == src.id);
    REQUIRE_THAT(dst.x, WithinAbs(src.x, 0.0f));
    REQUIRE_THAT(dst.y, WithinAbs(src.y, 0.0f));
    REQUIRE_THAT(dst.z, WithinAbs(src.z, 0.0f));
    REQUIRE(dst.health == src.health);
    REQUIRE(dst.alive  == src.alive);
}

TEST_CASE("ReplicationRegistry: encodeSnapshot partial mask leaves untouched fields at dst default",
          "[replication][snapshot][roundtrip]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    ReplTestScalar src{};
    src.id     = 999;
    src.x      = 3.14f;
    src.y      = 4.25f;
    src.z      = 5.5f;
    src.health = 77;
    src.alive  = false;

    // Mutate only health + x. Snapshot should carry those two
    // fields only; decode should leave all others at the dst's
    // pre-decode value.
    DirtyMask mask(meta->fieldCount());
    mask.set(SV_DIRTY(ReplTestScalar, health));
    mask.set(SV_DIRTY(ReplTestScalar, x));
    REQUIRE(mask.count() == 2);

    SnapshotWriter w;
    REQUIRE(encodeSnapshot(*meta, &src, mask, w));

    ReplTestScalar dst{};
    dst.id     = -1;
    dst.x      = 0.0f;
    dst.y      = 42.0f;  // should stay at 42.0f after decode
    dst.z      = 43.0f;  // should stay at 43.0f
    dst.health = 0;
    dst.alive  = true;   // should stay true

    DirtyMask decodedMask;
    SnapshotReader r(w.buffer());
    REQUIRE(decodeSnapshot(*meta, &dst, decodedMask, r));
    REQUIRE(r.atEnd());

    REQUIRE(decodedMask.count() == 2);
    REQUIRE(decodedMask.test(meta->fieldIndex("x")));
    REQUIRE(decodedMask.test(meta->fieldIndex("health")));
    REQUIRE_FALSE(decodedMask.test(meta->fieldIndex("id")));
    REQUIRE_FALSE(decodedMask.test(meta->fieldIndex("y")));
    REQUIRE_FALSE(decodedMask.test(meta->fieldIndex("z")));
    REQUIRE_FALSE(decodedMask.test(meta->fieldIndex("alive")));

    // Decoded fields match the source.
    REQUIRE(dst.health == 77);
    REQUIRE_THAT(dst.x, WithinAbs(3.14f, 0.0f));
    // Untouched fields keep their pre-decode values.
    REQUIRE(dst.id     == -1);
    REQUIRE_THAT(dst.y, WithinAbs(42.0f, 0.0f));
    REQUIRE_THAT(dst.z, WithinAbs(43.0f, 0.0f));
    REQUIRE(dst.alive  == true);
}

TEST_CASE("ReplicationRegistry: encodeSnapshot empty mask writes header only",
          "[replication][snapshot][roundtrip]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    ReplTestScalar src{};
    src.id = 1234;

    DirtyMask mask(meta->fieldCount());
    REQUIRE(mask.count() == 0);

    SnapshotWriter w;
    REQUIRE(encodeSnapshot(*meta, &src, mask, w));
    // 2 bytes schema version + 1 byte mask (6 fields → ceil(6/8)=1).
    REQUIRE(w.size() == 3);

    ReplTestScalar dst{};
    dst.id = -999;
    DirtyMask decodedMask;
    SnapshotReader r(w.buffer());
    REQUIRE(decodeSnapshot(*meta, &dst, decodedMask, r));
    REQUIRE(r.atEnd());
    REQUIRE(decodedMask.count() == 0);
    // dst.id stays at its pre-decode value because no field was sent.
    REQUIRE(dst.id == -999);
}

TEST_CASE("ReplicationRegistry: encodeSnapshot + decodeSnapshot quantized component",
          "[replication][snapshot][roundtrip][quant]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestQuant");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields[0].quantStep > 0.0f);  // position: 0.001f
    REQUIRE(meta->fields[1].quantStep > 0.0f);  // yaw:      0.01f

    // ReplTestQuant is declared at file scope near the top of this
    // file; reuse it directly.
    ReplTestQuant src{};
    src.position = 12.345f;
    src.yaw      = -1.570796f;

    DirtyMask mask(meta->fieldCount());
    mask.setAll();

    SnapshotWriter w;
    REQUIRE(encodeSnapshot(*meta, &src, mask, w));

    ReplTestQuant dst{};
    DirtyMask      decodedMask;
    SnapshotReader r(w.buffer());
    REQUIRE(decodeSnapshot(*meta, &dst, decodedMask, r));
    REQUIRE(r.atEnd());

    // Quantization tolerance is ±step/2 per-field. position has a
    // 1 mm step, yaw has a 10 mrad step.
    REQUIRE_THAT(dst.position, WithinAbs(src.position, 0.0005f + 1e-5f));
    REQUIRE_THAT(dst.yaw,      WithinAbs(src.yaw,      0.005f  + 1e-5f));
}

TEST_CASE("ReplicationRegistry: decodeSnapshot rejects mismatched schema version",
          "[replication][snapshot][error]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    // Build a valid snapshot first, then corrupt the schema-version
    // prefix to force a mismatch.
    ReplTestScalar src{};
    src.id = 42;
    DirtyMask mask(meta->fieldCount());
    mask.set(SV_DIRTY(ReplTestScalar, id));

    SnapshotWriter w;
    REQUIRE(encodeSnapshot(*meta, &src, mask, w));
    std::vector<uint8_t> bytes = w.buffer();
    REQUIRE(bytes.size() >= 2);
    bytes[0] ^= 0xFFu;  // corrupt low byte of schema version
    bytes[1] ^= 0xFFu;  // corrupt high byte

    ReplTestScalar dst{};
    dst.id = -1;
    DirtyMask decodedMask;
    SnapshotReader r(bytes.data(), bytes.size());
    REQUIRE_FALSE(decodeSnapshot(*meta, &dst, decodedMask, r));
    // dst.id must not have been touched — decode bails before
    // walking fields on schema mismatch.
    REQUIRE(dst.id == -1);
}

TEST_CASE("ReplicationRegistry: decodeSnapshot rejects truncated buffer mid-field",
          "[replication][snapshot][error]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    ReplTestScalar src{};
    src.id = 9999;
    src.x  = 1.25f;
    DirtyMask mask(meta->fieldCount());
    mask.setAll();

    SnapshotWriter w;
    REQUIRE(encodeSnapshot(*meta, &src, mask, w));
    std::vector<uint8_t> full = w.buffer();
    REQUIRE(full.size() > 4);

    // Truncate halfway through the field stream (keeping the
    // header + mask intact). Anywhere past byte 3 counts, since
    // `id` is a zigzag+varint that will need at least 1 byte.
    std::vector<uint8_t> truncated(full.begin(),
                                   full.begin() + static_cast<ptrdiff_t>(full.size() / 2));
    REQUIRE(truncated.size() >= 3);

    ReplTestScalar dst{};
    DirtyMask decodedMask;
    SnapshotReader r(truncated.data(), truncated.size());
    REQUIRE_FALSE(decodeSnapshot(*meta, &dst, decodedMask, r));
}

TEST_CASE("ReplicationRegistry: decodeSnapshot rejects truncated buffer in mask bytes",
          "[replication][snapshot][error]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    // Build a buffer with only the schema version and no mask bytes.
    SnapshotWriter w;
    w.writeU16(meta->schemaVersion);
    // Intentionally omit the mask bytes.

    ReplTestScalar dst{};
    DirtyMask decodedMask;
    SnapshotReader r(w.buffer());
    REQUIRE_FALSE(decodeSnapshot(*meta, &dst, decodedMask, r));
}

TEST_CASE("ReplicationRegistry: encodeSnapshot rejects DirtyMask size mismatch",
          "[replication][snapshot][error]") {
    rebuildTestRegistry();
    const ReplicationMeta* meta = ReplicationRegistry::get().find("ReplTestScalar");
    REQUIRE(meta != nullptr);

    ReplTestScalar src{};
    DirtyMask      wrongSize(3);  // meta has 6 fields
    SnapshotWriter w;
    REQUIRE_FALSE(encodeSnapshot(*meta, &src, wrongSize, w));
    // Nothing should have been written to the buffer.
    REQUIRE(w.size() == 0);
}

TEST_CASE("ReplicationRegistry: SnapshotReader.readVarintU32 rejects overlong encoding",
          "[replication][decode][varint][error]") {
    // Six continuation bytes with no terminator — a legitimate u32
    // varint is at most 5 bytes. The reader should bail without
    // advancing the cursor past the start.
    const uint8_t overlong[] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
    SnapshotReader r(overlong, sizeof(overlong));
    uint32_t v = 0xAAAAAAAAu;
    REQUIRE_FALSE(r.readVarintU32(v));
    // On failure the reader restores the cursor so callers can
    // try a different decode path. v is left untouched (its
    // pre-call sentinel is preserved).
    REQUIRE(r.cursor() == 0);
    REQUIRE(v == 0xAAAAAAAAu);
}
