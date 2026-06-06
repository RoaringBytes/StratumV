// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── FrustumCuller ──────────────────────────────────────────────────
// Layer 4 — 6-plane frustum extraction + AABB culling

#include "FrustumCuller.h"
#include <cmath>

namespace sv {

// ── Gribb-Hartmann plane extraction ───────────────────────────────
// Given column-major VP matrix M, the six frustum planes are derived
// from linear combinations of M's rows.  Each plane is then normalized
// so that distance tests give true signed distances.

void FrustumCuller::extractFromMatrix(const glm::mat4& vp)
{
    // glm is column-major: vp[col][row].
    // Row i of the matrix = (vp[0][i], vp[1][i], vp[2][i], vp[3][i]).

    auto row = [&](int i) -> glm::vec4 {
        return { vp[0][i], vp[1][i], vp[2][i], vp[3][i] };
    };

    const glm::vec4 r0 = row(0);
    const glm::vec4 r1 = row(1);
    const glm::vec4 r2 = row(2);
    const glm::vec4 r3 = row(3);

    // Left:   row3 + row0
    m_planes[0] = r3 + r0;
    // Right:  row3 - row0
    m_planes[1] = r3 - r0;
    // Bottom: row3 + row1
    m_planes[2] = r3 + r1;
    // Top:    row3 - row1
    m_planes[3] = r3 - r1;
    // Near:   row3 + row2
    m_planes[4] = r3 + r2;
    // Far:    row3 - row2
    m_planes[5] = r3 - r2;

    // Normalize each plane so that (a,b,c) is unit length.
    for (auto& p : m_planes) {
        float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len > 1e-8f)
            p /= len;
    }
}

// ── AABB vs frustum test ──────────────────────────────────────────
// For each plane, find the AABB vertex most in the direction of the
// plane normal (p-vertex) and the one most against it (n-vertex).
//   - If the p-vertex is behind the plane → box is entirely outside.
//   - If the n-vertex is behind the plane → box intersects.
//   - If all n-vertices are in front of their planes → fully inside.

CullResult FrustumCuller::testAABB(const glm::vec3& bmin,
                                    const glm::vec3& bmax) const
{
    bool allInside = true;

    for (const auto& plane : m_planes) {
        const glm::vec3 normal(plane.x, plane.y, plane.z);
        const float d = plane.w;

        // p-vertex: pick the corner farthest along the normal
        glm::vec3 pv;
        pv.x = (normal.x >= 0.0f) ? bmax.x : bmin.x;
        pv.y = (normal.y >= 0.0f) ? bmax.y : bmin.y;
        pv.z = (normal.z >= 0.0f) ? bmax.z : bmin.z;

        // If the p-vertex is behind the plane, the whole box is outside
        if (glm::dot(normal, pv) + d < 0.0f)
            return CullResult::Outside;

        // n-vertex: the opposite corner
        glm::vec3 nv;
        nv.x = (normal.x >= 0.0f) ? bmin.x : bmax.x;
        nv.y = (normal.y >= 0.0f) ? bmin.y : bmax.y;
        nv.z = (normal.z >= 0.0f) ? bmin.z : bmax.z;

        // If the n-vertex is behind the plane, box is not fully inside
        if (glm::dot(normal, nv) + d < 0.0f)
            allInside = false;
    }

    return allInside ? CullResult::Inside : CullResult::Intersecting;
}

} // namespace sv
