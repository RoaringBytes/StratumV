// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#version 460

// Basic PBR fragment shader for skinned meshes.
// Outputs HDR color + motion vectors (zeroed for now).
// extended SceneData with a light tail + simple
// Lambertian loop for dynamic lights.

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;

#define SV_MAX_LIGHTS 8
struct Light {
    vec4 positionType;    // .xyz = world pos, .w = type (float-cast)
    vec4 directionRange;  // .xyz = unit direction, .w = range
    vec4 colorIntensity;  // .xyz = color,     .w = intensity
    vec4 coneParams;      // .x = cos(innerDeg), .y = cos(outerDeg), .z = enabled flag
};
layout(set = 0, binding = 0) uniform SceneData {
    mat4  viewProj;
    mat4  invViewProj;
    mat4  invViewProjUnjittered;
    vec4  cameraPos;
    vec4  sunDirection;  // .w = sunHeight
    vec4  sunColor;      // .w = sunIntensity
    vec4  ambientColor;
    uvec4 lightCount; // .x = count 0..SV_MAX_LIGHTS
    Light lights[SV_MAX_LIGHTS]; //
    vec4 materialOverride; // .rgb = tint, .a = strength
} scene;

void main()
{
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(scene.sunDirection.xyz);
    float NdotL = max(dot(N, L), 0.0);

    vec3 diffuse = vec3(0.7) * NdotL * scene.sunColor.xyz * scene.sunColor.w;
    vec3 ambient = vec3(0.7) * scene.ambientColor.xyz;

    // ── Dynamic lights ─────────────────────────
    // Same Lambertian + distance-falloff math as skinnedPBR.frag, but
    // applied to a flat 0.7 grey albedo since this shader doesn't
    // sample textures. Keeps the two pipelines visually consistent
    // when the same entity is lit by the same LightComponent.
    vec3 dynamicLightLo = vec3(0.0);
    uint activeLights = min(scene.lightCount.x, uint(SV_MAX_LIGHTS));
    vec3 albedoGrey = vec3(0.7);
    for (uint i = 0u; i < activeLights; ++i) {
        Light Li = scene.lights[i];
        if (Li.coneParams.z < 0.5) continue;
        uint ltype = uint(Li.positionType.w);

        vec3 toLight = Li.positionType.xyz - fragWorldPos;
        float dist = length(toLight);
        vec3 Ldir;
        float attenuation;

        if (ltype == 1u) {
            Ldir = -normalize(Li.directionRange.xyz);
            attenuation = 1.0;
        } else {
            Ldir = (dist > 0.0001) ? (toLight / dist) : vec3(0.0, 1.0, 0.0);
            float range = max(Li.directionRange.w, 0.0001);
            float d = clamp(dist / range, 0.0, 1.0);
            attenuation = (1.0 - d) * (1.0 - d);
        }

        if (ltype == 3u) {
            vec3 spotForward = normalize(Li.directionRange.xyz);
            float cosAngle = dot(-Ldir, spotForward);
            float inner = Li.coneParams.x;
            float outer = Li.coneParams.y;
            attenuation *= smoothstep(outer, inner, cosAngle);
        }

        float NdotLi = max(dot(N, Ldir), 0.0);
        dynamicLightLo += albedoGrey * Li.colorIntensity.xyz
                       * Li.colorIntensity.w * NdotLi * attenuation
                       / 3.14159265;
    }

    vec3 finalRgb = ambient + diffuse + dynamicLightLo;

    // ── material override ───────────────────────
    // Single global tint × strength applied at the very end of the
    // lighting pipeline. Strength = 0 (default) leaves rendering
    // byte-identical to 1.3.9; strength = 1 fully tints toward the
    // override color. The shader does NOT clamp strength itself, so
    // pushing > 1 from the bridge produces an over-saturated mix.
    finalRgb = mix(finalRgb,
                   finalRgb * scene.materialOverride.rgb,
                   clamp(scene.materialOverride.a, 0.0, 1.0));

    outColor  = vec4(finalRgb, 1.0);
    outMotion = vec2(0.0);
}
