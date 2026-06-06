// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

// ── FrustumCuller ──────────────────────────────────────────────────
// CPU frustum culling utility.  Extracts 6 frustum planes from a
// view-projection matrix and tests axis-aligned bounding boxes.
// Layer 4 — depends on: glm only (no engine deps)

#include <glm/glm.hpp>
#include <array>

namespace sv {

// Plane stored as (normal.x, normal.y, normal.z, distance).
// Positive half-space = inside the frustum.
using FrustumPlane = glm::vec4;

enum class CullResult : uint8_t {
    Outside     = 0,  // entirely outside frustum
    Inside      = 1,  // entirely inside frustum
    Intersecting = 2  // partially inside
};

// ── FrustumCuller ─────────────────────────────────────────────────
class FrustumCuller {
public:
    // Extract 6 normalized frustum planes from a combined
    // view-projection matrix (Gribb-Hartmann method).
    void extractFromMatrix(const glm::mat4& viewProj);

    // Test an AABB against the frustum.
    // Returns Outside if fully outside any plane, Inside if fully
    // inside all planes, Intersecting otherwise.
    CullResult testAABB(const glm::vec3& min, const glm::vec3& max) const;

    // Convenience: true if the AABB is at least partially visible.
    bool isVisible(const glm::vec3& min, const glm::vec3& max) const {
        return testAABB(min, max) != CullResult::Outside;
    }

    // Access individual planes (Left, Right, Bottom, Top, Near, Far).
    const std::array<FrustumPlane, 6>& planes() const { return m_planes; }

private:
    std::array<FrustumPlane, 6> m_planes{};
};

} // namespace sv
