// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif

#include "FbxLoader.h"
#include "CC5Sidecar.h"
#include "../MorphTargetTypes.h"   // MAX_VERTEX_SHADER_MORPH_TARGETS

#include "stb_image.h"   // declarations only — impl in VkTexture.cpp

#include <ufbx.h>
#include <glm/gtc/matrix_inverse.hpp>

#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>

namespace sv {

bool loadFbx(const std::string& path, bool loadTextures, MeshImportData& out)
{
    ufbx_load_opts opts = {};
    opts.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    opts.target_axes.up    = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    opts.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
    opts.space_conversion   = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    opts.target_unit_meters = 1.0f;
    opts.clean_skin_weights = true;
    if (!loadTextures) opts.ignore_embedded = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) {
        printf("[FbxLoader] FBX load failed: %s (%s)\n", path.c_str(), error.description.data);
        return false;
    }

    // Debug: print source coordinate system + units
    {
        auto axName = [](ufbx_coordinate_axis a) -> const char* {
            switch (a) {
                case UFBX_COORDINATE_AXIS_POSITIVE_X: return "+X";
                case UFBX_COORDINATE_AXIS_NEGATIVE_X: return "-X";
                case UFBX_COORDINATE_AXIS_POSITIVE_Y: return "+Y";
                case UFBX_COORDINATE_AXIS_NEGATIVE_Y: return "-Y";
                case UFBX_COORDINATE_AXIS_POSITIVE_Z: return "+Z";
                case UFBX_COORDINATE_AXIS_NEGATIVE_Z: return "-Z";
                default: return "?";
            }
        };
        auto& ax = scene->settings.axes;
        printf("[FbxLoader] FBX source axes: right=%s up=%s front=%s | unit=%.4f m/unit\n",
               axName(ax.right), axName(ax.up), axName(ax.front),
               scene->settings.unit_meters);
    }

    auto& allVertices = out.vertices;
    auto& allIndices  = out.indices;
    size_t morphTargetCount = 0;

    // Reference skin deformer (largest cluster count across all meshes)
    ufbx_skin_deformer* refSkin = nullptr;
    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        ufbx_mesh* mesh = scene->meshes.data[mi];
        if (mesh->skin_deformers.count > 0) {
            ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
            if (!refSkin || skin->clusters.count > refSkin->clusters.count)
                refSkin = skin;
        }
    }

    // ── Materials ────────────────────────────────────────────────────────
    auto toLower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };

    for (size_t mi = 0; mi < scene->materials.count; mi++) {
        ufbx_material* fbxMat = scene->materials.data[mi];
        MeshMaterial mat;
        mat.name = std::string(fbxMat->name.data, fbxMat->name.length);

        if (fbxMat->pbr.base_color.has_value) {
            auto& c = fbxMat->pbr.base_color.value_vec4;
            mat.baseColor = glm::vec4((float)c.x, (float)c.y, (float)c.z, (float)c.w);
        } else if (fbxMat->fbx.diffuse_color.has_value) {
            auto& c = fbxMat->fbx.diffuse_color.value_vec3;
            mat.baseColor = glm::vec4((float)c.x, (float)c.y, (float)c.z, 1.0f);
        }
        if (fbxMat->pbr.metalness.has_value)
            mat.metallic = (float)fbxMat->pbr.metalness.value_real;
        if (fbxMat->pbr.roughness.has_value)
            mat.roughness = (float)fbxMat->pbr.roughness.value_real;

        out.materials.push_back(mat);
    }

    // ── CC5 JSON sidecar ─────────────────────────────────────────────────
    CC5MatMap cc5Map = parseCC5Sidecar(path);

    // Apply CC5 data to engine materials (BlendMode, twoSided)
    for (auto& mat : out.materials) {
        auto it = cc5Map.find(mat.name);
        if (it != cc5Map.end()) {
            const auto& info = it->second;
            mat.twoSided = info.twoSide;
            if (info.nodeType == "Hair" || info.nodeType == "Eyelash" || info.nodeType == "Brow" ||
                (info.twoSide && info.hasOpacity)) {
                mat.blendMode = BlendMode::AlphaBlend;
            }
            if (mat.blendMode != BlendMode::Opaque)
                printf("[FbxLoader] Material '%s' -> AlphaBlend (NodeType=%s, TwoSide=%s)\n",
                       mat.name.c_str(), info.nodeType.c_str(), info.twoSide ? "true" : "false");
        } else if (cc5Map.empty()) {
            // Fallback: no CC5 JSON — use name-based detection for non-CC5 FBX files
            std::string nameLower = toLower(mat.name);
            if (nameLower.find("transparency") != std::string::npos ||
                nameLower.find("eyelash") != std::string::npos ||
                nameLower.find("hair")    != std::string::npos ||
                nameLower.find("scalp")   != std::string::npos ||
                nameLower.find("lash")    != std::string::npos) {
                mat.blendMode = BlendMode::AlphaBlend;
                printf("[FbxLoader] Material '%s' -> AlphaBlend (name fallback)\n", mat.name.c_str());
            }
        }
    }

    // Lookup: bone node -> refSkin cluster index
    std::unordered_map<ufbx_node*, int> boneToCluster;
    if (refSkin) {
        for (size_t ci = 0; ci < refSkin->clusters.count; ci++) {
            if (refSkin->clusters.data[ci]->bone_node)
                boneToCluster[refSkin->clusters.data[ci]->bone_node] = (int)ci;
        }
    }

    printf("[FbxLoader] FBX scene: %zu meshes, %zu materials, refSkin=%zu clusters\n",
           scene->meshes.count, scene->materials.count,
           refSkin ? refSkin->clusters.count : 0);

    // ── Process each mesh ────────────────────────────────────────────────
    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        ufbx_mesh* mesh = scene->meshes.data[mi];

        glm::mat4 geoToWorld(1.0f);
        glm::mat3 geoToWorldNorm(1.0f);
        if (mesh->instances.count > 0) {
            auto toGlmMat = [](const ufbx_matrix& m) -> glm::mat4 {
                glm::mat4 r;
                r[0] = glm::vec4((float)m.cols[0].x, (float)m.cols[0].y, (float)m.cols[0].z, 0.0f);
                r[1] = glm::vec4((float)m.cols[1].x, (float)m.cols[1].y, (float)m.cols[1].z, 0.0f);
                r[2] = glm::vec4((float)m.cols[2].x, (float)m.cols[2].y, (float)m.cols[2].z, 0.0f);
                r[3] = glm::vec4((float)m.cols[3].x, (float)m.cols[3].y, (float)m.cols[3].z, 1.0f);
                return r;
            };
            geoToWorld = toGlmMat(mesh->instances.data[0]->geometry_to_world);
            geoToWorldNorm = glm::mat3(geoToWorld);
        }

        size_t vertexOffset = allVertices.size();
        allVertices.resize(vertexOffset + mesh->num_indices);

        for (size_t vi = 0; vi < mesh->num_indices; vi++) {
            MeshVertex& v = allVertices[vertexOffset + vi];

            ufbx_vec3 pos = mesh->vertex_position.values.data[
                mesh->vertex_position.indices.data[vi]];
            glm::vec3 rawPos((float)pos.x, (float)pos.y, (float)pos.z);
            v.pos = glm::vec3(geoToWorld * glm::vec4(rawPos, 1.0f));

            if (mesh->vertex_normal.exists) {
                ufbx_vec3 norm = mesh->vertex_normal.values.data[
                    mesh->vertex_normal.indices.data[vi]];
                glm::vec3 rawNorm((float)norm.x, (float)norm.y, (float)norm.z);
                v.normal = glm::normalize(geoToWorldNorm * rawNorm);
            } else {
                v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            if (mesh->vertex_uv.exists) {
                ufbx_vec2 uv = mesh->vertex_uv.values.data[
                    mesh->vertex_uv.indices.data[vi]];
                v.uv = glm::vec2((float)uv.x, 1.0f - (float)uv.y);
            }
        }

        // ── Skin weights ─────────────────────────────────────────────────
        if (mesh->skin_deformers.count > 0) {
            ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];

            std::vector<int> clusterRemap;
            bool needsRemap = (skin != refSkin && refSkin);
            if (needsRemap) {
                clusterRemap.resize(skin->clusters.count, -1);
                for (size_t ci = 0; ci < skin->clusters.count; ci++) {
                    if (!skin->clusters.data[ci]->bone_node) continue;
                    std::string boneName(
                        skin->clusters.data[ci]->bone_node->name.data,
                        skin->clusters.data[ci]->bone_node->name.length);
                    for (size_t ri = 0; ri < refSkin->clusters.count; ri++) {
                        if (!refSkin->clusters.data[ri]->bone_node) continue;
                        ufbx_string rn = refSkin->clusters.data[ri]->bone_node->name;
                        if (boneName == std::string(rn.data, rn.length)) {
                            clusterRemap[ci] = (int)ri;
                            break;
                        }
                    }
                }
                for (size_t ri = 0; ri < clusterRemap.size(); ri++) {
                    if (clusterRemap[ri] >= 0) continue;
                    ufbx_node* bone = skin->clusters.data[ri]->bone_node;
                    if (!bone) continue;
                    ufbx_node* cur = bone->parent;
                    while (cur) {
                        auto ait = boneToCluster.find(cur);
                        if (ait != boneToCluster.end()) {
                            clusterRemap[ri] = ait->second;
                            break;
                        }
                        cur = cur->parent;
                    }
                }
            }

            for (size_t vi = 0; vi < mesh->num_indices; vi++) {
                uint32_t logVert = mesh->vertex_position.indices.data[vi];
                if (logVert >= skin->vertices.count) continue;

                ufbx_skin_vertex sv = skin->vertices.data[logVert];
                MeshVertex& v = allVertices[vertexOffset + vi];

                uint32_t numW = std::min(sv.num_weights, (uint32_t)4);
                for (uint32_t wi = 0; wi < numW; wi++) {
                    ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + wi];
                    uint32_t jointIdx = sw.cluster_index;
                    if (needsRemap) {
                        if (jointIdx < clusterRemap.size() && clusterRemap[jointIdx] >= 0)
                            jointIdx = (uint32_t)clusterRemap[jointIdx];
                        else
                            continue;
                    }
                    v.joints[wi] = jointIdx;
                    v.weights[wi] = (float)sw.weight;
                }
            }
        }

        // ── Blend shapes (morph targets) ─────────────────────────────────
        if (mesh->blend_deformers.count > 0) {
            ufbx_blend_deformer* blendDef = mesh->blend_deformers.data[0];
            size_t numTargets = std::min(blendDef->channels.count,
                                         (size_t)MAX_VERTEX_SHADER_MORPH_TARGETS);
            if (morphTargetCount == 0 && numTargets > 0) {
                morphTargetCount = numTargets;
                out.morphPosDeltas.resize(numTargets);
                out.morphNormDeltas.resize(numTargets);
            }

            size_t totalVerts = vertexOffset + mesh->num_indices;
            for (size_t t = 0; t < morphTargetCount; t++) {
                out.morphPosDeltas[t].resize(totalVerts, glm::vec3(0.0f));
                out.morphNormDeltas[t].resize(totalVerts, glm::vec3(0.0f));
            }

            std::vector<std::vector<uint32_t>> logToFace(mesh->num_vertices);
            for (size_t fi = 0; fi < mesh->num_indices; fi++) {
                uint32_t lv = mesh->vertex_position.indices.data[fi];
                logToFace[lv].push_back((uint32_t)fi);
            }

            for (size_t t = 0; t < numTargets && t < morphTargetCount; t++) {
                ufbx_blend_channel* channel = blendDef->channels.data[t];
                if (!channel->target_shape) continue;
                ufbx_blend_shape* shape = channel->target_shape;

                for (size_t oi = 0; oi < shape->position_offsets.count &&
                     oi < shape->offset_vertices.count; oi++) {
                    uint32_t lv = shape->offset_vertices.data[oi];
                    ufbx_vec3 pd = shape->position_offsets.data[oi];
                    if (lv < logToFace.size()) {
                        glm::vec3 delta = geoToWorldNorm * glm::vec3((float)pd.x, (float)pd.y, (float)pd.z);
                        for (uint32_t fv : logToFace[lv])
                            out.morphPosDeltas[t][vertexOffset + fv] = delta;
                    }
                }

                for (size_t oi = 0; oi < shape->normal_offsets.count &&
                     oi < shape->offset_vertices.count; oi++) {
                    uint32_t lv = shape->offset_vertices.data[oi];
                    ufbx_vec3 nd = shape->normal_offsets.data[oi];
                    if (lv < logToFace.size()) {
                        glm::vec3 delta = geoToWorldNorm * glm::vec3((float)nd.x, (float)nd.y, (float)nd.z);
                        for (uint32_t fv : logToFace[lv])
                            out.morphNormDeltas[t][vertexOffset + fv] = delta;
                    }
                }
            }
        }

        // ── Triangulate + build index buffer per material part ───────────
        if (mesh->material_parts.count > 0) {
            for (size_t pi = 0; pi < mesh->material_parts.count; pi++) {
                ufbx_mesh_part& part = mesh->material_parts.data[pi];

                SubMesh sub;
                sub.indexOffset = (uint32_t)allIndices.size();
                sub.materialIndex = (part.index < mesh->materials.count && mesh->materials.data[part.index])
                    ? (int)mesh->materials.data[part.index]->typed_id : -1;

                for (size_t fi = 0; fi < part.face_indices.count; fi++) {
                    uint32_t faceIdx = part.face_indices.data[fi];
                    ufbx_face face = mesh->faces.data[faceIdx];
                    uint32_t triIdx[256];
                    uint32_t numTri = ufbx_triangulate_face(triIdx, 256, mesh, face);
                    for (uint32_t ti = 0; ti < numTri * 3; ti++)
                        allIndices.push_back((uint32_t)(vertexOffset + triIdx[ti]));
                }

                sub.indexCount = (uint32_t)allIndices.size() - sub.indexOffset;
                if (sub.indexCount > 0)
                    out.submeshes.push_back(sub);
            }
        } else {
            SubMesh sub;
            sub.indexOffset = (uint32_t)allIndices.size();
            sub.materialIndex = -1;
            for (size_t fi = 0; fi < mesh->num_faces; fi++) {
                ufbx_face face = mesh->faces.data[fi];
                uint32_t triIdx[256];
                uint32_t numTri = ufbx_triangulate_face(triIdx, 256, mesh, face);
                for (uint32_t ti = 0; ti < numTri * 3; ti++)
                    allIndices.push_back((uint32_t)(vertexOffset + triIdx[ti]));
            }
            sub.indexCount = (uint32_t)allIndices.size() - sub.indexOffset;
            if (sub.indexCount > 0)
                out.submeshes.push_back(sub);
        }
    }

    if (allVertices.empty()) {
        printf("[FbxLoader] No geometry in FBX: %s\n", path.c_str());
        ufbx_free_scene(scene);
        return false;
    }

    out.morphTargetCount = morphTargetCount;
    uint32_t vertexCount = (uint32_t)allVertices.size();

    // ── Morph target names + default weights ─────────────────────────────
    if (morphTargetCount > 0) {
        out.morphTargetNames.resize(morphTargetCount);
        out.morphTargetDefaultWeights.resize(morphTargetCount, 0.0f);

        for (size_t mshi = 0; mshi < scene->meshes.count; mshi++) {
            ufbx_mesh* mesh = scene->meshes.data[mshi];
            if (mesh->blend_deformers.count == 0) continue;
            ufbx_blend_deformer* bd = mesh->blend_deformers.data[0];
            for (size_t t = 0; t < morphTargetCount && t < bd->channels.count; t++) {
                ufbx_blend_channel* ch = bd->channels.data[t];
                if (ch->name.length > 0 && out.morphTargetNames[t].empty())
                    out.morphTargetNames[t] = std::string(ch->name.data, ch->name.length);
                out.morphTargetDefaultWeights[t] = (float)ch->weight;
            }
            break;
        }

        printf("[FbxLoader] FBX morph targets: %d targets, %d verts from '%s'\n",
               (int)morphTargetCount, vertexCount, path.c_str());
    }

    // ── Build skeleton from reference skin deformer ──────────────────────
    if (refSkin) {
        out.skeleton.joints.resize(refSkin->clusters.count);

        auto toGlm = [](const ufbx_matrix& m) -> glm::mat4 {
            glm::mat4 r;
            r[0] = glm::vec4((float)m.cols[0].x, (float)m.cols[0].y, (float)m.cols[0].z, 0.0f);
            r[1] = glm::vec4((float)m.cols[1].x, (float)m.cols[1].y, (float)m.cols[1].z, 0.0f);
            r[2] = glm::vec4((float)m.cols[2].x, (float)m.cols[2].y, (float)m.cols[2].z, 0.0f);
            r[3] = glm::vec4((float)m.cols[3].x, (float)m.cols[3].y, (float)m.cols[3].z, 1.0f);
            return r;
        };

        auto decomposeTRS = [](const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
            t = glm::vec3(m[3]);
            s.x = glm::length(glm::vec3(m[0]));
            s.y = glm::length(glm::vec3(m[1]));
            s.z = glm::length(glm::vec3(m[2]));
            glm::mat3 rotMat(
                glm::vec3(m[0]) / s.x,
                glm::vec3(m[1]) / s.y,
                glm::vec3(m[2]) / s.z);
            r = glm::quat_cast(rotMat);
        };

        for (size_t ci = 0; ci < refSkin->clusters.count; ci++) {
            ufbx_skin_cluster* cluster = refSkin->clusters.data[ci];
            ufbx_node* bone = cluster->bone_node;
            if (!bone) continue;

            out.skeleton.joints[ci].name = std::string(bone->name.data, bone->name.length);
            out.skeleton.joints[ci].inverseBindMatrix = glm::inverse(toGlm(cluster->bind_to_world));

            out.skeleton.joints[ci].parent = -1;
            ufbx_node* cur = bone->parent;
            while (cur) {
                auto it = boneToCluster.find(cur);
                if (it != boneToCluster.end()) {
                    out.skeleton.joints[ci].parent = it->second;
                    break;
                }
                cur = cur->parent;
            }

            glm::mat4 boneWorld = toGlm(bone->node_to_world);
            if (out.skeleton.joints[ci].parent >= 0) {
                ufbx_node* parentBone = refSkin->clusters.data[out.skeleton.joints[ci].parent]->bone_node;
                glm::mat4 parentWorld = toGlm(parentBone->node_to_world);
                glm::mat4 localMat = glm::inverse(parentWorld) * boneWorld;
                decomposeTRS(localMat,
                    out.skeleton.joints[ci].restTranslation,
                    out.skeleton.joints[ci].restRotation,
                    out.skeleton.joints[ci].restScale);
            } else {
                decomposeTRS(boneWorld,
                    out.skeleton.joints[ci].restTranslation,
                    out.skeleton.joints[ci].restRotation,
                    out.skeleton.joints[ci].restScale);
            }
        }

        printf("[FbxLoader] FBX skeleton: %d joints from '%s'\n",
               out.skeleton.jointCount(), path.c_str());
    }

    // ── Decode textures to CPU buffers ───────────────────────────────────
    if (loadTextures) {
        std::unordered_map<ufbx_texture*, int> texMap;

        std::string fbxDir;
        auto lastSlash = path.find_last_of("/\\");
        if (lastSlash != std::string::npos)
            fbxDir = path.substr(0, lastSlash + 1);

        auto decodeTex = [&](ufbx_texture* tex) -> int {
            if (!tex) return -1;
            auto it = texMap.find(tex);
            if (it != texMap.end()) return it->second;

            int w = 0, h = 0, comp = 0;
            unsigned char* pixels = nullptr;

            if (tex->content.size > 0) {
                pixels = stbi_load_from_memory(
                    (const unsigned char*)tex->content.data, (int)tex->content.size,
                    &w, &h, &comp, 4);
            }
            if (!pixels && tex->relative_filename.length > 0) {
                std::string texPath = fbxDir + std::string(
                    tex->relative_filename.data, tex->relative_filename.length);
                pixels = stbi_load(texPath.c_str(), &w, &h, &comp, 4);
            }
            if (!pixels && tex->absolute_filename.length > 0) {
                std::string texPath(tex->absolute_filename.data, tex->absolute_filename.length);
                pixels = stbi_load(texPath.c_str(), &w, &h, &comp, 4);
            }

            if (!pixels || w <= 0 || h <= 0) {
                if (pixels) stbi_image_free(pixels);
                return -1;
            }

            TextureImportData tid;
            tid.pixels.assign(pixels, pixels + (size_t)w * h * 4);
            tid.width  = (uint32_t)w;
            tid.height = (uint32_t)h;
            tid.srgb   = true;
            stbi_image_free(pixels);

            int idx = (int)out.textures.size();
            texMap[tex] = idx;
            out.textures.push_back(std::move(tid));
            return idx;
        };

        // Map material textures
        for (size_t mati = 0; mati < scene->materials.count && mati < out.materials.size(); mati++) {
            ufbx_material* fm = scene->materials.data[mati];
            MeshMaterial& mat = out.materials[mati];

            mat.baseColorTex = decodeTex(fm->pbr.base_color.texture
                ? fm->pbr.base_color.texture : fm->fbx.diffuse_color.texture);
            mat.normalTex = decodeTex(fm->pbr.normal_map.texture
                ? fm->pbr.normal_map.texture : fm->fbx.normal_map.texture);
            mat.metallicRoughnessTex = decodeTex(fm->pbr.metalness.texture);
            mat.emissiveTex = decodeTex(fm->pbr.emission_color.texture);
            mat.occlusionTex = decodeTex(fm->pbr.ambient_occlusion.texture);
            if (mat.blendMode == BlendMode::AlphaBlend) {
                mat.opacityTex = decodeTex(fm->pbr.opacity.texture);
                if (mat.opacityTex < 0) {
                    if (fm->fbx.transparency_color.texture)
                        mat.opacityTex = decodeTex(fm->fbx.transparency_color.texture);
                    else if (fm->fbx.transparency_factor.texture)
                        mat.opacityTex = decodeTex(fm->fbx.transparency_factor.texture);
                }
                if (mat.opacityTex >= 0)
                    printf("[FbxLoader] Material '%s' -> opacity texture (idx %d)\n",
                           mat.name.c_str(), mat.opacityTex);
            }

            auto tag = [&](int idx, TextureType t) {
                if (idx >= 0 && idx < (int)out.textures.size())
                    out.textures[idx].type = t;
            };
            tag(mat.baseColorTex,         TextureType::baseColor);
            tag(mat.normalTex,            TextureType::normal);
            tag(mat.metallicRoughnessTex, TextureType::metallicRoughness);
            tag(mat.emissiveTex,          TextureType::emissive);
            tag(mat.occlusionTex,         TextureType::occlusion);
            tag(mat.opacityTex,           TextureType::opacity);
        }

        int ufbxTexCount = (int)out.textures.size();
        if (ufbxTexCount > 0)
            printf("[FbxLoader] FBX decoded %d textures from '%s'\n",
                   ufbxTexCount, path.c_str());

        // ── CC5 JSON sidecar: supplement missing textures from JSON paths ──
        if (!cc5Map.empty()) {
            auto resolveCC5Path = [&](const std::string& relPath) -> std::string {
                std::string p = relPath;
                if (p.size() >= 2 && p[0] == '.' && (p[1] == '/' || p[1] == '\\'))
                    p = p.substr(2);
                return fbxDir + p;
            };

            auto decodeFromPath = [&](const std::string& absPath, bool srgb = true) -> int {
                int w = 0, h = 0, comp = 0;
                unsigned char* pixels = stbi_load(absPath.c_str(), &w, &h, &comp, 4);
                if (!pixels || w <= 0 || h <= 0) {
                    if (pixels) stbi_image_free(pixels);
                    return -1;
                }
                TextureImportData tid;
                tid.pixels.assign(pixels, pixels + (size_t)w * h * 4);
                tid.width  = (uint32_t)w;
                tid.height = (uint32_t)h;
                tid.srgb   = srgb;
                stbi_image_free(pixels);
                int idx = (int)out.textures.size();
                out.textures.push_back(std::move(tid));
                return idx;
            };

            auto packMetalRough = [&](const std::string& metalPath,
                                      const std::string& roughPath) -> int {
                int mw = 0, mh = 0, mc = 0, rw = 0, rh = 0, rc = 0;
                unsigned char* metalPx = stbi_load(metalPath.c_str(), &mw, &mh, &mc, 1);
                unsigned char* roughPx = stbi_load(roughPath.c_str(), &rw, &rh, &rc, 1);
                if (!metalPx && !roughPx) return -1;

                int w = metalPx ? mw : rw;
                int h = metalPx ? mh : rh;
                TextureImportData tid;
                tid.pixels.resize(w * h * 4);
                for (int i = 0; i < w * h; i++) {
                    tid.pixels[i * 4 + 0] = 0;
                    tid.pixels[i * 4 + 1] = roughPx && i < rw * rh ? roughPx[i] : 255;
                    tid.pixels[i * 4 + 2] = metalPx && i < mw * mh ? metalPx[i] : 0;
                    tid.pixels[i * 4 + 3] = 255;
                }
                if (metalPx) stbi_image_free(metalPx);
                if (roughPx) stbi_image_free(roughPx);

                tid.width  = (uint32_t)w;
                tid.height = (uint32_t)h;
                tid.srgb   = false;
                int idx = (int)out.textures.size();
                out.textures.push_back(std::move(tid));
                return idx;
            };

            auto tag = [&](int idx, TextureType t) {
                if (idx >= 0 && idx < (int)out.textures.size())
                    out.textures[idx].type = t;
            };

            int cc5TexLoaded = 0;
            for (auto& mat : out.materials) {
                auto it = cc5Map.find(mat.name);
                if (it == cc5Map.end() || it->second.texturePaths.empty()) continue;
                const auto& tp = it->second.texturePaths;

                if (mat.baseColorTex < 0) {
                    auto bc = tp.find("Base Color");
                    if (bc != tp.end()) {
                        mat.baseColorTex = decodeFromPath(resolveCC5Path(bc->second));
                        if (mat.baseColorTex >= 0) { tag(mat.baseColorTex, TextureType::baseColor); cc5TexLoaded++; }
                    }
                }

                if (mat.normalTex < 0) {
                    auto nm = tp.find("Normal");
                    if (nm != tp.end()) {
                        mat.normalTex = decodeFromPath(resolveCC5Path(nm->second), false);
                        if (mat.normalTex >= 0) { tag(mat.normalTex, TextureType::normal); cc5TexLoaded++; }
                    }
                }

                if (mat.metallicRoughnessTex < 0) {
                    auto metal = tp.find("Metallic");
                    auto rough = tp.find("Roughness");
                    if (metal != tp.end() || rough != tp.end()) {
                        std::string mp = metal != tp.end() ? resolveCC5Path(metal->second) : "";
                        std::string rp = rough != tp.end() ? resolveCC5Path(rough->second) : "";
                        mat.metallicRoughnessTex = packMetalRough(mp, rp);
                        if (mat.metallicRoughnessTex >= 0) {
                            tag(mat.metallicRoughnessTex, TextureType::metallicRoughness);
                            mat.metallic  = 1.0f;
                            mat.roughness = 1.0f;
                            cc5TexLoaded++;
                        }
                    }
                }

                if (mat.occlusionTex < 0) {
                    auto ao = tp.find("AO");
                    if (ao != tp.end()) {
                        mat.occlusionTex = decodeFromPath(resolveCC5Path(ao->second), false);
                        if (mat.occlusionTex >= 0) { tag(mat.occlusionTex, TextureType::occlusion); cc5TexLoaded++; }
                    }
                }

                if (mat.blendMode == BlendMode::AlphaBlend && mat.opacityTex < 0) {
                    auto op = tp.find("Opacity");
                    if (op != tp.end()) {
                        mat.opacityTex = decodeFromPath(resolveCC5Path(op->second), false);
                        if (mat.opacityTex >= 0) { tag(mat.opacityTex, TextureType::opacity); cc5TexLoaded++; }
                    }
                }
            }

            if (cc5TexLoaded > 0)
                printf("[FbxLoader] CC5 JSON: loaded %d supplemental textures\n", cc5TexLoaded);
        }
    }

    // ── CC5 JSON sidecar: apply RootColor for materials without diffuse ──
    for (auto& mat : out.materials) {
        if (mat.baseColorTex >= 0) continue;
        auto it = cc5Map.find(mat.name);
        if (it == cc5Map.end() || !it->second.hasRootColor) continue;
        const auto& info = it->second;
        mat.baseColor = glm::vec4(info.rootColor, 1.0f);
        printf("[FbxLoader] Material '%s' -- RootColor from CC5 JSON: (%.3f, %.3f, %.3f)\n",
               mat.name.c_str(), info.rootColor.r, info.rootColor.g, info.rootColor.b);
    }

    // Debug: print vertex bounds
    {
        glm::vec3 lo(1e9f), hi(-1e9f);
        for (const auto& v : allVertices) {
            lo = glm::min(lo, v.pos);
            hi = glm::max(hi, v.pos);
        }
        printf("[FbxLoader] FBX bounds: (%.3f,%.3f,%.3f) to (%.3f,%.3f,%.3f)\n",
               lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
    }

    printf("[FbxLoader] FBX parsed '%s': %d verts, %d indices, %d submeshes, %d materials\n",
           path.c_str(), (int)allVertices.size(), (int)allIndices.size(),
           (int)out.submeshes.size(), (int)out.materials.size());

    ufbx_free_scene(scene);
    return true;
}

} // namespace sv
