// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "stb_image.h"   // declarations only — impl in VkTexture.cpp

#include "GltfLoader.h"
#include "../MorphTargetTypes.h"   // MAX_VERTEX_SHADER_MORPH_TARGETS

#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <cstring>

namespace sv {

bool loadGltf(const std::string& path, bool loadTextures, MeshImportData& out)
{
    tinygltf::Model gltfModel;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    if (loadTextures) {
        loader.SetImageLoader(
            [](tinygltf::Image* img, const int, std::string* outErr, std::string*,
               int, int, const unsigned char* bytes, int size, void*) -> bool {
                int w, h, comp;
                unsigned char* px = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4);
                if (!px) {
                    if (outErr) *outErr = "stb_image decode failed";
                    return false;
                }
                img->width      = w;
                img->height     = h;
                img->component  = 4;
                img->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
                img->image.assign(px, px + (size_t)w * h * 4);
                stbi_image_free(px);
                return true;
            },
            nullptr);
    } else {
        loader.SetImageLoader(
            [](tinygltf::Image*, const int, std::string*, std::string*,
               int, int, const unsigned char*, int, void*) { return true; },
            nullptr);
    }

    bool ok = false;
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".glb") {
        ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, path);
    } else {
        ok = loader.LoadASCIIFromFile(&gltfModel, &err, &warn, path);
    }

    if (!warn.empty()) printf("[GltfLoader] Warning: %s\n", warn.c_str());
    if (!err.empty())  printf("[GltfLoader] Error: %s\n", err.c_str());
    if (!ok) {
        printf("[GltfLoader] Failed to load: %s\n", path.c_str());
        return false;
    }

    auto& allVertices = out.vertices;
    auto& allIndices  = out.indices;
    size_t morphTargetCount = 0;

    // ── Materials ────────────────────────────────────────────────────────
    for (const auto& gltfMat : gltfModel.materials) {
        MeshMaterial mat;
        const auto& pbr = gltfMat.pbrMetallicRoughness;
        mat.baseColor = glm::vec4(
            (float)pbr.baseColorFactor[0],
            (float)pbr.baseColorFactor[1],
            (float)pbr.baseColorFactor[2],
            (float)pbr.baseColorFactor[3]);
        mat.metallic  = (float)pbr.metallicFactor;
        mat.roughness = (float)pbr.roughnessFactor;
        out.materials.push_back(mat);
    }

    // ── Process all meshes ───────────────────────────────────────────────
    for (const auto& mesh : gltfModel.meshes) {
        for (const auto& primitive : mesh.primitives) {
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1) continue;

            // Positions (required)
            auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end()) continue;

            const auto& posAccessor = gltfModel.accessors[posIt->second];
            const auto& posView = gltfModel.bufferViews[posAccessor.bufferView];
            const auto& posBuffer = gltfModel.buffers[posView.buffer];
            const float* posData = reinterpret_cast<const float*>(
                &posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);
            int posStride = posView.byteStride ? (int)(posView.byteStride / sizeof(float)) : 3;

            size_t vertexOffset = allVertices.size();
            size_t vertCount = posAccessor.count;
            allVertices.resize(vertexOffset + vertCount);

            for (size_t i = 0; i < vertCount; i++) {
                allVertices[vertexOffset + i].pos = glm::vec3(
                    posData[i * posStride + 0],
                    posData[i * posStride + 1],
                    posData[i * posStride + 2]);
                allVertices[vertexOffset + i].normal = glm::vec3(0, 1, 0);
                allVertices[vertexOffset + i].uv = glm::vec2(0);
            }

            // Normals
            auto normIt = primitive.attributes.find("NORMAL");
            if (normIt != primitive.attributes.end()) {
                const auto& normAccessor = gltfModel.accessors[normIt->second];
                const auto& normView = gltfModel.bufferViews[normAccessor.bufferView];
                const auto& normBuffer = gltfModel.buffers[normView.buffer];
                const float* normData = reinterpret_cast<const float*>(
                    &normBuffer.data[normView.byteOffset + normAccessor.byteOffset]);
                int normStride = normView.byteStride ? (int)(normView.byteStride / sizeof(float)) : 3;

                for (size_t i = 0; i < std::min(normAccessor.count, vertCount); i++) {
                    allVertices[vertexOffset + i].normal = glm::vec3(
                        normData[i * normStride + 0],
                        normData[i * normStride + 1],
                        normData[i * normStride + 2]);
                }
            }

            // UVs
            auto uvIt = primitive.attributes.find("TEXCOORD_0");
            if (uvIt != primitive.attributes.end()) {
                const auto& uvAccessor = gltfModel.accessors[uvIt->second];
                const auto& uvView = gltfModel.bufferViews[uvAccessor.bufferView];
                const auto& uvBuffer = gltfModel.buffers[uvView.buffer];
                const float* uvData = reinterpret_cast<const float*>(
                    &uvBuffer.data[uvView.byteOffset + uvAccessor.byteOffset]);
                int uvStride = uvView.byteStride ? (int)(uvView.byteStride / sizeof(float)) : 2;

                for (size_t i = 0; i < std::min(uvAccessor.count, vertCount); i++) {
                    allVertices[vertexOffset + i].uv = glm::vec2(
                        uvData[i * uvStride + 0],
                        uvData[i * uvStride + 1]);
                }
            }

            // Joints (JOINTS_0)
            auto jointIt = primitive.attributes.find("JOINTS_0");
            if (jointIt != primitive.attributes.end()) {
                const auto& jAcc  = gltfModel.accessors[jointIt->second];
                const auto& jView = gltfModel.bufferViews[jAcc.bufferView];
                const auto& jBuf  = gltfModel.buffers[jView.buffer];
                const uint8_t* jRaw = &jBuf.data[jView.byteOffset + jAcc.byteOffset];

                for (size_t i = 0; i < std::min(jAcc.count, vertCount); i++) {
                    glm::uvec4 j{0};
                    if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        int stride = jView.byteStride ? (int)jView.byteStride : 4;
                        const uint8_t* p = jRaw + i * stride;
                        j = {p[0], p[1], p[2], p[3]};
                    } else if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        int stride = jView.byteStride ? (int)jView.byteStride : 8;
                        const uint16_t* p = reinterpret_cast<const uint16_t*>(jRaw + i * stride);
                        j = {p[0], p[1], p[2], p[3]};
                    }
                    allVertices[vertexOffset + i].joints = j;
                }
            }

            // Weights (WEIGHTS_0)
            auto weightIt = primitive.attributes.find("WEIGHTS_0");
            if (weightIt != primitive.attributes.end()) {
                const auto& wAcc  = gltfModel.accessors[weightIt->second];
                const auto& wView = gltfModel.bufferViews[wAcc.bufferView];
                const auto& wBuf  = gltfModel.buffers[wView.buffer];
                const float* wData = reinterpret_cast<const float*>(
                    &wBuf.data[wView.byteOffset + wAcc.byteOffset]);
                int wStride = wView.byteStride ? (int)(wView.byteStride / sizeof(float)) : 4;

                for (size_t i = 0; i < std::min(wAcc.count, vertCount); i++) {
                    allVertices[vertexOffset + i].weights = glm::vec4(
                        wData[i * wStride + 0],
                        wData[i * wStride + 1],
                        wData[i * wStride + 2],
                        wData[i * wStride + 3]);
                }
            }

            // Morph targets
            if (!primitive.targets.empty()) {
                size_t numTargets = std::min(primitive.targets.size(), (size_t)MAX_VERTEX_SHADER_MORPH_TARGETS);
                if (morphTargetCount == 0) {
                    morphTargetCount = numTargets;
                    out.morphPosDeltas.resize(numTargets);
                    out.morphNormDeltas.resize(numTargets);
                }
                size_t totalVerts = vertexOffset + vertCount;
                for (size_t t = 0; t < morphTargetCount; t++) {
                    out.morphPosDeltas[t].resize(totalVerts, glm::vec3(0.0f));
                    out.morphNormDeltas[t].resize(totalVerts, glm::vec3(0.0f));
                }
                for (size_t t = 0; t < numTargets; t++) {
                    const auto& target = primitive.targets[t];
                    auto tPosIt = target.find("POSITION");
                    if (tPosIt != target.end() &&
                        tPosIt->second >= 0 && tPosIt->second < (int)gltfModel.accessors.size()) {
                        const auto& tAcc = gltfModel.accessors[tPosIt->second];
                        if (tAcc.bufferView >= 0 && tAcc.bufferView < (int)gltfModel.bufferViews.size()) {
                            const auto& tView = gltfModel.bufferViews[tAcc.bufferView];
                            if (tView.buffer >= 0 && tView.buffer < (int)gltfModel.buffers.size()) {
                                const auto& tBuf = gltfModel.buffers[tView.buffer];
                                size_t dataOffset = tView.byteOffset + tAcc.byteOffset;
                                if (dataOffset < tBuf.data.size()) {
                                    const float* tData = reinterpret_cast<const float*>(&tBuf.data[dataOffset]);
                                    int tStride = tView.byteStride ? (int)(tView.byteStride / sizeof(float)) : 3;
                                    for (size_t i = 0; i < std::min(tAcc.count, vertCount); i++)
                                        out.morphPosDeltas[t][vertexOffset + i] = glm::vec3(
                                            tData[i * tStride], tData[i * tStride + 1], tData[i * tStride + 2]);
                                }
                            }
                        }
                    }
                    auto tNormIt = target.find("NORMAL");
                    if (tNormIt != target.end() &&
                        tNormIt->second >= 0 && tNormIt->second < (int)gltfModel.accessors.size()) {
                        const auto& tAcc = gltfModel.accessors[tNormIt->second];
                        if (tAcc.bufferView >= 0 && tAcc.bufferView < (int)gltfModel.bufferViews.size()) {
                            const auto& tView = gltfModel.bufferViews[tAcc.bufferView];
                            if (tView.buffer >= 0 && tView.buffer < (int)gltfModel.buffers.size()) {
                                const auto& tBuf = gltfModel.buffers[tView.buffer];
                                size_t dataOffset = tView.byteOffset + tAcc.byteOffset;
                                if (dataOffset < tBuf.data.size()) {
                                    const float* tData = reinterpret_cast<const float*>(&tBuf.data[dataOffset]);
                                    int tStride = tView.byteStride ? (int)(tView.byteStride / sizeof(float)) : 3;
                                    for (size_t i = 0; i < std::min(tAcc.count, vertCount); i++)
                                        out.morphNormDeltas[t][vertexOffset + i] = glm::vec3(
                                            tData[i * tStride], tData[i * tStride + 1], tData[i * tStride + 2]);
                                }
                            }
                        }
                    }
                }
            }

            // Indices
            SubMesh sub;
            sub.indexOffset = (uint32_t)allIndices.size();
            sub.materialIndex = primitive.material;

            if (primitive.indices >= 0) {
                const auto& idxAccessor = gltfModel.accessors[primitive.indices];
                const auto& idxView = gltfModel.bufferViews[idxAccessor.bufferView];
                const auto& idxBuffer = gltfModel.buffers[idxView.buffer];
                const uint8_t* idxData = &idxBuffer.data[idxView.byteOffset + idxAccessor.byteOffset];

                size_t idxStart = allIndices.size();
                allIndices.resize(idxStart + idxAccessor.count);

                switch (idxAccessor.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                        const uint16_t* p = reinterpret_cast<const uint16_t*>(idxData);
                        for (size_t i = 0; i < idxAccessor.count; i++)
                            allIndices[idxStart + i] = (uint32_t)vertexOffset + p[i];
                        break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                        const uint32_t* p = reinterpret_cast<const uint32_t*>(idxData);
                        for (size_t i = 0; i < idxAccessor.count; i++)
                            allIndices[idxStart + i] = (uint32_t)vertexOffset + p[i];
                        break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                        for (size_t i = 0; i < idxAccessor.count; i++)
                            allIndices[idxStart + i] = (uint32_t)vertexOffset + idxData[i];
                        break;
                    }
                }
                sub.indexCount = (uint32_t)idxAccessor.count;
            } else {
                size_t idxStart = allIndices.size();
                allIndices.resize(idxStart + vertCount);
                for (size_t i = 0; i < vertCount; i++)
                    allIndices[idxStart + i] = (uint32_t)(vertexOffset + i);
                sub.indexCount = (uint32_t)vertCount;
            }

            out.submeshes.push_back(sub);
        }
    }

    if (allVertices.empty()) {
        printf("[GltfLoader] No geometry found in: %s\n", path.c_str());
        return false;
    }

    out.morphTargetCount = morphTargetCount;

    // ── Morph target names + default weights ─────────────────────────────
    if (morphTargetCount > 0 && !gltfModel.meshes.empty()) {
        out.morphTargetNames.resize(morphTargetCount);
        out.morphTargetDefaultWeights.resize(morphTargetCount, 0.0f);

        const auto& meshExtras = gltfModel.meshes[0].extras;
        if (meshExtras.Has("targetNames") && meshExtras.Get("targetNames").IsArray()) {
            const auto& names = meshExtras.Get("targetNames");
            for (size_t t = 0; t < morphTargetCount && t < names.ArrayLen(); t++) {
                if (names.Get((int)t).IsString())
                    out.morphTargetNames[t] = names.Get((int)t).Get<std::string>();
            }
        }
        const auto& weights = gltfModel.meshes[0].weights;
        for (size_t t = 0; t < morphTargetCount && t < weights.size(); t++) {
            out.morphTargetDefaultWeights[t] = (float)weights[t];
        }
    }

    // ── Parse skin/skeleton (first skin only) ────────────────────────────
    if (!gltfModel.skins.empty()) {
        const auto& skin = gltfModel.skins[0];

        std::unordered_map<int, int> nodeToJoint;
        for (int j = 0; j < (int)skin.joints.size(); j++)
            nodeToJoint[skin.joints[j]] = j;

        out.skeleton.joints.resize(skin.joints.size());

        // Inverse bind matrices
        if (skin.inverseBindMatrices >= 0) {
            const auto& ibmAcc  = gltfModel.accessors[skin.inverseBindMatrices];
            const auto& ibmView = gltfModel.bufferViews[ibmAcc.bufferView];
            const auto& ibmBuf  = gltfModel.buffers[ibmView.buffer];
            const float* ibmData = reinterpret_cast<const float*>(
                &ibmBuf.data[ibmView.byteOffset + ibmAcc.byteOffset]);

            for (size_t j = 0; j < std::min(ibmAcc.count, skin.joints.size()); j++) {
                memcpy(&out.skeleton.joints[j].inverseBindMatrix, ibmData + j * 16, 64);
            }
        }

        // Joint names, rest poses, parent indices
        for (int j = 0; j < (int)skin.joints.size(); j++) {
            int nodeIdx = skin.joints[j];
            const auto& node = gltfModel.nodes[nodeIdx];
            out.skeleton.joints[j].name = node.name;
            out.skeleton.joints[j].parent = -1;

            if (node.translation.size() == 3) {
                out.skeleton.joints[j].restTranslation = glm::vec3(
                    (float)node.translation[0],
                    (float)node.translation[1],
                    (float)node.translation[2]);
            }
            if (node.rotation.size() == 4) {
                out.skeleton.joints[j].restRotation = glm::quat(
                    (float)node.rotation[3],
                    (float)node.rotation[0],
                    (float)node.rotation[1],
                    (float)node.rotation[2]);
            }
            if (node.scale.size() == 3) {
                out.skeleton.joints[j].restScale = glm::vec3(
                    (float)node.scale[0],
                    (float)node.scale[1],
                    (float)node.scale[2]);
            }
        }

        // Parent indices by walking node tree
        for (int j = 0; j < (int)skin.joints.size(); j++) {
            int nodeIdx = skin.joints[j];
            const auto& node = gltfModel.nodes[nodeIdx];
            for (int childIdx : node.children) {
                auto it = nodeToJoint.find(childIdx);
                if (it != nodeToJoint.end())
                    out.skeleton.joints[it->second].parent = j;
            }
        }

        printf("[GltfLoader] Skeleton: %d joints from '%s'\n",
               out.skeleton.jointCount(), path.c_str());
    }

    // ── Decode textures to CPU buffers ───────────────────────────────────
    if (loadTextures && !gltfModel.images.empty()) {
        std::unordered_map<int, int> imgToTex;

        for (int i = 0; i < (int)gltfModel.images.size(); i++) {
            auto& img = gltfModel.images[i];
            if (img.image.empty() || img.width <= 0 || img.height <= 0) continue;

            TextureImportData tid;
            tid.pixels = std::move(img.image);
            tid.width  = (uint32_t)img.width;
            tid.height = (uint32_t)img.height;
            tid.srgb   = true;

            imgToTex[i] = (int)out.textures.size();
            out.textures.push_back(std::move(tid));
        }

        auto resolve = [&](int texIndex) -> int {
            if (texIndex < 0 || texIndex >= (int)gltfModel.textures.size()) return -1;
            int src = gltfModel.textures[texIndex].source;
            auto it = imgToTex.find(src);
            return (it != imgToTex.end()) ? it->second : -1;
        };

        for (size_t mi = 0; mi < gltfModel.materials.size() && mi < out.materials.size(); mi++) {
            const auto& gm = gltfModel.materials[mi];
            auto& mat = out.materials[mi];

            mat.baseColorTex         = resolve(gm.pbrMetallicRoughness.baseColorTexture.index);
            mat.normalTex            = resolve(gm.normalTexture.index);
            mat.metallicRoughnessTex = resolve(gm.pbrMetallicRoughness.metallicRoughnessTexture.index);
            mat.emissiveTex          = resolve(gm.emissiveTexture.index);
            mat.occlusionTex         = resolve(gm.occlusionTexture.index);

            auto tag = [&](int idx, TextureType t) {
                if (idx >= 0 && idx < (int)out.textures.size())
                    out.textures[idx].type = t;
            };
            tag(mat.baseColorTex,         TextureType::baseColor);
            tag(mat.normalTex,            TextureType::normal);
            tag(mat.metallicRoughnessTex, TextureType::metallicRoughness);
            tag(mat.emissiveTex,          TextureType::emissive);
            tag(mat.occlusionTex,         TextureType::occlusion);
        }

        printf("[GltfLoader] %d textures from '%s'\n",
               (int)out.textures.size(), path.c_str());
    }

    printf("[GltfLoader] Parsed '%s': %d verts, %d indices, %d submeshes, %d materials\n",
           path.c_str(), (int)allVertices.size(), (int)allIndices.size(),
           (int)out.submeshes.size(), (int)out.materials.size());

    return true;
}

} // namespace sv
