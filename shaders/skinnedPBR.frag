// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#version 460

// PBR fragment shader for skinned meshes.
// Samples 5 PBR textures from MaterialPipeline descriptor set.
// Outputs HDR color + motion vectors (currently zeroed).

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;

// ── Set 0: Scene UBO ──────────────────────────────────────────────
// grew to 784 B with a lightCount + lights[8] tail
// appended at offset 256. The original sun/ambient block is
// byte-identical at offsets 0..255.
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

// ── Set 2: Material (MaterialPipeline layout) ─────────────────────
layout(set = 2, binding = 0) uniform MaterialUBO {
    vec4  baseColor;       // factor
    float metallic;        // factor
    float roughness;       // factor
    float _pad[2];
} material;

layout(set = 2, binding = 1) uniform sampler2D texBaseColor;
layout(set = 2, binding = 2) uniform sampler2D texNormal;
layout(set = 2, binding = 3) uniform sampler2D texMetallicRoughness;
layout(set = 2, binding = 4) uniform sampler2D texEmissive;
layout(set = 2, binding = 5) uniform sampler2D texOcclusion;
layout(set = 2, binding = 6) uniform sampler2D texOpacity;

// ── Push constants (fragment stage reads tintColor + alphaMode) ───
layout(push_constant) uniform PushConstants {
    layout(offset = 76) uint alphaMode;  // 0=opaque (alpha test), 1=alpha blend
    layout(offset = 80) vec4 tintColor;
};

void main()
{
    // ── Base color ────────────────────────────────────────────────
    vec4 albedo = texture(texBaseColor, fragUV) * material.baseColor;

    // Apply separate opacity texture (CC5 FBX stores transparency here)
    float opacity = texture(texOpacity, fragUV).r;
    albedo.a *= opacity;

    // Alpha handling: opaque uses hard cutoff, blend uses soft cutoff
    if (alphaMode == 0u) {
        if (albedo.a < 0.5) discard;
    } else {
        if (albedo.a < 0.01) discard;
    }

    // ── Normal mapping ────────────────────────────────────────────
    vec3 N = normalize(fragNormal);
    vec3 tangentNormal = texture(texNormal, fragUV).xyz * 2.0 - 1.0;

    // Derive TBN from screen-space derivatives (no tangent attribute needed)
    vec3 dPdx = dFdx(fragWorldPos);
    vec3 dPdy = dFdy(fragWorldPos);
    vec2 dUVdx = dFdx(fragUV);
    vec2 dUVdy = dFdy(fragUV);

    vec3 T = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);
    vec3 B = normalize(dPdy * dUVdx.x - dPdx * dUVdy.x);
    mat3 TBN = mat3(T, B, N);
    N = normalize(TBN * tangentNormal);

    // ── Metallic / roughness ──────────────────────────────────────
    vec2 mr = texture(texMetallicRoughness, fragUV).bg;  // glTF: B=metallic, G=roughness
    float metallic  = mr.x * material.metallic;
    float roughness = mr.y * material.roughness;

    // ── Occlusion ─────────────────────────────────────────────────
    float ao = texture(texOcclusion, fragUV).r;

    // ── Emissive ──────────────────────────────────────────────────
    vec3 emissive = texture(texEmissive, fragUV).rgb;

    // ── Lighting (simplified PBR — Lambertian + Schlick Fresnel) ──
    vec3 V = normalize(scene.cameraPos.xyz - fragWorldPos);
    vec3 L = normalize(scene.sunDirection.xyz);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Fresnel (Schlick)
    vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 F  = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    // GGX distribution
    float a  = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * denom * denom);

    // Geometry (Smith GGX, Schlick approximation)
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G1V = NdotV / (NdotV * (1.0 - k) + k);
    float G1L = NdotL / (NdotL * (1.0 - k) + k);
    float G = G1V * G1L;

    // Specular BRDF
    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Diffuse (energy conservation: (1-F) * (1-metallic))
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo.rgb / 3.14159265;

    // Direct lighting
    vec3 Lo = (diffuse + specular) * scene.sunColor.xyz * scene.sunColor.w * NdotL;

    // Ambient (simple hemisphere)
    vec3 ambient = scene.ambientColor.xyz * albedo.rgb * ao;

    // ── Dynamic lights ─────────────────────────
    // Replicated LightComponent sidecars arrive as a fixed-size array
    // in the SceneData UBO. The lab client packs up to SV_MAX_LIGHTS
    // active lights per frame; unused slots have enabled=0 and are
    // skipped. Lambertian + distance falloff only — full PBR at 8x
    // is a later perf session, and the visual checkpoint only needs
    // the contribution to be visibly distinct from the sun-only
    // baseline.
    vec3 dynamicLightLo = vec3(0.0);
    uint activeLights = min(scene.lightCount.x, uint(SV_MAX_LIGHTS));
    for (uint i = 0u; i < activeLights; ++i) {
        Light Li = scene.lights[i];
        if (Li.coneParams.z < 0.5) continue;   // disabled slot
        uint ltype = uint(Li.positionType.w);

        vec3 toLight = Li.positionType.xyz - fragWorldPos;
        float dist = length(toLight);
        vec3 Ldir;
        float attenuation;

        if (ltype == 1u) {
            // Directional — entity quaternion supplies forward dir
            // in Li.directionRange.xyz; clients store it already
            // normalized.
            Ldir = -normalize(Li.directionRange.xyz);
            attenuation = 1.0;
        } else {
            // Point (2) or Spot (3) — attenuate by distance.
            Ldir = (dist > 0.0001) ? (toLight / dist) : vec3(0.0, 1.0, 0.0);
            float range = max(Li.directionRange.w, 0.0001);
            float d = clamp(dist / range, 0.0, 1.0);
            attenuation = (1.0 - d) * (1.0 - d);
        }

        if (ltype == 3u) {
            // Spot cone smoothstep between inner + outer cosines.
            vec3 spotForward = normalize(Li.directionRange.xyz);
            float cosAngle = dot(-Ldir, spotForward);
            float inner = Li.coneParams.x;
            float outer = Li.coneParams.y;
            attenuation *= smoothstep(outer, inner, cosAngle);
        }

        float NdotLi = max(dot(N, Ldir), 0.0);
        dynamicLightLo += albedo.rgb * Li.colorIntensity.xyz
                       * Li.colorIntensity.w * NdotLi * attenuation
                       / 3.14159265;
    }

    vec3 finalRgb = ambient + Lo + dynamicLightLo + emissive;

    // ── material override ───────────────────────
    // Single global tint × strength applied at the very end of the
    // PBR pipeline. Strength = 0 (default) leaves rendering byte-
    // identical to 1.3.9; strength = 1 fully tints toward the
    // override color. Same code path as skinned.frag so both
    // pipelines render an entity identically when both pick up the
    // same MaterialComponent state.
    finalRgb = mix(finalRgb,
                   finalRgb * scene.materialOverride.rgb,
                   clamp(scene.materialOverride.a, 0.0, 1.0));

    float outAlpha = (alphaMode == 0u) ? 1.0 : albedo.a;
    outColor  = vec4(finalRgb, outAlpha);
    outMotion = vec2(0.0);
}
