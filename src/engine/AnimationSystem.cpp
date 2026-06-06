// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#ifdef _WIN32
#define NOMINMAX
#endif

// tinygltf declarations only — implementation compiled in VkMesh.cpp
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "AnimationSystem.h"
#include "BlendTree.h"   // AnimBodyLayer, IBlendNode
#include "vk/VkMesh.h"  // SkeletonData, SkeletonJoint

#include <ufbx.h>

#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/runtime/ik_two_bone_job.h>
#include <ozz/animation/runtime/ik_aim_job.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/simd_quaternion.h>
#include <ozz/base/memory/allocator.h>
#include <ozz/base/span.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <algorithm>

namespace sv {

// ── Helpers ─────────────────────────────────────────────────────────

// Convert ozz SIMD 4x4 matrix to glm::mat4.
// Both are column-major, 64 bytes (4 columns x 4 floats).
static glm::mat4 ozzToGlm(const ozz::math::Float4x4& m) {
    static_assert(sizeof(ozz::math::Float4x4) == 64, "Unexpected Float4x4 size");
    static_assert(sizeof(glm::mat4) == 64, "Unexpected mat4 size");
    glm::mat4 r;
    std::memcpy(&r, &m, 64);
    return r;
}

// Recursively build ozz joint hierarchy from flat SkeletonData.
// Tracks depth-first ordering in ozzToDataIndex.
static void buildJointTree(
    const SkeletonData& data,
    int dataJointIdx,
    ozz::animation::offline::RawSkeleton::Joint& ozzJoint,
    const std::vector<std::vector<int>>& childrenMap,
    std::vector<int>& ozzToDataIndex)
{
    ozzToDataIndex.push_back(dataJointIdx);

    const auto& j = data.joints[dataJointIdx];
    ozzJoint.name = j.name.c_str();
    ozzJoint.transform.translation = ozz::math::Float3(
        j.restTranslation.x, j.restTranslation.y, j.restTranslation.z);
    ozzJoint.transform.rotation = ozz::math::Quaternion(
        j.restRotation.x, j.restRotation.y, j.restRotation.z, j.restRotation.w);
    ozzJoint.transform.scale = ozz::math::Float3(
        j.restScale.x, j.restScale.y, j.restScale.z);

    const auto& children = childrenMap[dataJointIdx];
    ozzJoint.children.resize(children.size());
    for (size_t c = 0; c < children.size(); c++)
        buildJointTree(data, children[c], ozzJoint.children[c], childrenMap, ozzToDataIndex);
}

// ── buildSkeleton ───────────────────────────────────────────────────

SkeletonHandle buildSkeleton(const SkeletonData& data) {
    SkeletonHandle handle;
    if (data.empty()) return handle;

    // Build children map: parent index -> list of child indices
    std::vector<std::vector<int>> childrenMap(data.joints.size());
    std::vector<int> roots;
    for (int i = 0; i < (int)data.joints.size(); i++) {
        int p = data.joints[i].parent;
        if (p < 0)
            roots.push_back(i);
        else
            childrenMap[p].push_back(i);
    }

    // Build ozz RawSkeleton (hierarchical) and track joint ordering
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(roots.size());
    for (size_t r = 0; r < roots.size(); r++)
        buildJointTree(data, roots[r], raw.roots[r], childrenMap, handle.ozzToDataIndex);

    if (!raw.Validate()) {
        printf("[AnimationSystem] RawSkeleton validation failed\n");
        handle.ozzToDataIndex.clear();
        return handle;
    }

    // Build runtime skeleton
    ozz::animation::offline::SkeletonBuilder builder;
    auto built = builder(raw);
    if (!built) {
        printf("[AnimationSystem] SkeletonBuilder failed\n");
        handle.ozzToDataIndex.clear();
        return handle;
    }

    // ozz allocates Skeleton via a custom heap allocator (malloc + alignment
    // header in ozz/base/memory/allocator.cc). std::shared_ptr's default
    // deleter would call ::operator delete on that pointer and corrupt the
    // heap. Route destruction back through ozz::Delete which calls the
    // matching Deallocate().
    handle.skeleton = std::shared_ptr<ozz::animation::Skeleton>(
        built.release(),
        [](ozz::animation::Skeleton* s) { ozz::Delete(s); });
    printf("[AnimationSystem] Built skeleton: %d joints (%d SoA)\n",
           handle.jointCount(), handle.soaCount());
    return handle;
}

// ── loadGltfAnimations ──────────────────────────────────────────────

std::vector<AnimationClip> loadGltfAnimations(
    const std::string& path,
    const SkeletonHandle& skeleton)
{
    std::vector<AnimationClip> clips;
    if (!skeleton) return clips;

    tinygltf::Model gltfModel;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    // No-op image loader — we only need animation data
    loader.SetImageLoader(
        [](tinygltf::Image*, const int, std::string*, std::string*,
           int, int, const unsigned char*, int, void*) { return true; },
        nullptr);

    bool ok = false;
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".glb")
        ok = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, path);
    else
        ok = loader.LoadASCIIFromFile(&gltfModel, &err, &warn, path);

    if (!ok) {
        if (!err.empty()) printf("[AnimationSystem] glTF error: %s\n", err.c_str());
        return clips;
    }
    if (gltfModel.animations.empty()) return clips;

    // Build node name -> ozz joint index map
    auto jointNames = skeleton.skeleton->joint_names();
    std::unordered_map<std::string, int> nameToOzzIdx;
    for (int i = 0; i < skeleton.jointCount(); i++)
        nameToOzzIdx[jointNames[i]] = i;

    // Build glTF node index -> ozz joint index map
    std::unordered_map<int, int> nodeToOzzIdx;
    for (int n = 0; n < (int)gltfModel.nodes.size(); n++) {
        auto it = nameToOzzIdx.find(gltfModel.nodes[n].name);
        if (it != nameToOzzIdx.end())
            nodeToOzzIdx[n] = it->second;
    }

    // Process each glTF animation
    for (const auto& gltfAnim : gltfModel.animations) {
        ozz::animation::offline::RawAnimation raw;
        raw.name = gltfAnim.name.c_str();
        raw.tracks.resize(skeleton.jointCount());
        raw.duration = 0.0f;

        for (const auto& channel : gltfAnim.channels) {
            auto jit = nodeToOzzIdx.find(channel.target_node);
            if (jit == nodeToOzzIdx.end()) continue;
            int ozzJointIdx = jit->second;

            const auto& sampler = gltfAnim.samplers[channel.sampler];

            // Read input (time) keyframes
            const auto& inputAcc  = gltfModel.accessors[sampler.input];
            const auto& inputView = gltfModel.bufferViews[inputAcc.bufferView];
            const auto& inputBuf  = gltfModel.buffers[inputView.buffer];
            const float* times = reinterpret_cast<const float*>(
                &inputBuf.data[inputView.byteOffset + inputAcc.byteOffset]);

            // Read output (value) keyframes
            const auto& outputAcc  = gltfModel.accessors[sampler.output];
            const auto& outputView = gltfModel.bufferViews[outputAcc.bufferView];
            const auto& outputBuf  = gltfModel.buffers[outputView.buffer];
            const float* values = reinterpret_cast<const float*>(
                &outputBuf.data[outputView.byteOffset + outputAcc.byteOffset]);

            size_t keyCount = inputAcc.count;
            if (keyCount > 0)
                raw.duration = std::max(raw.duration, times[keyCount - 1]);

            auto& track = raw.tracks[ozzJointIdx];

            if (channel.target_path == "translation") {
                track.translations.resize(keyCount);
                for (size_t k = 0; k < keyCount; k++) {
                    track.translations[k].time = times[k];
                    track.translations[k].value = ozz::math::Float3(
                        values[k * 3 + 0], values[k * 3 + 1], values[k * 3 + 2]);
                }
            } else if (channel.target_path == "rotation") {
                track.rotations.resize(keyCount);
                for (size_t k = 0; k < keyCount; k++) {
                    track.rotations[k].time = times[k];
                    // glTF: [x, y, z, w] → ozz: Quaternion(x, y, z, w)
                    track.rotations[k].value = ozz::math::Quaternion(
                        values[k * 4 + 0], values[k * 4 + 1],
                        values[k * 4 + 2], values[k * 4 + 3]);
                }
            } else if (channel.target_path == "scale") {
                track.scales.resize(keyCount);
                for (size_t k = 0; k < keyCount; k++) {
                    track.scales[k].time = times[k];
                    track.scales[k].value = ozz::math::Float3(
                        values[k * 3 + 0], values[k * 3 + 1], values[k * 3 + 2]);
                }
            }
        }

        if (raw.duration <= 0.0f) continue;

        if (!raw.Validate()) {
            printf("[AnimationSystem] RawAnimation '%s' validation failed\n",
                   gltfAnim.name.c_str());
            continue;
        }

        ozz::animation::offline::AnimationBuilder animBuilder;
        auto built = animBuilder(raw);
        if (!built) {
            printf("[AnimationSystem] AnimationBuilder failed for '%s'\n",
                   gltfAnim.name.c_str());
            continue;
        }

        AnimationClip clip;
        clip.name      = gltfAnim.name;
        clip.duration  = raw.duration;
        // Use ozz::Delete deleter — see buildSkeleton() comment above.
        clip.animation = std::shared_ptr<ozz::animation::Animation>(
            built.release(),
            [](ozz::animation::Animation* a) { ozz::Delete(a); });
        clips.push_back(std::move(clip));

        printf("[AnimationSystem] Loaded animation '%s' (%.2fs)\n",
               gltfAnim.name.c_str(), raw.duration);
    }

    return clips;
}

// ── loadFbxAnimations ────────────────────────────────

std::vector<AnimationClip> loadFbxAnimations(
    const std::string& path,
    const SkeletonHandle& skeleton)
{
    std::vector<AnimationClip> clips;
    if (!skeleton) return clips;

    ufbx_load_opts opts = {};
    opts.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
    opts.target_axes.up    = UFBX_COORDINATE_AXIS_POSITIVE_Y;
    opts.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
    opts.space_conversion   = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    opts.target_unit_meters = 1.0f;
    opts.ignore_geometry    = true;
    opts.ignore_embedded   = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) {
        printf("[AnimationSystem] FBX load failed: %s (%s)\n", path.c_str(), error.description.data);
        return clips;
    }

    if (scene->anim_stacks.count == 0) {
        ufbx_free_scene(scene);
        return clips;
    }

    // Map bone node names → ozz joint indices
    auto jointNames = skeleton.skeleton->joint_names();
    std::unordered_map<std::string, int> nameToOzzIdx;
    for (int i = 0; i < skeleton.jointCount(); i++)
        nameToOzzIdx[jointNames[i]] = i;

    // Map FBX scene nodes to ozz joint indices by name
    std::unordered_map<uint32_t, int> nodeToOzzIdx; // ufbx typed_id → ozz index
    for (size_t ni = 0; ni < scene->nodes.count; ni++) {
        ufbx_node* node = scene->nodes.data[ni];
        std::string name(node->name.data, node->name.length);
        auto it = nameToOzzIdx.find(name);
        if (it != nameToOzzIdx.end())
            nodeToOzzIdx[node->typed_id] = it->second;
    }

    constexpr int kSampleRate = 30;

    // Helper: ufbx_transform → glm::mat4 (TRS composition)
    auto xfToMat4 = [](const ufbx_transform& xf) -> glm::mat4 {
        glm::vec3 t((float)xf.translation.x, (float)xf.translation.y, (float)xf.translation.z);
        glm::quat r((float)xf.rotation.w, (float)xf.rotation.x, (float)xf.rotation.y, (float)xf.rotation.z);
        glm::vec3 s((float)xf.scale.x, (float)xf.scale.y, (float)xf.scale.z);
        glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
        m *= glm::mat4_cast(r);
        m = glm::scale(m, s);
        return m;
    };

    // Helper: decompose glm::mat4 → TRS
    auto decompose = [](const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
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

    // Build lookup: typed_id → ufbx_node*
    std::unordered_map<uint32_t, ufbx_node*> nodeById;
    for (size_t ni = 0; ni < scene->nodes.count; ni++)
        nodeById[scene->nodes.data[ni]->typed_id] = scene->nodes.data[ni];

    // Build intermediate parent chains for bones with hierarchy gaps.
    // When a skeleton joint's ozz parent skips non-deforming FBX nodes,
    // animation local transforms must be chained through the intermediates.
    struct IntermediateChain { std::vector<ufbx_node*> nodes; }; // bone→parent order
    std::unordered_map<uint32_t, IntermediateChain> chainMap;

    auto ozzParents = skeleton.skeleton->joint_parents();
    for (auto& [nodeId, ozzIdx] : nodeToOzzIdx) {
        auto nit = nodeById.find(nodeId);
        if (nit == nodeById.end()) continue;
        ufbx_node* node = nit->second;

        int parentOzz = ozzParents[ozzIdx];
        if (parentOzz < 0) continue; // root bone

        std::string parentName = jointNames[parentOzz];
        IntermediateChain chain;
        ufbx_node* cur = node->parent;
        while (cur) {
            std::string curName(cur->name.data, cur->name.length);
            if (curName == parentName) break; // reached ozz parent
            chain.nodes.push_back(cur);
            cur = cur->parent;
        }
        if (!chain.nodes.empty())
            chainMap[nodeId] = std::move(chain);
    }

    for (size_t si = 0; si < scene->anim_stacks.count; si++) {
        ufbx_anim_stack* stack = scene->anim_stacks.data[si];
        double duration = stack->time_end - stack->time_begin;
        if (duration <= 0.0) continue;

        int numSamples = (int)(duration * kSampleRate) + 1;
        if (numSamples < 2) numSamples = 2;

        ozz::animation::offline::RawAnimation raw;
        raw.name = std::string(stack->name.data, stack->name.length).c_str();
        raw.duration = (float)duration;
        raw.tracks.resize(skeleton.jointCount());

        // Sample each bone at regular intervals
        for (auto& [nodeId, ozzIdx] : nodeToOzzIdx) {
            auto nit = nodeById.find(nodeId);
            if (nit == nodeById.end()) continue;
            ufbx_node* node = nit->second;

            auto chainIt = chainMap.find(nodeId);
            bool hasIntermediates = (chainIt != chainMap.end());

            auto& track = raw.tracks[ozzIdx];
            track.translations.resize(numSamples);
            track.rotations.resize(numSamples);
            track.scales.resize(numSamples);

            for (int f = 0; f < numSamples; f++) {
                double t = stack->time_begin + (double)f * duration / (numSamples - 1);
                float localT = (float)(t - stack->time_begin);

                glm::vec3 trans; glm::quat rot; glm::vec3 scl;

                if (hasIntermediates) {
                    // Chain intermediate transforms: parent→child order
                    auto& chain = chainIt->second;
                    glm::mat4 effective(1.0f);
                    for (size_t i = chain.nodes.size(); i > 0; i--) {
                        ufbx_transform ixf = ufbx_evaluate_transform(stack->anim, chain.nodes[i-1], t);
                        effective *= xfToMat4(ixf);
                    }
                    ufbx_transform xf = ufbx_evaluate_transform(stack->anim, node, t);
                    effective *= xfToMat4(xf);
                    decompose(effective, trans, rot, scl);
                } else {
                    ufbx_transform xf = ufbx_evaluate_transform(stack->anim, node, t);
                    trans = glm::vec3((float)xf.translation.x, (float)xf.translation.y, (float)xf.translation.z);
                    rot   = glm::quat((float)xf.rotation.w, (float)xf.rotation.x, (float)xf.rotation.y, (float)xf.rotation.z);
                    scl   = glm::vec3((float)xf.scale.x, (float)xf.scale.y, (float)xf.scale.z);
                }

                track.translations[f].time = localT;
                track.translations[f].value = ozz::math::Float3(trans.x, trans.y, trans.z);

                track.rotations[f].time = localT;
                track.rotations[f].value = ozz::math::Quaternion(rot.x, rot.y, rot.z, rot.w);

                track.scales[f].time = localT;
                track.scales[f].value = ozz::math::Float3(scl.x, scl.y, scl.z);
            }
        }

        if (!raw.Validate()) {
            printf("[AnimationSystem] FBX RawAnimation '%s' validation failed\n",
                   stack->name.data);
            continue;
        }

        ozz::animation::offline::AnimationBuilder animBuilder;
        auto built = animBuilder(raw);
        if (!built) {
            printf("[AnimationSystem] AnimationBuilder failed for FBX '%s'\n",
                   stack->name.data);
            continue;
        }

        AnimationClip clip;
        clip.name      = std::string(stack->name.data, stack->name.length);
        clip.duration  = (float)duration;
        clip.animation = std::shared_ptr<ozz::animation::Animation>(
            built.release(),
            [](ozz::animation::Animation* a) { ozz::Delete(a); });
        clips.push_back(std::move(clip));

        printf("[AnimationSystem] FBX animation '%s' (%.2fs, %d samples)\n",
               clip.name.c_str(), duration, numSamples);
    }

    ufbx_free_scene(scene);
    return clips;
}

// ── AnimationSystem ─────────────────────────────────────────────────

void AnimationSystem::init(VkDevice device, VmaAllocator allocator, uint32_t maxBones) {
    m_device    = device;
    m_allocator = allocator;
    m_maxBones  = maxBones;

    // Create SSBO bone palette (CPU-visible for per-frame upload)
    VkDeviceSize bufSize = (VkDeviceSize)maxBones * sizeof(glm::mat4);
    m_bonePaletteSSBO = VkBuf::create(allocator, bufSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    m_cpuBoneStaging.resize(maxBones, glm::mat4(1.0f));

    // Descriptor set layout: binding 0 = SSBO (vertex stage)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_bonePaletteLayout);

    // Descriptor pool (1 set, 1 SSBO)
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    vkCreateDescriptorPool(device, &poolCI, nullptr, &m_bonePalettePool);

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocCI{};
    allocCI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocCI.descriptorPool     = m_bonePalettePool;
    allocCI.descriptorSetCount = 1;
    allocCI.pSetLayouts        = &m_bonePaletteLayout;
    vkAllocateDescriptorSets(device, &allocCI, &m_bonePaletteDescSet);

    // Point descriptor at the SSBO
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_bonePaletteSSBO.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range  = bufSize;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_bonePaletteDescSet;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &bufferInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    printf("[AnimationSystem] Initialized — %u bone capacity, SSBO %llu bytes\n",
           maxBones, (unsigned long long)bufSize);
}

void AnimationSystem::destroy() {
    if (m_bonePaletteLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_bonePaletteLayout, nullptr);
        m_bonePaletteLayout = VK_NULL_HANDLE;
    }
    if (m_bonePalettePool) {
        vkDestroyDescriptorPool(m_device, m_bonePalettePool, nullptr);
        m_bonePalettePool = VK_NULL_HANDLE;
    }
    m_bonePaletteSSBO.destroy(m_allocator);
    m_cpuBoneStaging.clear();
    m_bonePaletteDescSet = VK_NULL_HANDLE;
}

AnimationInstance AnimationSystem::createInstance(const SkeletonHandle& skeleton) {
    AnimationInstance inst;
    inst.skeleton = skeleton;

    int numJoints = skeleton.jointCount();
    int numSoa    = skeleton.soaCount();

    inst.samplingContext = std::make_unique<ozz::animation::SamplingJob::Context>(numJoints);
    inst.locals.resize(numSoa);
    inst.blended.resize(numSoa);
    inst.models.resize(numJoints);
    // skinningMatrices indexed by SkeletonData joint order (for vertex shader)
    inst.skinningMatrices.resize(skeleton.dataJointCount(), glm::mat4(1.0f));

    return inst;
}

void AnimationSystem::sample(AnimationInstance& inst, const AnimationClip& clip, float timeSeconds) {
    if (!inst.skeleton || !clip.animation) return;

    float ratio = (clip.duration > 0.0f) ? (timeSeconds / clip.duration) : 0.0f;
    ratio = ratio - std::floor(ratio); // wrap to [0, 1]

    ozz::animation::SamplingJob job;
    job.animation = clip.animation.get();
    job.context   = inst.samplingContext.get();
    job.ratio     = ratio;
    job.output    = ozz::make_span(inst.locals);

    if (!job.Run()) {
        printf("[AnimationSystem] SamplingJob failed\n");
    }
}

void AnimationSystem::blend(AnimationInstance& inst, const BlendLayer* layers, int layerCount) {
    if (!inst.skeleton) return;

    if (layerCount == 0) {
        // No layers — use skeleton rest pose
        auto restPose = inst.skeleton.skeleton->joint_rest_poses();
        std::copy(restPose.begin(), restPose.end(), inst.blended.begin());
        return;
    }

    if (layerCount == 1 && !layers[0].jointWeights) {
        // Single layer, no joint mask — sample directly, copy to blended
        sample(inst, *layers[0].clip, layers[0].time);
        std::copy(inst.locals.begin(), inst.locals.end(), inst.blended.begin());
        return;
    }

    // Multi-layer blending: sample each layer, then blend
    std::vector<ozz::vector<ozz::math::SoaTransform>> layerLocals(layerCount);
    std::vector<ozz::animation::BlendingJob::Layer>    ozzLayers(layerCount);

    for (int i = 0; i < layerCount; i++) {
        layerLocals[i].resize(inst.skeleton.soaCount());

        if (layers[i].clip && layers[i].clip->animation) {
            float ratio = (layers[i].clip->duration > 0.0f)
                ? (layers[i].time / layers[i].clip->duration) : 0.0f;
            ratio = ratio - std::floor(ratio);

            // Temporary context per layer (correct caching requires per-layer state)
            ozz::animation::SamplingJob::Context tempCtx;
            tempCtx.Resize(inst.skeleton.jointCount());

            ozz::animation::SamplingJob sampleJob;
            sampleJob.animation = layers[i].clip->animation.get();
            sampleJob.context   = &tempCtx;
            sampleJob.ratio     = ratio;
            sampleJob.output    = ozz::make_span(layerLocals[i]);
            sampleJob.Run();
        }

        ozzLayers[i].transform = ozz::make_span(layerLocals[i]);
        ozzLayers[i].weight    = layers[i].weight;
        if (layers[i].jointWeights)
            ozzLayers[i].joint_weights = ozz::span<const ozz::math::SimdFloat4>(
                layers[i].jointWeights, (size_t)inst.skeleton.soaCount());
    }

    ozz::animation::BlendingJob blendJob;
    blendJob.threshold = ozz::animation::BlendingJob().threshold;
    blendJob.layers    = ozz::make_span(ozzLayers);
    blendJob.rest_pose = inst.skeleton.skeleton->joint_rest_poses();
    blendJob.output    = ozz::make_span(inst.blended);

    if (!blendJob.Run()) {
        printf("[AnimationSystem] BlendingJob failed\n");
    }
}

void AnimationSystem::computeSkinningMatrices(AnimationInstance& inst, const SkeletonData& bindPose) {
    if (!inst.skeleton) return;

    // LocalToModel: blended local transforms -> model-space matrices
    ozz::animation::LocalToModelJob l2mJob;
    l2mJob.skeleton = inst.skeleton.skeleton.get();
    l2mJob.input    = ozz::make_span(inst.blended);
    l2mJob.output   = ozz::make_span(inst.models);

    if (!l2mJob.Run()) {
        printf("[AnimationSystem] LocalToModelJob failed\n");
        return;
    }

    // Compute skinning matrices: model[ozz_i] * inverseBindMatrix[data_i]
    // Output indexed by SkeletonData order (matches vertex JOINTS_0 indices).
    int numJoints = inst.skeleton.jointCount();
    for (int ozzIdx = 0; ozzIdx < numJoints; ozzIdx++) {
        int dataIdx = inst.skeleton.ozzToDataIndex[ozzIdx];
        if (dataIdx < 0 || dataIdx >= (int)bindPose.joints.size()) continue;
        if (dataIdx >= (int)inst.skinningMatrices.size()) continue;

        glm::mat4 model = ozzToGlm(inst.models[ozzIdx]);
        inst.skinningMatrices[dataIdx] = model * bindPose.joints[dataIdx].inverseBindMatrix;
    }
}

// ── Body layer blending ────────────────────────────────────

void AnimationSystem::blendBodyLayers(AnimationInstance& inst,
                                       AnimBodyLayer* layers, int layerCount, float dt) {
    if (!inst.skeleton) return;

    if (layerCount == 0) {
        auto restPose = inst.skeleton.skeleton->joint_rest_poses();
        std::copy(restPose.begin(), restPose.end(), inst.blended.begin());
        return;
    }

    int numSoa = inst.skeleton.soaCount();

    // Evaluate each layer's blend tree and build ozz layers
    std::vector<ozz::vector<ozz::math::SoaTransform>> layerLocals(layerCount);
    std::vector<ozz::animation::BlendingJob::Layer> normalLayers;
    std::vector<ozz::animation::BlendingJob::Layer> additiveLayers;

    for (int i = 0; i < layerCount; i++) {
        layerLocals[i].resize(numSoa);

        if (layers[i].node) {
            layers[i].node->evaluate(dt, ozz::make_span(layerLocals[i]));
        } else {
            // No node — rest pose
            auto rest = inst.skeleton.skeleton->joint_rest_poses();
            std::copy(rest.begin(), rest.end(), layerLocals[i].begin());
        }

        ozz::animation::BlendingJob::Layer ozzLayer;
        ozzLayer.transform = ozz::make_span(layerLocals[i]);
        ozzLayer.weight    = layers[i].weight;
        if (layers[i].jointWeights)
            ozzLayer.joint_weights = ozz::span<const ozz::math::SimdFloat4>(
                layers[i].jointWeights, (size_t)numSoa);

        if (layers[i].additive)
            additiveLayers.push_back(ozzLayer);
        else
            normalLayers.push_back(ozzLayer);
    }

    ozz::animation::BlendingJob blendJob;
    blendJob.threshold       = ozz::animation::BlendingJob().threshold;
    blendJob.layers          = ozz::make_span(normalLayers);
    blendJob.additive_layers = ozz::make_span(additiveLayers);
    blendJob.rest_pose       = inst.skeleton.skeleton->joint_rest_poses();
    blendJob.output          = ozz::make_span(inst.blended);

    if (!blendJob.Run()) {
        printf("[AnimationSystem] BlendingJob (body layers) failed\n");
    }
}

// ── SoA correction helper ─────────────────────────────────

// Apply a quaternion correction to a single joint's rotation in SoA local transforms.
// IK jobs output per-joint correction quaternions; this multiplies one into the
// SoaTransform array at the correct SoA group and lane.
static void applySoACorrection(
    ozz::span<ozz::math::SoaTransform> locals,
    int jointIndex,
    const ozz::math::SimdQuaternion& correction)
{
    const int soaIdx = jointIndex / 4;
    const int lane   = jointIndex % 4;

    auto& rot = locals[soaIdx].rotation;

    // Extract SoA rotation to scalar arrays (4 joints per SoA group)
    alignas(16) float rx[4], ry[4], rz[4], rw[4];
    ozz::math::StorePtrU(rot.x, rx);
    ozz::math::StorePtrU(rot.y, ry);
    ozz::math::StorePtrU(rot.z, rz);
    ozz::math::StorePtrU(rot.w, rw);

    // Extract correction quaternion components (xyzw packed in one SimdFloat4)
    alignas(16) float cq[4]; // [x, y, z, w]
    ozz::math::StorePtrU(correction.xyzw, cq);
    float cx = cq[0], cy = cq[1], cz = cq[2], cw = cq[3];

    // Hamilton product: result = correction * original
    float ox = rx[lane], oy = ry[lane], oz = rz[lane], ow = rw[lane];
    rx[lane] = cw * ox + cx * ow + cy * oz - cz * oy;
    ry[lane] = cw * oy - cx * oz + cy * ow + cz * ox;
    rz[lane] = cw * oz + cx * oy - cy * ox + cz * ow;
    rw[lane] = cw * ow - cx * ox - cy * oy - cz * oz;

    // Store back to SoA
    rot.x = ozz::math::simd_float4::Load(rx[0], rx[1], rx[2], rx[3]);
    rot.y = ozz::math::simd_float4::Load(ry[0], ry[1], ry[2], ry[3]);
    rot.z = ozz::math::simd_float4::Load(rz[0], rz[1], rz[2], rz[3]);
    rot.w = ozz::math::simd_float4::Load(rw[0], rw[1], rw[2], rw[3]);
}

// ── IK post-processing ────────────────────────────────────

void AnimationSystem::applyIK(AnimationInstance& inst, const SkeletonData& bindPose,
                               const TwoBoneIKSlot* twoBone, int twoBoneCount,
                               const AimIKSlot* aim, int aimCount) {
    if (!inst.skeleton) return;
    if (twoBoneCount == 0 && aimCount == 0) return;

    int numJoints = inst.skeleton.jointCount();

    // Two-bone IK (feet, arms)
    for (int i = 0; i < twoBoneCount; i++) {
        const auto& tb = twoBone[i];
        if (tb.weight <= 0.0f) continue;
        if (tb.startJoint < 0 || tb.midJoint < 0 || tb.endJoint < 0) continue;
        if (tb.startJoint >= numJoints || tb.midJoint >= numJoints || tb.endJoint >= numJoints) continue;

        ozz::animation::IKTwoBoneJob job;
        job.target       = ozz::math::simd_float4::Load(tb.target.x, tb.target.y, tb.target.z, 0.0f);
        job.pole_vector  = ozz::math::simd_float4::Load(tb.poleVector.x, tb.poleVector.y, tb.poleVector.z, 0.0f);
        job.mid_axis     = ozz::math::simd_float4::Load(tb.midAxis.x, tb.midAxis.y, tb.midAxis.z, 0.0f);
        job.weight       = tb.weight;
        job.soften       = tb.soften;
        job.start_joint  = &inst.models[tb.startJoint];
        job.mid_joint    = &inst.models[tb.midJoint];
        job.end_joint    = &inst.models[tb.endJoint];

        ozz::math::SimdQuaternion startCorr = ozz::math::SimdQuaternion::identity();
        ozz::math::SimdQuaternion midCorr   = ozz::math::SimdQuaternion::identity();
        job.start_joint_correction = &startCorr;
        job.mid_joint_correction   = &midCorr;

        bool reached = false;
        job.reached = &reached;

        if (job.Run()) {
            applySoACorrection(ozz::make_span(inst.blended), tb.startJoint, startCorr);
            applySoACorrection(ozz::make_span(inst.blended), tb.midJoint, midCorr);
        }
    }

    // Aim IK (head, spine look-at)
    for (int i = 0; i < aimCount; i++) {
        const auto& a = aim[i];
        if (a.weight <= 0.0f) continue;
        if (a.joint < 0 || a.joint >= numJoints) continue;

        ozz::animation::IKAimJob job;
        job.target      = ozz::math::simd_float4::Load(a.target.x, a.target.y, a.target.z, 0.0f);
        job.forward     = ozz::math::simd_float4::Load(a.forward.x, a.forward.y, a.forward.z, 0.0f);
        job.up          = ozz::math::simd_float4::Load(a.up.x, a.up.y, a.up.z, 0.0f);
        job.pole_vector = ozz::math::simd_float4::Load(a.poleVector.x, a.poleVector.y, a.poleVector.z, 0.0f);
        job.weight      = a.weight;
        job.joint       = &inst.models[a.joint];

        ozz::math::SimdQuaternion correction = ozz::math::SimdQuaternion::identity();
        job.joint_correction = &correction;

        bool reached = false;
        job.reached = &reached;

        if (job.Run()) {
            applySoACorrection(ozz::make_span(inst.blended), a.joint, correction);
        }
    }

    // Re-run L2M + skinning with corrected local transforms
    computeSkinningMatrices(inst, bindPose);
}

// ── Root motion extraction ─────────────────────────────────

RootMotionDelta AnimationSystem::extractRootMotion(AnimationInstance& inst,
                                                     glm::vec3& prevRootPos,
                                                     glm::quat& prevRootRot) {
    RootMotionDelta delta;
    if (!inst.skeleton) return delta;

    // Root joint is ozz index 0: SoA group 0, lane 0
    auto& root = inst.blended[0];

    // Extract root translation
    alignas(16) float tx[4], ty[4], tz[4];
    ozz::math::StorePtrU(root.translation.x, tx);
    ozz::math::StorePtrU(root.translation.y, ty);
    ozz::math::StorePtrU(root.translation.z, tz);

    glm::vec3 currentPos(tx[0], ty[0], tz[0]);

    // Extract root rotation
    alignas(16) float rx[4], ry[4], rz[4], rw[4];
    ozz::math::StorePtrU(root.rotation.x, rx);
    ozz::math::StorePtrU(root.rotation.y, ry);
    ozz::math::StorePtrU(root.rotation.z, rz);
    ozz::math::StorePtrU(root.rotation.w, rw);

    glm::quat currentRot(rw[0], rx[0], ry[0], rz[0]); // glm::quat ctor is (w, x, y, z)

    // Compute delta from previous frame
    delta.deltaPosition = currentPos - prevRootPos;
    delta.deltaRotation = currentRot * glm::inverse(prevRootRot);

    // Store current as previous for next frame
    prevRootPos = currentPos;
    prevRootRot = currentRot;

    // Zero root XZ translation (keep Y for vertical movement)
    tx[0] = 0.0f;
    tz[0] = 0.0f;
    root.translation.x = ozz::math::simd_float4::Load(tx[0], tx[1], tx[2], tx[3]);
    root.translation.z = ozz::math::simd_float4::Load(tz[0], tz[1], tz[2], tz[3]);

    return delta;
}

// ── SSBO bone palette ───────────────────────────────────────────────

void AnimationSystem::resetBonePalette() {
    m_boneWriteCursor = 0;
}

uint32_t AnimationSystem::uploadBones(const AnimationInstance& inst) {
    uint32_t offset = m_boneWriteCursor;
    uint32_t count  = (uint32_t)inst.skinningMatrices.size();

    if (offset + count > m_maxBones) {
        printf("[AnimationSystem] Bone palette overflow (%u + %u > %u)\n",
               offset, count, m_maxBones);
        return offset;
    }

    std::memcpy(&m_cpuBoneStaging[offset], inst.skinningMatrices.data(),
                count * sizeof(glm::mat4));
    m_boneWriteCursor += count;

    return offset;
}

void AnimationSystem::flushBonePalette(VkDevice /*device*/, VmaAllocator allocator) {
    if (m_boneWriteCursor == 0) return;

    // Write to persistently mapped SSBO (CPU_TO_GPU + MAPPED_BIT)
    void* mapped = m_bonePaletteSSBO.info.pMappedData;
    if (mapped) {
        std::memcpy(mapped, m_cpuBoneStaging.data(),
                    (size_t)m_boneWriteCursor * sizeof(glm::mat4));
    }

    // Flush for non-coherent memory (safe no-op if already coherent)
    vmaFlushAllocation(allocator, m_bonePaletteSSBO.allocation,
                       0, (VkDeviceSize)m_boneWriteCursor * sizeof(glm::mat4));
}

} // namespace sv
