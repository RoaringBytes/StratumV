// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#version 460

// Skinned mesh vertex shader (morph targets).
// Morph target blending + 4-weight GPU skinning from SSBO bone palette.
// Bone indices in vertex data use glTF skin.joints order;
// skinning matrices in the SSBO are pre-ordered to match.

// ── Vertex inputs (MeshVertex, 64 bytes) ───────────────────────
layout(location = 0) in vec3  inPos;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inUV;
layout(location = 3) in uvec4 inJoints;   // bone indices (4 influences)
layout(location = 4) in vec4  inWeights;  // bone weights (4 influences)

// ── Descriptor set 0: scene UBO ────────────────────────────────
layout(set = 0, binding = 0) uniform SceneData {
    mat4 viewProj;
    // remaining SceneUBO fields not needed by vertex shader
} scene;

// ── Descriptor set 1: bone palette SSBO ────────────────────────
layout(std430, set = 1, binding = 0) readonly buffer BonePalette {
    mat4 bones[];
};

// ── Descriptor set 3: morph target deltas SSBO ─────────────────
// Layout: vec4[(t * vertexCount + v) * 2 + 0].xyz = position delta
//         vec4[(t * vertexCount + v) * 2 + 1].xyz = normal delta
layout(std430, set = 3, binding = 0) readonly buffer MorphTargets {
    vec4 morphDeltas[];
};

// ── Push constants (128 bytes) ─────────────────────────────────
layout(push_constant) uniform PushConstants {
    mat4 model;             // 0-63
    uint boneOffset;        // 64-67
    uint morphTargetCount;  // 68-71
    uint vertexCount;       // 72-75
    uint alphaMode;         // 76-79 (0=opaque, 1=alpha blend)
    vec4 tintColor;         // 80-95  (read by fragment shader)
    vec4 morphWeights0;     // 96-111 (morph weights 0-3)
    vec4 morphWeights1;     // 112-127 (morph weights 4-7)
};

// ── Outputs ────────────────────────────────────────────────────
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;

void main()
{
    // ── Morph target blending (before skinning) ────────────────
    vec3 morphedPos  = inPos;
    vec3 morphedNorm = inNormal;

    if (morphTargetCount > 0u) {
        float w[8] = float[](
            morphWeights0.x, morphWeights0.y, morphWeights0.z, morphWeights0.w,
            morphWeights1.x, morphWeights1.y, morphWeights1.z, morphWeights1.w
        );

        for (uint t = 0u; t < min(morphTargetCount, 8u); t++) {
            if (w[t] != 0.0) {
                uint idx = (t * vertexCount + gl_VertexIndex) * 2u;
                morphedPos  += w[t] * morphDeltas[idx].xyz;
                morphedNorm += w[t] * morphDeltas[idx + 1u].xyz;
            }
        }
    }

    // ── 4-weight linear blend skinning ─────────────────────────
    mat4 skin = inWeights.x * bones[boneOffset + inJoints.x]
              + inWeights.y * bones[boneOffset + inJoints.y]
              + inWeights.z * bones[boneOffset + inJoints.z]
              + inWeights.w * bones[boneOffset + inJoints.w];

    vec4 worldPos = model * skin * vec4(morphedPos, 1.0);
    fragWorldPos  = worldPos.xyz;

    // Approximate normal transform (correct for uniform scale)
    fragNormal = normalize(mat3(model) * mat3(skin) * morphedNorm);

    fragUV = inUV;

    gl_Position = scene.viewProj * worldPos;
}
