// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "DLLLoader.h"
#include "AssetWatcher.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace sv {

// ═���═════════════════════════════���═══════════════════════════════════
// init — register DLL path + file watcher
// ══��════════════════════════════════════════════════════════════════

bool DLLLoader::init(const std::string& dllPath, AssetWatcher& watcher)
{
    m_dllPath = resolveDLLPath(dllPath);

    // Shadow path: same dir, appended _live
    auto p = std::filesystem::path(dllPath);
    m_shadowPath = (p.parent_path() / (p.stem().string() + "_live" + p.extension().string())).string();

    // Watch the original DLL for changes
    watcher.watch(dllPath, [this]() {
        printf("[DLLLoader] Detected change in %s\n", m_dllPath.c_str());
        m_pendingReload = true;
    });

    printf("[DLLLoader] Watching %s\n", dllPath.c_str());
    return true;
}

// ═���═══════════════════════��═══════════════════════════════��═════════
// shutdown — unload DLL + cleanup
// ═════════════════��═════════════════════════════════════════════════

void DLLLoader::shutdown(std::vector<IModularSystem*>& systems)
{
    if (!m_handle) return;

    // Shutdown all systems
    for (auto* sys : systems)
        sys->shutdown();
    systems.clear();

    // Destroy + unload
    if (m_destroyFn) m_destroyFn();
    FreeLibrary(m_handle);
    m_handle    = nullptr;
    m_createFn  = nullptr;
    m_destroyFn = nullptr;

    // Clean up shadow copy
    std::error_code ec;
    std::filesystem::remove(m_shadowPath, ec);

    printf("[DLLLoader] Unloaded\n");
}

// ═══════════════════════════════════════════════════════════════════
// load — initial DLL load
// ═══════════════════════════════════════════════════════════════════

bool DLLLoader::load(const SystemContext& ctx,
                     std::vector<IModularSystem*>& systems)
{
    return loadSystems(ctx, systems, {});
}

// ════��══════════════════════════════════════════════════════════════
// checkReload — poll for changes, perform reload cycle
// NOTE: Caller must call vkDeviceWaitIdle() before this.
// ══════════════════���══════════════════════════════��═════════════════

bool DLLLoader::checkReload(const SystemContext& ctx,
                            std::vector<IModularSystem*>& systems)
{
    if (!m_pendingReload) return false;
    m_pendingReload = false;

    printf("[DLLLoader] Reloading %s...\n", m_dllPath.c_str());

    // 1. Serialize state from all current systems
    auto savedStates = unloadSystems(systems);

    // 2. Load new DLL + restore state
    if (!loadSystems(ctx, systems, savedStates)) {
        fprintf(stderr, "[DLLLoader] Reload FAILED, systems may be missing\n");
        return false;
    }

    printf("[DLLLoader] Reload complete (%zu systems)\n", systems.size());
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// unloadSystems — serialize state + shutdown + FreeLibrary
// ═══════════════════════════════════════════════════════════════════

std::vector<std::string> DLLLoader::unloadSystems(std::vector<IModularSystem*>& systems)
{
    std::vector<std::string> states;
    states.reserve(systems.size());

    for (auto* sys : systems) {
        states.push_back(sys->serializeState());
        sys->shutdown();
    }
    systems.clear();

    if (m_destroyFn) m_destroyFn();
    if (m_handle) FreeLibrary(m_handle);
    m_handle    = nullptr;
    m_createFn  = nullptr;
    m_destroyFn = nullptr;

    return states;
}

static bool sweepContextSlots(const BaseSystemContext& ctx);

// ═��═════════════════════════════════════════════��═══════════════════
// loadSystems — shadow copy + LoadLibrary + init systems
// ════════════════════════════════════════════════════���══════════════

bool DLLLoader::loadSystems(const SystemContext& ctx,
                            std::vector<IModularSystem*>& systems,
                            const std::vector<std::string>& savedStates)
{
    if (!shadowCopy()) return false;

    m_handle = LoadLibraryA(m_shadowPath.c_str());
    if (!m_handle) {
        DWORD err = GetLastError();
        fprintf(stderr, "[DLLLoader] LoadLibrary failed (error %lu): %s\n",
                err, m_shadowPath.c_str());
        return false;
    }

    m_createFn  = (CreateSystemsFn)GetProcAddress(m_handle, "createSystems");
    m_destroyFn = (DestroySystemsFn)GetProcAddress(m_handle, "destroySystems");

    if (!m_createFn) {
        fprintf(stderr, "[DLLLoader] createSystems() not found in DLL\n");
        FreeLibrary(m_handle);
        m_handle = nullptr;
        return false;
    }

    // Create systems
    int count = 0;
    IModularSystem** sysArray = m_createFn(&count);
    if (!sysArray || count <= 0) {
        fprintf(stderr, "[DLLLoader] createSystems returned no systems\n");
        return true;
    }

    // ── Version check ──────────────────────────────────────────────
    std::vector<IModularSystem*> accepted;
    accepted.reserve(count);
    for (int i = 0; i < count; i++) {
        IModularSystem* sys = sysArray[i];
        if (!sys) continue;

        uint32_t ver = sys->getInterfaceVersion();
        if (ver != IModularSystem::kEngineInterfaceVersion) {
            fprintf(stderr, "[DLLLoader]   REJECTED %s: interface v%u (engine expects v%u)\n",
                    sys->name(), ver, IModularSystem::kEngineInterfaceVersion);
            continue;
        }
        accepted.push_back(sys);
    }

    // ── Priority sort (stable, ascending — lower values init first)
    std::stable_sort(accepted.begin(), accepted.end(),
        [](const IModularSystem* a, const IModularSystem* b) {
            return a->initPriority() < b->initPriority();
        });

    // ── Null fn-ptr sweep on context ───────────────────────────────
    // SystemContext inherits BaseSystemContext via single non-virtual
    // public inheritance — base subobject is at offset 0.
    if (!sweepContextSlots(reinterpret_cast<const BaseSystemContext&>(ctx))) {
        fprintf(stderr, "[DLLLoader] Aborting system init — context not fully wired\n");
        return false;
    }

    // ── Init each system ───────────────────────────────────────────
    for (int i = 0; i < (int)accepted.size(); i++) {
        IModularSystem* sys = accepted[i];

        if (sys->init(ctx)) {
            if (i < (int)savedStates.size())
                sys->deserializeState(savedStates[i]);

            systems.push_back(sys);
            printf("[DLLLoader]   System loaded: %s (hook=%d, priority=%d)\n",
                   sys->name(), (int)sys->hookMode(), sys->initPriority());
        } else {
            fprintf(stderr, "[DLLLoader]   System FAILED to init: %s\n", sys->name());
        }
    }

    return true;
}

// ═════════════════════════════════════════════���═════════════════════
// sweepContextSlots — verify required fn-ptr slots in BaseSystemContext
// Returns false if any required slot is null.
// ═══════════════════════════════════════════════════════════════════

static bool sweepContextSlots(const BaseSystemContext& ctx)
{
    int missing = 0;

    auto require = [&](bool present, const char* name) {
        if (!present) {
            fprintf(stderr, "[DLLLoader]   REQUIRED fn-ptr null: %s\n", name);
            ++missing;
        }
    };
    auto warn = [](bool present, const char* name) {
        if (!present)
            printf("[DLLLoader]   Optional fn-ptr null: %s\n", name);
    };

    // ── std::function — required ───────────────────────────────────
    // (sub-struct paths — see BaseSystemContext.h)
    require(!!ctx.rendering.getGraphicsPipeline, "rendering.getGraphicsPipeline");
    require(!!ctx.input.isKeyPressed,            "input.isKeyPressed");
    require(!!ctx.input.isKeyDown,               "input.isKeyDown");
    require(!!ctx.input.getMousePos,             "input.getMousePos");
    require(!!ctx.input.isMouseDown,             "input.isMouseDown");
    require(!!ctx.input.getScrollDelta,          "input.getScrollDelta");
    require(!!ctx.input.isCursorLocked,          "input.isCursorLocked");
    require(!!ctx.input.setCursorLocked,         "input.setCursorLocked");
    require(!!ctx.buffers.createBuffer,          "buffers.createBuffer");
    require(!!ctx.buffers.destroyBuffer,         "buffers.destroyBuffer");
    require(!!ctx.buffers.mapBuffer,             "buffers.mapBuffer");
    require(!!ctx.buffers.unmapBuffer,           "buffers.unmapBuffer");
    require(!!ctx.buffers.createShaderModule,    "buffers.createShaderModule");

    // ── PFN_vk — required (loaded by volk at init) ────────────────
    require(ctx.vkfn.fnCmdBindPipeline            != nullptr, "vkfn.fnCmdBindPipeline");
    require(ctx.vkfn.fnCmdBindDescriptorSets      != nullptr, "vkfn.fnCmdBindDescriptorSets");
    require(ctx.vkfn.fnCmdBindVertexBuffers       != nullptr, "vkfn.fnCmdBindVertexBuffers");
    require(ctx.vkfn.fnCmdBindIndexBuffer         != nullptr, "vkfn.fnCmdBindIndexBuffer");
    require(ctx.vkfn.fnCmdDrawIndexed             != nullptr, "vkfn.fnCmdDrawIndexed");
    require(ctx.vkfn.fnCmdDraw                    != nullptr, "vkfn.fnCmdDraw");
    require(ctx.vkfn.fnCmdDrawIndirect            != nullptr, "vkfn.fnCmdDrawIndirect");
    require(ctx.vkfn.fnCmdDrawIndexedIndirect     != nullptr, "vkfn.fnCmdDrawIndexedIndirect");
    require(ctx.vkfn.fnCmdPushConstants           != nullptr, "vkfn.fnCmdPushConstants");
    require(ctx.vkfn.fnCmdSetScissor              != nullptr, "vkfn.fnCmdSetScissor");
    require(ctx.vkfn.fnCmdDispatch                != nullptr, "vkfn.fnCmdDispatch");
    require(ctx.vkfn.fnCmdPipelineBarrier         != nullptr, "vkfn.fnCmdPipelineBarrier");
    require(ctx.vkfn.fnCmdFillBuffer              != nullptr, "vkfn.fnCmdFillBuffer");
    require(ctx.vkfn.fnCmdUpdateBuffer            != nullptr, "vkfn.fnCmdUpdateBuffer");
    require(ctx.vkfn.fnCreateDescriptorPool       != nullptr, "vkfn.fnCreateDescriptorPool");
    require(ctx.vkfn.fnDestroyDescriptorPool      != nullptr, "vkfn.fnDestroyDescriptorPool");
    require(ctx.vkfn.fnCreateDescriptorSetLayout  != nullptr, "vkfn.fnCreateDescriptorSetLayout");
    require(ctx.vkfn.fnDestroyDescriptorSetLayout != nullptr, "vkfn.fnDestroyDescriptorSetLayout");
    require(ctx.vkfn.fnAllocateDescriptorSets     != nullptr, "vkfn.fnAllocateDescriptorSets");
    require(ctx.vkfn.fnUpdateDescriptorSets       != nullptr, "vkfn.fnUpdateDescriptorSets");
    require(ctx.vkfn.fnCreatePipelineLayout       != nullptr, "vkfn.fnCreatePipelineLayout");
    require(ctx.vkfn.fnDestroyPipelineLayout      != nullptr, "vkfn.fnDestroyPipelineLayout");
    require(ctx.vkfn.fnCreateComputePipelines     != nullptr, "vkfn.fnCreateComputePipelines");
    require(ctx.vkfn.fnDestroyPipeline            != nullptr, "vkfn.fnDestroyPipeline");
    require(ctx.vkfn.fnCreateImageView            != nullptr, "vkfn.fnCreateImageView");
    require(ctx.vkfn.fnDestroyImageView           != nullptr, "vkfn.fnDestroyImageView");
    require(ctx.vkfn.fnCreateSampler              != nullptr, "vkfn.fnCreateSampler");
    require(ctx.vkfn.fnDestroySampler             != nullptr, "vkfn.fnDestroySampler");
    require(ctx.vkfn.fnDestroyShaderModule        != nullptr, "vkfn.fnDestroyShaderModule");

    // ── std::function — optional (warn only) ───────────────────────
    warn(!!ctx.input.toggleFullscreen,          "input.toggleFullscreen");
    warn(!!ctx.ui.saveStyleSection,             "ui.saveStyleSection");
    warn(!!ctx.buffers.createImage,             "buffers.createImage");
    warn(!!ctx.buffers.destroyImage,            "buffers.destroyImage");
    warn(!!ctx.meshRegistry.registerMesh,       "meshRegistry.registerMesh");
    warn(!!ctx.meshRegistry.registerMaterial,   "meshRegistry.registerMaterial");
    warn(!!ctx.meshRegistry.getMeshSlot,        "meshRegistry.getMeshSlot");
    warn(!!ctx.rendering.registerRenderPass,    "rendering.registerRenderPass");
    warn(!!ctx.rendering.unregisterRenderPass,  "rendering.unregisterRenderPass");
    warn(!!ctx.input.isActionDown,              "input.isActionDown");
    warn(!!ctx.input.isActionPressed,           "input.isActionPressed");
    warn(!!ctx.input.getActionAxis,             "input.getActionAxis");
    warn(!!ctx.rendering.captureScreenshot,     "rendering.captureScreenshot");

    if (missing > 0) {
        fprintf(stderr, "[DLLLoader] Context sweep FAILED: %d required fn-ptr(s) null\n", missing);
        return false;
    }
    printf("[DLLLoader] Context sweep OK\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// shadowCopy — copy DLL to _live path (avoids build lock)
// ═══════════════════════════════════════════════════════════════════

bool DLLLoader::shadowCopy()
{
    std::error_code ec;

    // Remove old shadow copy
    std::filesystem::remove(m_shadowPath, ec);

    // Copy fresh
    if (!std::filesystem::copy_file(m_dllPath, m_shadowPath,
            std::filesystem::copy_options::overwrite_existing, ec)) {
        fprintf(stderr, "[DLLLoader] Shadow copy failed (%s -> %s): %s\n",
                m_dllPath.c_str(), m_shadowPath.c_str(), ec.message().c_str());
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// resolveDLLPath — find DLL: check given path, then exe-relative
// ═══════════════════════════════════════════════════════════════════

std::string DLLLoader::resolveDLLPath(const std::string& path)
{
    namespace fs = std::filesystem;

    // Already absolute and exists — use as-is
    if (fs::path(path).is_absolute() && fs::exists(path))
        return path;

    // Relative and exists in CWD — resolve to absolute
    if (fs::exists(path))
        return fs::absolute(path).string();

    // Try exe-relative
    char exeBuf[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exeBuf, MAX_PATH) > 0) {
        auto exeDir = fs::path(exeBuf).parent_path();
        auto candidate = exeDir / path;
        if (fs::exists(candidate)) {
            printf("[DLLLoader] Resolved DLL via exe directory: %s\n",
                   candidate.string().c_str());
            return candidate.string();
        }
    }

    // Not found yet — return original (DLL may appear after build)
    return path;
}

} // namespace sv
