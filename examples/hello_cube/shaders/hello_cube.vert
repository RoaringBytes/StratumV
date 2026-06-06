// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#version 460

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(set = 0, binding = 0) uniform SceneBlock {
    mat4 viewProj;
    mat4 model;
} u;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = inColor;
    gl_Position = u.viewProj * u.model * vec4(inPos, 1.0);
}
