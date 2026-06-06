// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#version 460

// Skinned mesh shadow vertex shader.
// Depth-only pass with 4-weight GPU skinning.

layout(location = 0) in vec3  inPos;
layout(location = 1) in vec3  inNormal;   // unused in shadow pass
layout(location = 2) in vec2  inUV;       // unused in shadow pass
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4  inWeights;

// ── Descriptor set 0: scene UBO (light-space view-proj per cascade) ──
layout(set = 0, binding = 0) uniform SceneData {
    mat4 viewProj;
} scene;

// ── Descriptor set 1: bone palette SSBO ──
layout(std430, set = 1, binding = 0) readonly buffer BonePalette {
    mat4 bones[];
};

// ── Push constants ──
layout(push_constant) uniform PushConstants {
    mat4 model;
    uint cascadeIndex;
    uint boneOffset;
};

void main()
{
    mat4 skin = inWeights.x * bones[boneOffset + inJoints.x]
              + inWeights.y * bones[boneOffset + inJoints.y]
              + inWeights.z * bones[boneOffset + inJoints.z]
              + inWeights.w * bones[boneOffset + inJoints.w];

    gl_Position = scene.viewProj * model * skin * vec4(inPos, 1.0);
}
