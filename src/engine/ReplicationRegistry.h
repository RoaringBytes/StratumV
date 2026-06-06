// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── ReplicationRegistry ────────────────────────────
// Layer 4 module — pure-logic reflection registry + snapshot encoder
// for replicated game components. Games declare which fields of which
// components flow across the network via SV_REPLICATE, tag the
// component with an Authority via SV_COMPONENT_AUTHORITY, and the
// registry collects the descriptors at static init. The snapshot
// encoder/decoder walks a DirtyMask and writes/reads only the dirty
// fields into a byte buffer.
//
// Reflection scope:
//   - Field metadata (offset, type tag, name hash, dirty bit index,
//     quantization hint)
//   - Static registration via SV_REPLICATE macro
//   - Dirty bit helpers (per-instance mask + bit ops)
//   - Schema version hash derived from field name+type sequence
//
// Snapshot/authority scope:
//   - Authority enum (Server/Owner/Editor/None)
//   - SV_COMPONENT_AUTHORITY macro + per-type storage on ReplicationMeta
//   - SnapshotWriter / SnapshotReader byte-buffer primitives with
//     LEB128-style varint, zigzag for signed, raw float, and quantized
//     float using the quantStep hint
//   - encodeSnapshot / decodeSnapshot: scalar+quant round-trip that
//     stamps schemaVersion + dirty mask + dirty field values
//
// Out of scope (future work):
//   - MsQuic transport
//   - BaseSystemContext slot (network fields bundled into a
//     nested sub-struct at the semver 1.3.0 bump)
//   - Vec2/3/4/Quat/EntityHandle/StringFixed/Enum/Blob encoders
//     (added as games specialize fieldTypeFor<T> and extend the
//     switch — this module locks down the scalar path + quantization math
//     so later wire additions don't destabilize existing round-trips)
//
// Dependencies: <cstddef>, <cstdint>, <initializer_list>, <string>,
//               <string_view>, <unordered_map>, <vector>.
// No BSC, no Vulkan, no ECS, no glm. Safe for unit tests.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sv {

// ── FNV-1a 32-bit hash ────────────────────────────────────────────
// Chosen for constexpr-friendliness, stability across runs, and zero
// dependencies. Used for both field name hashes and schema version
// hashes. Not cryptographic — collision risk is negligible for the
// small set of field names any one component carries.
constexpr uint32_t fnv1a32(std::string_view s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

// ── Field type tag ────────────────────────────────────────────────
// The wire layer will switch on this tag to dispatch to the right
// encoder. Initially the tag is only used for metadata + schema
// version; actual encoding is handled by the snapshot encoder.
enum class FieldType : uint8_t {
    Unknown      = 0,
    Bool         = 1,
    Int8         = 2,
    Int16        = 3,
    Int32        = 4,
    Int64        = 5,
    UInt8        = 6,
    UInt16       = 7,
    UInt32       = 8,
    UInt64       = 9,
    Float        = 10,
    Double       = 11,
    Vec2         = 12,
    Vec3         = 13,
    Vec4         = 14,
    Quat         = 15,
    EntityHandle = 16,
    StringFixed  = 17,
    Enum         = 18,
    Blob         = 19,  // opaque POD fallback
};

const char* fieldTypeToString(FieldType t);

// ── Type tag dispatch ─────────────────────────────────────────────
// Primary template returns Unknown. Specializations map built-in
// scalar C++ types to the enum. Games and engine subsystems can add
// their own specializations (e.g. glm::vec3 → FieldType::Vec3) in
// the same TU as the component definition — the registry header has
// no glm/math dependency on purpose.
template <typename T>
constexpr FieldType fieldTypeFor() { return FieldType::Unknown; }

template <> constexpr FieldType fieldTypeFor<bool>()     { return FieldType::Bool;   }
template <> constexpr FieldType fieldTypeFor<int8_t>()   { return FieldType::Int8;   }
template <> constexpr FieldType fieldTypeFor<int16_t>()  { return FieldType::Int16;  }
template <> constexpr FieldType fieldTypeFor<int32_t>()  { return FieldType::Int32;  }
template <> constexpr FieldType fieldTypeFor<int64_t>()  { return FieldType::Int64;  }
template <> constexpr FieldType fieldTypeFor<uint8_t>()  { return FieldType::UInt8;  }
template <> constexpr FieldType fieldTypeFor<uint16_t>() { return FieldType::UInt16; }
template <> constexpr FieldType fieldTypeFor<uint32_t>() { return FieldType::UInt32; }
template <> constexpr FieldType fieldTypeFor<uint64_t>() { return FieldType::UInt64; }
template <> constexpr FieldType fieldTypeFor<float>()    { return FieldType::Float;  }
template <> constexpr FieldType fieldTypeFor<double>()   { return FieldType::Double; }

// ── Authority ─────────────────────────────────────────────────────
// Who is allowed to mutate a replicated component.
// See REPLICATION_CONTRACT.md §2 for the full semantic definition.
// Stored per-component on ReplicationMeta; games tag components via
// SV_COMPONENT_AUTHORITY(Type, Authority::X).
enum class Authority : uint8_t {
    Server = 0,   // authoritative server only (default)
    Owner  = 1,   // owning client (predicted; validated by server)
    Editor = 2,   // any client holding the Editor permission scope
    None   = 3    // explicitly not replicated
};

const char* authorityToString(Authority a);

// ── Per-field descriptor ──────────────────────────────────────────
// Filled in by the SV_FIELD macro at registration time. `dirtyBit`
// and `nameHash` are populated inside svRegisterReplicatedType, so
// the macro-side initializer leaves them zero.
struct FieldDesc {
    std::string name;             // raw field name as written in C++
    uint32_t    nameHash  = 0;    // fnv1a32(name)
    size_t      offset    = 0;    // offsetof(Component, field)
    size_t      size      = 0;    // sizeof(Component::field)
    FieldType   type      = FieldType::Unknown;
    uint8_t     dirtyBit  = 0;    // index into per-instance DirtyMask
    float       quantStep = 0.0f; // 0 = raw; else wire granularity
};

// ── Per-component descriptor ──────────────────────────────────────
struct ReplicationMeta {
    std::string            typeName;          // C++ type name (unqualified)
    uint32_t               typeNameHash  = 0; // fnv1a32(typeName)
    size_t                 typeSize      = 0; // sizeof(Component)
    uint16_t               schemaVersion = 0; // hash over field name+type sequence
    Authority              authority     = Authority::Server;  // default is server-authoritative
    std::vector<FieldDesc> fields;

    // Linear scans — components are expected to have a small number
    // of fields (<32 in practice), so a vector walk is faster and
    // simpler than a hash table. findField returns nullptr on miss;
    // fieldIndex returns -1 on miss.
    const FieldDesc* findField(std::string_view name) const;
    const FieldDesc* findFieldByHash(uint32_t nameHash) const;
    int              fieldIndex(std::string_view name) const;
    size_t           fieldCount() const { return fields.size(); }
};

// ── Registry singleton ────────────────────────────────────────────
// Thread-safety: registration happens at static init (one thread) or
// from tests (also single-threaded). No locking. The runtime should
// treat the registry as read-only after start-up.
//
// Storage uses std::unordered_map so that references to stored meta
// values remain valid across subsequent insertions — the cppreference
// contract for unordered_map guarantees references are invalidated
// only by erase/clear.
class ReplicationRegistry {
public:
    static ReplicationRegistry& get();

    // Register (or re-register) a component type. Returns a stable
    // reference to the stored meta. If a type with the same typeName
    // is already registered, the existing entry is REPLACED and a
    // diagnostic is logged — duplicate registration means either a
    // renamed field or a test re-registering after a reset.
    const ReplicationMeta& registerType(ReplicationMeta meta);

    // Lookup. Both return nullptr on miss.
    const ReplicationMeta* find(std::string_view typeName) const;
    const ReplicationMeta* findByHash(uint32_t typeNameHash) const;

    // Patch a registered type's Authority tag in place.
    // Returns true on success, false if typeName is unknown (in which
    // case a warning is logged; the caller used SV_COMPONENT_AUTHORITY
    // on a type that never went through SV_REPLICATE). Convention is
    // that SV_COMPONENT_AUTHORITY appears right after SV_REPLICATE in
    // the same TU so the target meta is already present.
    bool setAuthority(std::string_view typeName, Authority auth);

    // Snapshot of all currently-registered metas (pointer copy into
    // a vector). Iteration order is unordered.
    std::vector<const ReplicationMeta*> all() const;

    // Per-type schema summary used by the connect-time
    // handshake preamble. Returns `(typeNameHash, schemaVersion)` pairs
    // for every registered type, sorted by typeNameHash so server and
    // client produce byte-identical orderings on matched registries.
    // The caller wraps these into a SchemaHandshake via
    // ReplicationProtocol::encodeSchemaHandshake.
    struct SchemaTableEntry {
        uint32_t typeNameHash  = 0;
        uint16_t schemaVersion = 0;
    };
    std::vector<SchemaTableEntry> getSchemaTable() const;

    size_t size()  const { return m_metas.size(); }
    bool   empty() const { return m_metas.empty(); }

    // Wipe all entries. Intended for unit tests only — the runtime
    // should never call this after start-up. After reset, any
    // previously-returned ReplicationMeta references are invalidated
    // (tests must re-invoke the SV_REPLICATE registration helper to
    // repopulate).
    void resetForTests();

private:
    ReplicationRegistry() = default;
    ReplicationRegistry(const ReplicationRegistry&)            = delete;
    ReplicationRegistry& operator=(const ReplicationRegistry&) = delete;

    std::unordered_map<std::string, ReplicationMeta> m_metas;
    std::unordered_map<uint32_t, std::string>        m_byHash;
};

// ── Macro registration helper ─────────────────────────────────────
// Instantiated once per game component type by the SV_REPLICATE
// macro. Builds a ReplicationMeta from the field initializer list,
// assigns dirty-bit indices in declaration order, computes field
// name hashes + schema version, and pushes the result into the
// registry. Returns a stable reference to the registered meta for
// the caller to cache.
//
// The helper is an inline template so multiple TUs including the
// same SV_REPLICATE declaration do not multiply-define it; the body
// runs on every call, but the registry dedupes by typeName.
template <typename T>
inline const ReplicationMeta& svRegisterReplicatedType(
        const char* typeName,
        std::initializer_list<FieldDesc> rawFields) {
    ReplicationMeta meta;
    meta.typeName     = typeName;
    meta.typeNameHash = fnv1a32(typeName);
    meta.typeSize     = sizeof(T);
    meta.fields.reserve(rawFields.size());

    // Schema version: FNV-1a over "name|type" in declaration order.
    // Captures rename (name changes) and type change (type tag
    // changes) but not field reorder with preserved name+type, which
    // matches how wire encoding iterates the field vector.
    uint32_t schemaHash = 2166136261u;
    uint8_t  bit        = 0;
    for (const FieldDesc& f : rawFields) {
        FieldDesc desc = f;
        desc.dirtyBit  = bit++;
        desc.nameHash  = fnv1a32(desc.name);
        for (char c : desc.name) {
            schemaHash ^= static_cast<uint8_t>(c);
            schemaHash *= 16777619u;
        }
        schemaHash ^= static_cast<uint8_t>(desc.type);
        schemaHash *= 16777619u;
        meta.fields.push_back(std::move(desc));
    }
    // Fold the 32-bit hash into a 16-bit schema version. Collisions
    // are acceptable here because the full 32-bit typeNameHash + the
    // typeName string both flow on the wire; schemaVersion is a fast
    // mismatch signal, not a collision-resistant ID.
    meta.schemaVersion = static_cast<uint16_t>(
        (schemaHash ^ (schemaHash >> 16)) & 0xFFFFu);

    return ReplicationRegistry::get().registerType(std::move(meta));
}

// ── DirtyMask ─────────────────────────────────────────────────────
// Per-instance bitmask of which replicated fields are dirty since
// the last snapshot. Components with ≤64 fields use the fast inline
// uint64 path (m_packed); larger components spill into a heap-
// allocated overflow bitset.
//
// The registry describes types; DirtyMask describes instances. ECS
// systems own the masks (typically one per replicated component per
// entity) and call DirtyMask::set(SV_DIRTY(Type, field)) at every
// mutation site. The (future) snapshot generator walks entities with
// non-empty masks and encodes only the bits that are set.
class DirtyMask {
public:
    DirtyMask() = default;
    explicit DirtyMask(size_t fieldCount);

    void resize(size_t fieldCount);
    void reset();    // clear all bits; size unchanged
    void setAll();   // set bits [0..size())

    void set(size_t bit);
    void clear(size_t bit);
    bool test(size_t bit) const;

    bool   any()   const;
    bool   none()  const { return !any(); }
    size_t count() const;
    size_t size()  const { return m_fieldCount; }

    // Fast-path accessors for the ≤64 case. packed() returns 0 if
    // the mask has spilled into the overflow bitset (in that case
    // callers must walk the overflow explicitly via test()).
    uint64_t packed()  const { return m_packed; }
    bool     spilled() const { return !m_overflow.empty(); }

private:
    size_t                m_fieldCount = 0;
    uint64_t              m_packed     = 0;
    std::vector<uint64_t> m_overflow;  // only populated when fieldCount > 64
};

// ── Snapshot primitives ──────────────────────────────────
// Byte-buffer based encoder/decoder pair. Wire format is little-
// endian for raw integer/float writes; varints are LEB128-style
// (7 bits payload + 1 continuation bit per byte); signed integers
// use zigzag mapping (n << 1) ^ (n >> (bits - 1)) before varint.
//
// The writer owns a std::vector<uint8_t> that grows as bytes are
// appended. The reader borrows a span (data + size) provided by the
// caller and tracks a cursor; all read methods return false on
// underflow without mutating the out parameter. This lets higher-
// level decoders short-circuit cleanly on malformed buffers.
//
// None of these types allocate on the hot path beyond the writer's
// backing vector; tests verify that a typical 6-field scalar
// component round-trips with zero reader allocations.

class SnapshotWriter {
public:
    SnapshotWriter() = default;
    explicit SnapshotWriter(size_t reserveBytes) { m_buffer.reserve(reserveBytes); }

    const std::vector<uint8_t>& buffer() const { return m_buffer; }
    std::vector<uint8_t>&&      takeBuffer()    { return std::move(m_buffer); }
    size_t                      size()   const { return m_buffer.size(); }
    void                        clear()        { m_buffer.clear(); }

    // Fixed-width raw writes (little-endian).
    void writeU8(uint8_t v);
    void writeU16(uint16_t v);
    void writeU32(uint32_t v);
    void writeU64(uint64_t v);
    void writeBool(bool v)   { writeU8(v ? 1u : 0u); }
    void writeFloatRaw(float v);
    void writeDoubleRaw(double v);

    // Varint / zigzag writes.
    void writeVarintU32(uint32_t v);
    void writeVarintU64(uint64_t v);
    void writeZigzagI32(int32_t v);
    void writeZigzagI64(int64_t v);

    // Quantized float: scales by 1/step, rounds to nearest int,
    // writes zigzag+varint. Callers must ensure step > 0 — step == 0
    // means "no quantization" and should be routed to writeFloatRaw.
    void writeFloatQuantized(float v, float step);

    // Raw blob (used for the dirty mask bytes + unknown-typed fields).
    void writeBytes(const void* data, size_t bytes);

private:
    std::vector<uint8_t> m_buffer;
};

class SnapshotReader {
public:
    SnapshotReader(const uint8_t* data, size_t size)
        : m_data(data), m_size(size) {}
    explicit SnapshotReader(const std::vector<uint8_t>& buf)
        : m_data(buf.data()), m_size(buf.size()) {}

    size_t cursor()    const { return m_cursor; }
    size_t remaining() const { return m_size - m_cursor; }
    bool   atEnd()     const { return m_cursor >= m_size; }

    // Each reader returns false on underflow and leaves the cursor
    // unchanged + the out parameter untouched.
    bool readU8(uint8_t& out);
    bool readU16(uint16_t& out);
    bool readU32(uint32_t& out);
    bool readU64(uint64_t& out);
    bool readBool(bool& out);
    bool readFloatRaw(float& out);
    bool readDoubleRaw(double& out);

    bool readVarintU32(uint32_t& out);
    bool readVarintU64(uint64_t& out);
    bool readZigzagI32(int32_t& out);
    bool readZigzagI64(int64_t& out);

    bool readFloatQuantized(float& out, float step);

    bool readBytes(void* dst, size_t bytes);

private:
    const uint8_t* m_data   = nullptr;
    size_t         m_size   = 0;
    size_t         m_cursor = 0;
};

// ── Snapshot encoder/decoder ────────────────────────────
// encodeSnapshot walks a ReplicationMeta's fields via the provided
// DirtyMask and writes only the dirty fields into `out`. The wire
// layout is:
//
//   [u16       : schemaVersion]
//   [byteMask  : ceil(fieldCount/8) bytes, LSB-first within each byte]
//   [for each dirty field in declaration order:]
//     [field encoding dispatched on FieldDesc::type]
//
// decodeSnapshot reads the inverse, placing the decoded values at the
// matching offsets inside `instance` (which must be a pointer to a
// live object of the component type the meta describes). Returns
// false if the schema version does not match, the byte mask overruns
// the buffer, a field decoder hits EOF, or a field has an unknown
// type tag. On failure the caller should assume `instance` is in a
// partially-updated state (previously-decoded fields WILL have been
// written) — the convention is to decode into a scratch copy and
// swap on success, not in-place.
//
// The dirty mask on the wire is byte-packed (LSB-first); the passed-
// in sv::DirtyMask is u64-packed + overflow vector. encodeSnapshot
// reads from the DirtyMask; decodeSnapshot populates `outMask` so the
// caller knows which fields were touched.

bool encodeSnapshot(const ReplicationMeta& meta,
                    const void*            instance,
                    const DirtyMask&       mask,
                    SnapshotWriter&        out);

bool decodeSnapshot(const ReplicationMeta& meta,
                    void*                  instance,
                    DirtyMask&             outMask,
                    SnapshotReader&        in);

// ── Macro surface ─────────────────────────────────────────────────

// __LINE__-based unique-name helpers for the file-scope static
// registration trigger. Avoids colliding across multiple SV_REPLICATE
// uses in the same TU.
#define SV_REPL_CONCAT_IMPL(a, b) a##b
#define SV_REPL_CONCAT(a, b)      SV_REPL_CONCAT_IMPL(a, b)

// SV_FIELD(name): declare a replicated field of the type whose
// SV_REPLICATE block contains this macro. Uses `SV_ReplicationType`
// which is a local `using` alias set up by SV_REPLICATE, so callers
// only repeat the type name once (at the SV_REPLICATE site).
#define SV_FIELD(name) \
    ::sv::FieldDesc { \
        /*name=*/      #name, \
        /*nameHash=*/  0, \
        /*offset=*/    offsetof(SV_ReplicationType, name), \
        /*size=*/      sizeof(((SV_ReplicationType*)0)->name), \
        /*type=*/      ::sv::fieldTypeFor<decltype(SV_ReplicationType::name)>(), \
        /*dirtyBit=*/  0, \
        /*quantStep=*/ 0.0f \
    }

// SV_FIELD_QUANT(name, step): variant with a quantization hint. Step
// is in the units of the underlying field (e.g. 0.001f for 1 mm
// precision on a float position). The hint is recorded in
// metadata only; the encoder uses it for wire encoding.
#define SV_FIELD_QUANT(name, step) \
    ::sv::FieldDesc { \
        #name, 0, \
        offsetof(SV_ReplicationType, name), \
        sizeof(((SV_ReplicationType*)0)->name), \
        ::sv::fieldTypeFor<decltype(SV_ReplicationType::name)>(), \
        0, \
        static_cast<float>(step) \
    }

// SV_REPLICATE(Type, ...): register a component type for
// replication. Must be placed at namespace scope (not inside a
// struct body). The Type argument must be unqualified or accessible
// via a using-alias — token pasting cannot handle identifiers
// containing "::".
//
// Emits:
//   1. A non-inline free function sv_buildReplicationMetaFor(Type*)
//      that rebuilds and re-registers the metadata on every call.
//      Test code can invoke this after
//      ReplicationRegistry::resetForTests() to repopulate the
//      registry in a deterministic order.
//      Deliberately NOT 'inline': the definition must be emitted as
//      a strong symbol into stratumv_core.a so the headless core
//      carve-out's tests can link against it under GNU ld. MSVC keeps
//      an out-of-line copy of an unused inline function in the
//      archive; ld does not, so an inline helper here links on
//      Windows but fails the Linux core build with an undefined
//      reference. The trade-off: each SV_REPLICATE(T) must live in
//      exactly one .cpp (never a shared header) or the strong symbol
//      collides at link time.
//   2. A file-scope static bool trigger that invokes the function
//      once at program start so the metadata is present by the time
//      the runtime spins up.
#define SV_REPLICATE(Type, ...) \
    const ::sv::ReplicationMeta& sv_buildReplicationMetaFor(Type*) { \
        using SV_ReplicationType = Type; \
        return ::sv::svRegisterReplicatedType<Type>( \
            #Type, { __VA_ARGS__ }); \
    } \
    static const bool SV_REPL_CONCAT(sv_repl_register_, __LINE__) \
        [[maybe_unused]] = \
            (sv_buildReplicationMetaFor((Type*)nullptr), true)

// SV_DIRTY(Type, field): return the dirty-bit index for a named
// field. Intended for use with DirtyMask::set() at mutation sites:
//
//   sv::DirtyMask mask(replMeta.fieldCount());
//   player.position.x += dt;
//   mask.set(SV_DIRTY(PlayerTransform, position));
//
// Returns 0xFF if the field is not found (game bug — the component
// was mutated via a field that isn't declared in SV_REPLICATE).
#define SV_DIRTY(Type, field) \
    ::sv::dirtyBitFor<Type>(#field)

// SV_COMPONENT_AUTHORITY(Type, auth): tag a replicated component
// with a per-component Authority value. Must be placed at
// namespace scope AFTER the matching SV_REPLICATE in the same TU, so
// the ADL helper function sv_buildReplicationMetaFor(Type*) is
// already declared. Expands to a file-scope static initializer that:
//   1. Calls sv_buildReplicationMetaFor((Type*)nullptr) — idempotent
//      re-registration of the field metadata, guaranteeing the
//      target meta is present in the registry before (2) runs.
//   2. Calls ReplicationRegistry::get().setAuthority(#Type, auth) to
//      stamp the per-type tag.
// If no SV_COMPONENT_AUTHORITY is emitted for a component, the meta
// keeps its default (Authority::Server). See REPLICATION_CONTRACT.md
// §2 for the semantic definition of each Authority value.
#define SV_COMPONENT_AUTHORITY(Type, auth) \
    static const bool SV_REPL_CONCAT(sv_repl_auth_, __LINE__) \
        [[maybe_unused]] = \
            (sv_buildReplicationMetaFor((Type*)nullptr), \
             ::sv::ReplicationRegistry::get().setAuthority(#Type, (auth)), \
             true)

template <typename T>
inline uint8_t dirtyBitFor(std::string_view fieldName) {
    // ADL-finds the overload emitted by SV_REPLICATE(T, ...).
    const ReplicationMeta& meta = sv_buildReplicationMetaFor(static_cast<T*>(nullptr));
    const FieldDesc* f = meta.findField(fieldName);
    return f ? f->dirtyBit : static_cast<uint8_t>(0xFF);
}

} // namespace sv
