// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── ReplicationRegistry implementation ─────────────
// See ReplicationRegistry.h for the design rationale. The snapshot
// layer adds Authority enum + setAuthority + SnapshotWriter/SnapshotReader +
// encodeSnapshot/decodeSnapshot on top of the reflection surface.

#include "ReplicationRegistry.h"

#include "EngineLog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace sv {

// ── FieldType diagnostics ─────────────────────────────────────────

const char* fieldTypeToString(FieldType t) {
    switch (t) {
        case FieldType::Unknown:      return "Unknown";
        case FieldType::Bool:         return "Bool";
        case FieldType::Int8:         return "Int8";
        case FieldType::Int16:        return "Int16";
        case FieldType::Int32:        return "Int32";
        case FieldType::Int64:        return "Int64";
        case FieldType::UInt8:        return "UInt8";
        case FieldType::UInt16:       return "UInt16";
        case FieldType::UInt32:       return "UInt32";
        case FieldType::UInt64:       return "UInt64";
        case FieldType::Float:        return "Float";
        case FieldType::Double:       return "Double";
        case FieldType::Vec2:         return "Vec2";
        case FieldType::Vec3:         return "Vec3";
        case FieldType::Vec4:         return "Vec4";
        case FieldType::Quat:         return "Quat";
        case FieldType::EntityHandle: return "EntityHandle";
        case FieldType::StringFixed:  return "StringFixed";
        case FieldType::Enum:         return "Enum";
        case FieldType::Blob:         return "Blob";
    }
    return "Unknown";
}

// ── Authority diagnostics ────────────────────────────────

const char* authorityToString(Authority a) {
    switch (a) {
        case Authority::Server: return "Server";
        case Authority::Owner:  return "Owner";
        case Authority::Editor: return "Editor";
        case Authority::None:   return "None";
    }
    return "Unknown";
}

// ── ReplicationMeta lookups ───────────────────────────────────────

const FieldDesc* ReplicationMeta::findField(std::string_view name) const {
    for (const FieldDesc& f : fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

const FieldDesc* ReplicationMeta::findFieldByHash(uint32_t nameHash) const {
    for (const FieldDesc& f : fields) {
        if (f.nameHash == nameHash) return &f;
    }
    return nullptr;
}

int ReplicationMeta::fieldIndex(std::string_view name) const {
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

// ── ReplicationRegistry singleton ─────────────────────────────────

ReplicationRegistry& ReplicationRegistry::get() {
    static ReplicationRegistry s;
    return s;
}

const ReplicationMeta& ReplicationRegistry::registerType(ReplicationMeta meta) {
    const std::string  typeName      = meta.typeName;   // copy for post-move use
    const uint32_t     typeNameHash  = meta.typeNameHash;
    const size_t       fieldCount    = meta.fields.size();
    const uint16_t     schemaVersion = meta.schemaVersion;

    // Replace-or-insert. Re-registration with the SAME schema version
    // is idempotent and silent — that happens on every program start
    // because SV_COMPONENT_AUTHORITY's expansion calls
    // sv_buildReplicationMetaFor right after SV_REPLICATE's static
    // trigger has already registered the type. A schema drift (new
    // field, renamed field, type change) is warned because outside
    // of a hot-reload that's always a source-of-truth mismatch the
    // developer should see.
    auto existing = m_metas.find(typeName);
    if (existing != m_metas.end()) {
        if (existing->second.schemaVersion != schemaVersion) {
            SV_LOG_WARN("ReplicationRegistry",
                "schema drift for component type '%s' — was %zu fields (0x%04x), now %zu fields (0x%04x)",
                typeName.c_str(),
                existing->second.fields.size(),
                static_cast<unsigned>(existing->second.schemaVersion),
                fieldCount,
                static_cast<unsigned>(schemaVersion));
        }
        existing->second = std::move(meta);
        m_byHash[typeNameHash] = typeName;
        return existing->second;
    }

    auto [it, inserted] = m_metas.emplace(typeName, std::move(meta));
    m_byHash[typeNameHash] = typeName;
    return it->second;
}

const ReplicationMeta* ReplicationRegistry::find(std::string_view typeName) const {
    // unordered_map::find requires a key comparable via the hasher.
    // std::string_view does not implicitly construct a std::string
    // for lookup in C++20 unordered_map without heterogeneous-lookup
    // opt-in. Build a temporary key — cheap for the handful of
    // distinct component types any session has.
    auto it = m_metas.find(std::string(typeName));
    return (it == m_metas.end()) ? nullptr : &it->second;
}

const ReplicationMeta* ReplicationRegistry::findByHash(uint32_t typeNameHash) const {
    auto hashIt = m_byHash.find(typeNameHash);
    if (hashIt == m_byHash.end()) return nullptr;
    auto metaIt = m_metas.find(hashIt->second);
    return (metaIt == m_metas.end()) ? nullptr : &metaIt->second;
}

bool ReplicationRegistry::setAuthority(std::string_view typeName, Authority auth) {
    auto it = m_metas.find(std::string(typeName));
    if (it == m_metas.end()) {
        SV_LOG_WARN("ReplicationRegistry",
            "setAuthority: type '%.*s' is not registered "
            "(SV_COMPONENT_AUTHORITY must appear after SV_REPLICATE in the same TU)",
            static_cast<int>(typeName.size()), typeName.data());
        return false;
    }
    it->second.authority = auth;
    return true;
}

std::vector<const ReplicationMeta*> ReplicationRegistry::all() const {
    std::vector<const ReplicationMeta*> out;
    out.reserve(m_metas.size());
    for (const auto& kv : m_metas) out.push_back(&kv.second);
    return out;
}

std::vector<ReplicationRegistry::SchemaTableEntry>
ReplicationRegistry::getSchemaTable() const {
    std::vector<SchemaTableEntry> out;
    out.reserve(m_metas.size());
    for (const auto& kv : m_metas) {
        out.push_back({kv.second.typeNameHash, kv.second.schemaVersion});
    }
    // Sort by typeNameHash so the byte layout of the serialised table
    // is stable across runs and identical on server + client when the
    // registries match. A matching registry produces the same sequence
    // of (hash, version) pairs, which lets the client walk both
    // tables in parallel during compareSchemaHandshake.
    std::sort(out.begin(), out.end(),
              [](const SchemaTableEntry& a, const SchemaTableEntry& b) {
                  return a.typeNameHash < b.typeNameHash;
              });
    return out;
}

void ReplicationRegistry::resetForTests() {
    m_metas.clear();
    m_byHash.clear();
}

// ── DirtyMask ─────────────────────────────────────────────────────

DirtyMask::DirtyMask(size_t fieldCount) {
    resize(fieldCount);
}

void DirtyMask::resize(size_t fieldCount) {
    m_fieldCount = fieldCount;
    m_packed     = 0;
    m_overflow.clear();
    if (fieldCount > 64) {
        // One uint64 per 64 bits above the packed fast path. The
        // packed uint64 itself covers bits [0..64); the overflow
        // vector covers bits [64..fieldCount).
        const size_t overflowBits  = fieldCount - 64;
        const size_t overflowWords = (overflowBits + 63) / 64;
        m_overflow.assign(overflowWords, 0u);
    }
}

void DirtyMask::reset() {
    m_packed = 0;
    std::fill(m_overflow.begin(), m_overflow.end(), 0u);
}

void DirtyMask::setAll() {
    if (m_fieldCount == 0) {
        m_packed = 0;
        return;
    }
    if (m_fieldCount >= 64) {
        m_packed = ~uint64_t{0};
    } else {
        m_packed = (uint64_t{1} << m_fieldCount) - 1u;
    }
    if (!m_overflow.empty()) {
        // Fill the overflow words with all-ones, then mask off the
        // tail of the last word so unused bits stay zero.
        std::fill(m_overflow.begin(), m_overflow.end(), ~uint64_t{0});
        const size_t tailBits = (m_fieldCount - 64) % 64;
        if (tailBits != 0) {
            m_overflow.back() = (uint64_t{1} << tailBits) - 1u;
        }
    }
}

void DirtyMask::set(size_t bit) {
    if (bit >= m_fieldCount) return;  // out-of-range is a no-op
    if (bit < 64) {
        m_packed |= (uint64_t{1} << bit);
    } else {
        const size_t idx = (bit - 64) / 64;
        const size_t off = (bit - 64) % 64;
        m_overflow[idx] |= (uint64_t{1} << off);
    }
}

void DirtyMask::clear(size_t bit) {
    if (bit >= m_fieldCount) return;
    if (bit < 64) {
        m_packed &= ~(uint64_t{1} << bit);
    } else {
        const size_t idx = (bit - 64) / 64;
        const size_t off = (bit - 64) % 64;
        m_overflow[idx] &= ~(uint64_t{1} << off);
    }
}

bool DirtyMask::test(size_t bit) const {
    if (bit >= m_fieldCount) return false;
    if (bit < 64) {
        return (m_packed & (uint64_t{1} << bit)) != 0;
    }
    const size_t idx = (bit - 64) / 64;
    const size_t off = (bit - 64) % 64;
    return (m_overflow[idx] & (uint64_t{1} << off)) != 0;
}

bool DirtyMask::any() const {
    if (m_packed != 0) return true;
    for (uint64_t w : m_overflow) {
        if (w != 0) return true;
    }
    return false;
}

size_t DirtyMask::count() const {
    size_t n = static_cast<size_t>(std::popcount(m_packed));
    for (uint64_t w : m_overflow) {
        n += static_cast<size_t>(std::popcount(w));
    }
    return n;
}

// ── SnapshotWriter ───────────────────────────────────────

void SnapshotWriter::writeU8(uint8_t v) {
    m_buffer.push_back(v);
}

void SnapshotWriter::writeU16(uint16_t v) {
    m_buffer.push_back(static_cast<uint8_t>(v & 0xFFu));
    m_buffer.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void SnapshotWriter::writeU32(uint32_t v) {
    m_buffer.push_back(static_cast<uint8_t>(v & 0xFFu));
    m_buffer.push_back(static_cast<uint8_t>((v >> 8)  & 0xFFu));
    m_buffer.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    m_buffer.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void SnapshotWriter::writeU64(uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        m_buffer.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

void SnapshotWriter::writeFloatRaw(float v) {
    // Bitwise aliasing via memcpy — stable, well-defined, matches
    // every target platform StratumV supports (all x86-64 little-
    // endian). IEEE-754 binary32 layout is round-trip safe.
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32(bits);
}

void SnapshotWriter::writeDoubleRaw(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU64(bits);
}

void SnapshotWriter::writeVarintU32(uint32_t v) {
    // LEB128-style: 7 payload bits per byte, MSB is continuation.
    while (v >= 0x80u) {
        m_buffer.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    m_buffer.push_back(static_cast<uint8_t>(v));
}

void SnapshotWriter::writeVarintU64(uint64_t v) {
    while (v >= 0x80u) {
        m_buffer.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    m_buffer.push_back(static_cast<uint8_t>(v));
}

void SnapshotWriter::writeZigzagI32(int32_t v) {
    // Zigzag: maps signed → unsigned so small magnitudes (positive
    // and negative) get small varint widths. (-1 → 1, 1 → 2, -2 → 3,
    // 2 → 4, ...).
    uint32_t u = (static_cast<uint32_t>(v) << 1) ^
                 static_cast<uint32_t>(v >> 31);
    writeVarintU32(u);
}

void SnapshotWriter::writeZigzagI64(int64_t v) {
    uint64_t u = (static_cast<uint64_t>(v) << 1) ^
                 static_cast<uint64_t>(v >> 63);
    writeVarintU64(u);
}

void SnapshotWriter::writeFloatQuantized(float v, float step) {
    // Caller owns the step > 0 invariant. Round to nearest; negative
    // values use lrintf semantics (round half away from zero is
    // acceptable — the test tolerance is half-step either way).
    const float  scaled  = v / step;
    const long   rounded = std::lrintf(scaled);
    const int64_t clamped = static_cast<int64_t>(rounded);
    writeZigzagI64(clamped);
}

void SnapshotWriter::writeBytes(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    m_buffer.insert(m_buffer.end(), p, p + bytes);
}

// ── SnapshotReader ───────────────────────────────────────

bool SnapshotReader::readU8(uint8_t& out) {
    if (m_cursor + 1 > m_size) return false;
    out = m_data[m_cursor];
    m_cursor += 1;
    return true;
}

bool SnapshotReader::readU16(uint16_t& out) {
    if (m_cursor + 2 > m_size) return false;
    out = static_cast<uint16_t>(m_data[m_cursor]) |
          (static_cast<uint16_t>(m_data[m_cursor + 1]) << 8);
    m_cursor += 2;
    return true;
}

bool SnapshotReader::readU32(uint32_t& out) {
    if (m_cursor + 4 > m_size) return false;
    out =  static_cast<uint32_t>(m_data[m_cursor])
        | (static_cast<uint32_t>(m_data[m_cursor + 1]) << 8)
        | (static_cast<uint32_t>(m_data[m_cursor + 2]) << 16)
        | (static_cast<uint32_t>(m_data[m_cursor + 3]) << 24);
    m_cursor += 4;
    return true;
}

bool SnapshotReader::readU64(uint64_t& out) {
    if (m_cursor + 8 > m_size) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<uint64_t>(m_data[m_cursor + i]) << (i * 8));
    }
    out = v;
    m_cursor += 8;
    return true;
}

bool SnapshotReader::readBool(bool& out) {
    uint8_t b;
    if (!readU8(b)) return false;
    out = (b != 0);
    return true;
}

bool SnapshotReader::readFloatRaw(float& out) {
    uint32_t bits;
    if (!readU32(bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

bool SnapshotReader::readDoubleRaw(double& out) {
    uint64_t bits;
    if (!readU64(bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

bool SnapshotReader::readVarintU32(uint32_t& out) {
    // Max width for a uint32 varint is 5 bytes (5 * 7 = 35 >= 32).
    uint32_t v     = 0;
    int      shift = 0;
    size_t   start = m_cursor;
    for (int i = 0; i < 5; ++i) {
        if (m_cursor >= m_size) {
            m_cursor = start;  // restore on EOF
            return false;
        }
        const uint8_t b = m_data[m_cursor++];
        v |= static_cast<uint32_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            out = v;
            return true;
        }
        shift += 7;
    }
    // 5 bytes consumed and the continuation bit is still set: the
    // buffer is malformed.
    m_cursor = start;
    return false;
}

bool SnapshotReader::readVarintU64(uint64_t& out) {
    // Max width for a uint64 varint is 10 bytes (10 * 7 = 70 >= 64).
    uint64_t v     = 0;
    int      shift = 0;
    size_t   start = m_cursor;
    for (int i = 0; i < 10; ++i) {
        if (m_cursor >= m_size) {
            m_cursor = start;
            return false;
        }
        const uint8_t b = m_data[m_cursor++];
        v |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            out = v;
            return true;
        }
        shift += 7;
    }
    m_cursor = start;
    return false;
}

bool SnapshotReader::readZigzagI32(int32_t& out) {
    uint32_t u;
    if (!readVarintU32(u)) return false;
    // Inverse zigzag: (u >> 1) ^ -(u & 1)
    out = static_cast<int32_t>((u >> 1) ^ (0u - (u & 1u)));
    return true;
}

bool SnapshotReader::readZigzagI64(int64_t& out) {
    uint64_t u;
    if (!readVarintU64(u)) return false;
    out = static_cast<int64_t>((u >> 1) ^ (0ull - (u & 1ull)));
    return true;
}

bool SnapshotReader::readFloatQuantized(float& out, float step) {
    int64_t q;
    if (!readZigzagI64(q)) return false;
    out = static_cast<float>(q) * step;
    return true;
}

bool SnapshotReader::readBytes(void* dst, size_t bytes) {
    if (m_cursor + bytes > m_size) return false;
    std::memcpy(dst, m_data + m_cursor, bytes);
    m_cursor += bytes;
    return true;
}

// ── encodeSnapshot / decodeSnapshot ──────────────────────

namespace {

// Size of the byte-packed dirty mask prefix for a component with N
// fields: ceil(N / 8). An empty-field component still writes 0 bytes.
inline size_t maskByteWidth(size_t fieldCount) {
    return (fieldCount + 7) / 8;
}

// Encode one field from `src` (raw pointer to the component field)
// into `out`, dispatching on `desc.type`. Returns false for Unknown
// or non-scalar types — only the scalar path is locked down;
// Vec2/3/4/Quat/etc. will be added when the consumer needs them
// (see REPLICATION_CONTRACT.md §3).
bool encodeField(const FieldDesc& desc, const void* src, SnapshotWriter& out) {
    switch (desc.type) {
        case FieldType::Bool: {
            bool v;
            std::memcpy(&v, src, sizeof(v));
            out.writeBool(v);
            return true;
        }
        case FieldType::Int8: {
            int8_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeZigzagI32(static_cast<int32_t>(v));
            return true;
        }
        case FieldType::Int16: {
            int16_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeZigzagI32(static_cast<int32_t>(v));
            return true;
        }
        case FieldType::Int32: {
            int32_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeZigzagI32(v);
            return true;
        }
        case FieldType::Int64: {
            int64_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeZigzagI64(v);
            return true;
        }
        case FieldType::UInt8: {
            uint8_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeVarintU32(static_cast<uint32_t>(v));
            return true;
        }
        case FieldType::UInt16: {
            uint16_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeVarintU32(static_cast<uint32_t>(v));
            return true;
        }
        case FieldType::UInt32: {
            uint32_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeVarintU32(v);
            return true;
        }
        case FieldType::UInt64: {
            uint64_t v;
            std::memcpy(&v, src, sizeof(v));
            out.writeVarintU64(v);
            return true;
        }
        case FieldType::Float: {
            float v;
            std::memcpy(&v, src, sizeof(v));
            if (desc.quantStep > 0.0f) {
                out.writeFloatQuantized(v, desc.quantStep);
            } else {
                out.writeFloatRaw(v);
            }
            return true;
        }
        case FieldType::Double: {
            double v;
            std::memcpy(&v, src, sizeof(v));
            out.writeDoubleRaw(v);
            return true;
        }
        // Composite + opaque types are deferred to future work.
        // Only the scalar path ships; games that need glm::vec3 or
        // custom strings will add the case in their own adapter TU.
        case FieldType::Vec2:
        case FieldType::Vec3:
        case FieldType::Vec4:
        case FieldType::Quat:
        case FieldType::EntityHandle:
        case FieldType::StringFixed:
        case FieldType::Enum:
        case FieldType::Blob:
        case FieldType::Unknown:
            return false;
    }
    return false;
}

// Decode one field from `in` into `dst`. Mirrors encodeField.
bool decodeField(const FieldDesc& desc, void* dst, SnapshotReader& in) {
    switch (desc.type) {
        case FieldType::Bool: {
            bool v;
            if (!in.readBool(v)) return false;
            std::memcpy(dst, &v, sizeof(v));
            return true;
        }
        case FieldType::Int8: {
            int32_t v;
            if (!in.readZigzagI32(v)) return false;
            const int8_t narrow = static_cast<int8_t>(v);
            std::memcpy(dst, &narrow, sizeof(narrow));
            return true;
        }
        case FieldType::Int16: {
            int32_t v;
            if (!in.readZigzagI32(v)) return false;
            const int16_t narrow = static_cast<int16_t>(v);
            std::memcpy(dst, &narrow, sizeof(narrow));
            return true;
        }
        case FieldType::Int32: {
            int32_t v;
            if (!in.readZigzagI32(v)) return false;
            std::memcpy(dst, &v, sizeof(v));
            return true;
        }
        case FieldType::Int64: {
            int64_t v;
            if (!in.readZigzagI64(v)) return false;
            std::memcpy(dst, &v, sizeof(v));
            return true;
        }
        case FieldType::UInt8: {
            uint32_t v;
            if (!in.readVarintU32(v)) return false;
            const uint8_t narrow = static_cast<uint8_t>(v);
            std::memcpy(dst, &narrow, sizeof(narrow));
            return true;
        }
        case FieldType::UInt16: {
            uint32_t v;
            if (!in.readVarintU32(v)) return false;
            const uint16_t narrow = static_cast<uint16_t>(v);
            std::memcpy(dst, &narrow, sizeof(narrow));
            return true;
        }
        case FieldType::UInt32: {
            uint32_t v;
            if (!in.readVarintU32(v)) return false;
            std::memcpy(dst, &v, sizeof(v));
            return true;
        }
        case FieldType::UInt64: {
            uint64_t v;
            if (!in.readVarintU64(v)) return false;
            std::memcpy(dst, &v, sizeof(v));
            return true;
        }
        case FieldType::Float: {
            float v;
            if (desc.quantStep > 0.0f) {
                if (!in.readFloatQuantized(v, desc.quantStep)) return false;
            } else {
                if (!in.readFloatRaw(v)) return false;
            }
            std::memcpy(dst, &v, sizeof(v));
            return true;
        }
        case FieldType::Double: {
            double v;
            if (!in.readDoubleRaw(v)) return false;
            std::memcpy(dst, &v, sizeof(v));
            return true;
        }
        case FieldType::Vec2:
        case FieldType::Vec3:
        case FieldType::Vec4:
        case FieldType::Quat:
        case FieldType::EntityHandle:
        case FieldType::StringFixed:
        case FieldType::Enum:
        case FieldType::Blob:
        case FieldType::Unknown:
            return false;
    }
    return false;
}

} // anonymous

bool encodeSnapshot(const ReplicationMeta& meta,
                    const void*            instance,
                    const DirtyMask&       mask,
                    SnapshotWriter&        out) {
    if (instance == nullptr) {
        SV_LOG_WARN("ReplicationRegistry",
            "encodeSnapshot: null instance for type '%s'",
            meta.typeName.c_str());
        return false;
    }
    if (mask.size() != meta.fields.size()) {
        SV_LOG_WARN("ReplicationRegistry",
            "encodeSnapshot: DirtyMask size %zu does not match field count %zu for type '%s'",
            mask.size(), meta.fields.size(), meta.typeName.c_str());
        return false;
    }

    // [u16: schemaVersion]
    out.writeU16(meta.schemaVersion);

    // [byteMask: ceil(fieldCount/8) bytes, LSB-first within each byte]
    const size_t fieldCount = meta.fields.size();
    const size_t maskBytes  = maskByteWidth(fieldCount);
    for (size_t byteIdx = 0; byteIdx < maskBytes; ++byteIdx) {
        uint8_t byte = 0;
        const size_t baseBit = byteIdx * 8;
        for (size_t bitIdx = 0; bitIdx < 8; ++bitIdx) {
            const size_t bit = baseBit + bitIdx;
            if (bit >= fieldCount) break;
            if (mask.test(bit)) {
                byte |= static_cast<uint8_t>(1u << bitIdx);
            }
        }
        out.writeU8(byte);
    }

    // [values of dirty fields, in declaration order]
    const auto* base = static_cast<const uint8_t*>(instance);
    for (size_t i = 0; i < fieldCount; ++i) {
        if (!mask.test(i)) continue;
        const FieldDesc& f = meta.fields[i];
        if (!encodeField(f, base + f.offset, out)) {
            SV_LOG_WARN("ReplicationRegistry",
                "encodeSnapshot: unsupported field type %s for '%s::%s' — only scalar fields are supported",
                fieldTypeToString(f.type),
                meta.typeName.c_str(), f.name.c_str());
            return false;
        }
    }
    return true;
}

bool decodeSnapshot(const ReplicationMeta& meta,
                    void*                  instance,
                    DirtyMask&             outMask,
                    SnapshotReader&        in) {
    if (instance == nullptr) {
        SV_LOG_WARN("ReplicationRegistry",
            "decodeSnapshot: null instance for type '%s'",
            meta.typeName.c_str());
        return false;
    }

    // [u16: schemaVersion]
    uint16_t wireVersion;
    if (!in.readU16(wireVersion)) {
        SV_LOG_WARN("ReplicationRegistry",
            "decodeSnapshot: EOF reading schemaVersion for '%s'",
            meta.typeName.c_str());
        return false;
    }
    if (wireVersion != meta.schemaVersion) {
        SV_LOG_WARN("ReplicationRegistry",
            "decodeSnapshot: schema version mismatch for '%s' — wire=0x%04x local=0x%04x",
            meta.typeName.c_str(),
            static_cast<unsigned>(wireVersion),
            static_cast<unsigned>(meta.schemaVersion));
        return false;
    }

    // Resize the output mask to the component's field count and
    // clear it. The caller gets told exactly which fields were
    // present on the wire.
    const size_t fieldCount = meta.fields.size();
    outMask.resize(fieldCount);

    // [byteMask]
    const size_t maskBytes = maskByteWidth(fieldCount);
    for (size_t byteIdx = 0; byteIdx < maskBytes; ++byteIdx) {
        uint8_t byte;
        if (!in.readU8(byte)) {
            SV_LOG_WARN("ReplicationRegistry",
                "decodeSnapshot: EOF reading dirty mask byte %zu for '%s'",
                byteIdx, meta.typeName.c_str());
            return false;
        }
        const size_t baseBit = byteIdx * 8;
        for (size_t bitIdx = 0; bitIdx < 8; ++bitIdx) {
            const size_t bit = baseBit + bitIdx;
            if (bit >= fieldCount) break;
            if ((byte & (1u << bitIdx)) != 0) {
                outMask.set(bit);
            }
        }
    }

    // [values]
    auto* base = static_cast<uint8_t*>(instance);
    for (size_t i = 0; i < fieldCount; ++i) {
        if (!outMask.test(i)) continue;
        const FieldDesc& f = meta.fields[i];
        if (!decodeField(f, base + f.offset, in)) {
            SV_LOG_WARN("ReplicationRegistry",
                "decodeSnapshot: decode failed for '%s::%s' (type %s)",
                meta.typeName.c_str(), f.name.c_str(),
                fieldTypeToString(f.type));
            return false;
        }
    }
    return true;
}

} // namespace sv
