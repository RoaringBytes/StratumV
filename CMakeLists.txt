cmake_minimum_required(VERSION 3.24)
project(StratumV VERSION 1.3.10 LANGUAGES C CXX)

# ── Version header (auto-generated from project VERSION) ─────────
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/StratumVVersion.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/StratumVVersion.h"
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Engine Options ─────────────────────────────────────────────────
# NVIDIA SDKs below are proprietary and OFF by default. Enabling them fetches
# (or, for DLSS, requires you to supply) NVIDIA-licensed code that is NOT
# covered by StratumV's Apache-2.0 license — see THIRD_PARTY_NOTICES.md.
option(STRATUMV_ENABLE_DLSS   "Enable NVIDIA DLSS Super Resolution"          OFF)
option(STRATUMV_ENABLE_SHARC  "Enable NVIDIA SHaRC Global Illumination"      OFF)
option(STRATUMV_ENABLE_NTC    "Enable NVIDIA RTXNTC Neural Texture Compression" OFF)
option(STRATUMV_ENABLE_JOLT   "Enable Jolt Physics (JoltPhysicsContext)"      OFF)
option(STRATUMV_ENABLE_MSQUIC "Enable MsQuic transport for networking/replication" ON)
option(STRATUMV_BUILD_TESTS   "Build StratumV unit tests"                    OFF)

# ── Core-only carve-out ─────────────────────────────────────────────
# When ON, the build omits the full engine library (`stratumv`) and
# every heavy graphics dependency (Vulkan, glslang, glm, ozz-animation,
# ImGui, miniaudio, ufbx, tinygltf, meshoptimizer, Jolt, DLSS, SHARC,
# NTC) — only `stratumv_core` + `stratumv_server` + the core test
# subset compile. This is the mode a headless Linux dedicated server
# uses; Windows dev boxes still build with CORE_ONLY=OFF by default.
#
# Requires STRATUMV_ENABLE_MSQUIC=ON — there is nothing to build in
# stratumv_core without the transport.
option(STRATUMV_CORE_ONLY
    "Build only stratumv_core (Layer 4 net/replication, no graphics)" OFF)

if(STRATUMV_CORE_ONLY AND NOT STRATUMV_ENABLE_MSQUIC)
    message(FATAL_ERROR
        "[StratumV] STRATUMV_CORE_ONLY=ON requires STRATUMV_ENABLE_MSQUIC=ON. "
        "The core carve-out is built around the MsQuic transport — there is "
        "nothing to compile without it.")
endif()

if(STRATUMV_CORE_ONLY)
    message(STATUS "[StratumV] STRATUMV_CORE_ONLY=ON: building stratumv_core only "
        "(graphics deps omitted)")
endif()

# ── Dependencies via FetchContent ──────────────────────────────────
include(FetchContent)

# nlohmann/json is the only non-graphics dependency — it's always fetched
# because both stratumv_core (via future config hooks) and stratumv (via
# the asset pipeline + scene loader + persistence modules) can consume it.
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)

# Heavy graphics-stack dependencies — omitted from STRATUMV_CORE_ONLY
# builds so a headless Linux server does not need Vulkan, glslang, glm,
# ozz, ImGui, etc. The stratumv_core carve-out compiles against std::
# + nlohmann/json + msquic only.
if(NOT STRATUMV_CORE_ONLY)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.3
        GIT_SHALLOW    TRUE
    )

    FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG        v1.4.313
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(VulkanHeaders)

    FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG        vulkan-sdk-1.4.313.0
        GIT_SHALLOW    TRUE
    )
    set(VOLK_STATIC_DEFINES      VK_USE_PLATFORM_WIN32_KHR CACHE STRING "" FORCE)
    set(VULKAN_HEADERS_INSTALL_DIR "${vulkanheaders_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(VOLK_PULL_IN_VULKAN      ON CACHE BOOL "" FORCE)

    FetchContent_Declare(VulkanMemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
    )
    set(VMA_VULKAN_VERSION 1003000 CACHE STRING "" FORCE)

    FetchContent_Declare(entt
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG        v3.16.0
        GIT_SHALLOW    TRUE
    )

    FetchContent_Declare(tinygltf
        GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
        GIT_TAG        v3.0.0
        GIT_SHALLOW    TRUE
    )

    FetchContent_Declare(meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG        v1.2
        GIT_SHALLOW    TRUE
    )

    # ufbx: single-source FBX file loader (MIT license)
    FetchContent_Declare(ufbx
        GIT_REPOSITORY https://github.com/ufbx/ufbx.git
        GIT_TAG        v0.23.0
        GIT_SHALLOW    TRUE
    )

    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER      OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST            OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL             OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING           OFF CACHE BOOL "" FORCE)
    set(SKIP_GLSLANG_INSTALL    ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG        16.3.0
        GIT_SHALLOW    TRUE
    )

    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.8
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    FetchContent_Declare(miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG        0.11.25
        GIT_SHALLOW    TRUE
    )

    # ozz-animation: SIMD-optimized skeletal animation runtime
    set(ozz_build_tools       OFF CACHE BOOL "" FORCE)
    set(ozz_build_samples     OFF CACHE BOOL "" FORCE)
    set(ozz_build_tests       OFF CACHE BOOL "" FORCE)
    set(ozz_build_howtos      OFF CACHE BOOL "" FORCE)
    set(ozz_build_fbx         OFF CACHE BOOL "" FORCE)
    set(ozz_build_gltf        OFF CACHE BOOL "" FORCE)
    set(ozz_build_msvc_rt_dll ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(ozz-animation
        GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git
        GIT_TAG        0.16.0
        GIT_SHALLOW    TRUE
    )
endif()  # NOT STRATUMV_CORE_ONLY

# ── MsQuic transport (networking/replication) ─────────────────────
# Windows: consumed as the Microsoft.Native.Quic.MsQuic.Schannel NuGet
# package (prebuilt Schannel-backed msquic.dll + msquic.lib + headers).
# Linux: consumed from a user-provided prebuilt tarball extraction
# directory pointed at by STRATUMV_LINUX_MSQUIC_ROOT — see
# docs/LINUX_SERVER.md for the step-by-step. Both paths populate the
# same `msquic` IMPORTED SHARED library target so the rest of the
# CMake tree is platform-agnostic below this block.
#
# Pinned to 2.5.9 (stable, 2026-07-02). When bumping: download the
# matching tarball/nupkg and update the SHA256/paths below.
if(STRATUMV_ENABLE_MSQUIC)
    if(WIN32)
        FetchContent_Declare(msquic
            URL         https://www.nuget.org/api/v2/package/Microsoft.Native.Quic.MsQuic.Schannel/2.5.9
            URL_HASH    SHA256=9877a919e1aa73aa4800f1e8a06b6539021f376228910138eb04540cd956617b
            DOWNLOAD_NAME msquic-2.5.9.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        # The MsQuic NuGet zip has no CMakeLists.txt — MakeAvailable
        # just extracts into msquic_SOURCE_DIR without add_subdirectory.
        FetchContent_MakeAvailable(msquic)

        set(MSQUIC_DLL_PATH "${msquic_SOURCE_DIR}/build/native/bin/x64/msquic.dll")
        set(MSQUIC_LIB_PATH "${msquic_SOURCE_DIR}/build/native/lib/x64/msquic.lib")
        set(MSQUIC_INCLUDE_DIR "${msquic_SOURCE_DIR}/build/native/include")

        if(NOT EXISTS "${MSQUIC_LIB_PATH}" OR NOT EXISTS "${MSQUIC_DLL_PATH}")
            message(FATAL_ERROR
                "[StratumV] MsQuic NuGet package extraction appears incomplete.\n"
                "  Expected: ${MSQUIC_LIB_PATH}\n"
                "            ${MSQUIC_DLL_PATH}\n"
                "Delete build/_deps/msquic-* and reconfigure, or disable with "
                "-DSTRATUMV_ENABLE_MSQUIC=OFF.")
        endif()

        add_library(msquic SHARED IMPORTED GLOBAL)
        set_target_properties(msquic PROPERTIES
            IMPORTED_LOCATION             "${MSQUIC_DLL_PATH}"
            IMPORTED_IMPLIB               "${MSQUIC_LIB_PATH}"
            INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}"
        )

        # Helper: POST_BUILD copy msquic.dll next to the target's exe.
        # Windows-only; on Linux the libmsquic.so resolution is
        # handled by LD_LIBRARY_PATH or the system loader.
        function(stratumv_copy_msquic_dll target)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MSQUIC_DLL_PATH}"
                    "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copying msquic.dll next to ${target}"
            )
        endfunction()

        message(STATUS "[StratumV] MsQuic enabled (${MSQUIC_DLL_PATH})")

    elseif(UNIX AND NOT APPLE)
        # ── Linux: prebuilt MsQuic 2.5.9 (headers + libmsquic.so) ──
        # The user must provision an MsQuic 2.5.9 root once (headers
        # from the pinned source tag, libmsquic.so from
        # packages.microsoft.com) and point CMake at it with
        # -DSTRATUMV_LINUX_MSQUIC_ROOT=/opt/msquic-2.5.9. The expected
        # layout is:
        #   <root>/include/msquic.h
        #   <root>/include/msquic_posix.h
        #   <root>/bin/libmsquic.so  (or libmsquic.so.2 / .2.5.9)
        #
        # Recipe in docs/LINUX_SERVER.md. We honour the pinned version
        # via a hard preflight check on msquic.h's #define.
        set(STRATUMV_LINUX_MSQUIC_ROOT "" CACHE PATH
            "Path to a provisioned MsQuic 2.5.9 Linux root (must contain include/msquic.h and bin/libmsquic.so).")
        if(NOT STRATUMV_LINUX_MSQUIC_ROOT)
            message(FATAL_ERROR
                "[StratumV] On Linux with STRATUMV_ENABLE_MSQUIC=ON you must "
                "set -DSTRATUMV_LINUX_MSQUIC_ROOT=/path/to/extracted/msquic. "
                "See docs/LINUX_SERVER.md for the download + extract steps.")
        endif()

        set(MSQUIC_INCLUDE_DIR "${STRATUMV_LINUX_MSQUIC_ROOT}/include")
        if(NOT EXISTS "${MSQUIC_INCLUDE_DIR}/msquic.h")
            message(FATAL_ERROR
                "[StratumV] ${MSQUIC_INCLUDE_DIR}/msquic.h not found. Is "
                "STRATUMV_LINUX_MSQUIC_ROOT pointing at the extracted "
                "tarball root?")
        endif()

        # MsQuic on Linux ships versioned libmsquic.so.<X.Y.Z> with
        # unversioned / major-version symlinks. Search for the most
        # specific name first, fall back to the ones glibc's ld.so
        # resolves from /etc/ld.so.conf.d/*.
        find_library(MSQUIC_LIB_PATH
            NAMES msquic libmsquic.so.2.5.9 libmsquic.so.2 libmsquic.so
            PATHS "${STRATUMV_LINUX_MSQUIC_ROOT}/bin"
                  "${STRATUMV_LINUX_MSQUIC_ROOT}/lib"
            NO_DEFAULT_PATH
        )
        if(NOT MSQUIC_LIB_PATH)
            message(FATAL_ERROR
                "[StratumV] libmsquic.so not found under "
                "${STRATUMV_LINUX_MSQUIC_ROOT}/bin or "
                "${STRATUMV_LINUX_MSQUIC_ROOT}/lib. The Linux tarball "
                "normally ships it as bin/libmsquic.so.<version>. "
                "Check the extracted tarball layout.")
        endif()

        add_library(msquic SHARED IMPORTED GLOBAL)
        set_target_properties(msquic PROPERTIES
            IMPORTED_LOCATION             "${MSQUIC_LIB_PATH}"
            INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}"
        )

        # POST_BUILD copy-next-to-exe is a Windows-only quirk because
        # the Windows loader walks $PWD before PATH. Linux uses
        # LD_LIBRARY_PATH / rpath, so this is a no-op on Linux — the
        # user adds the tarball's bin directory to LD_LIBRARY_PATH or
        # we set an rpath below on stratumv_server.
        function(stratumv_copy_msquic_dll target)
            # no-op on Linux; see docs/LINUX_SERVER.md rpath section.
        endfunction()

        message(STATUS "[StratumV] MsQuic (Linux) enabled (${MSQUIC_LIB_PATH})")

    else()
        message(FATAL_ERROR
            "[StratumV] STRATUMV_ENABLE_MSQUIC=ON but platform is neither "
            "Windows nor Linux. macOS / BSD is not supported.")
    endif()

    set(MSQUIC_FOUND TRUE)
else()
    function(stratumv_copy_msquic_dll target)
        # No-op when MsQuic is disabled.
    endfunction()
    set(MSQUIC_FOUND FALSE)
endif()

# nlohmann/json is always made available — both flavors can consume it.
FetchContent_MakeAvailable(json)

if(NOT STRATUMV_CORE_ONLY)
    # Optional: Jolt Physics (character controller, terrain, rigid bodies)
    if(STRATUMV_ENABLE_JOLT)
        FetchContent_Declare(JoltPhysics
            GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
            GIT_TAG        v5.2.0
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  Build
        )
        set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
        set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
        set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
        set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
        set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
        set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    endif()

    FetchContent_MakeAvailable(
        glfw glm volk VulkanMemoryAllocator entt tinygltf glslang meshoptimizer miniaudio
        ozz-animation ufbx
    )

    # ufbx: build as a static library (single C file, no CMakeLists.txt in repo)
    if(NOT TARGET ufbx)
        add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
        target_include_directories(ufbx PUBLIC ${ufbx_SOURCE_DIR})
        if(MSVC)
            set_target_properties(ufbx PROPERTIES
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endif()

    # Make Jolt available separately (after main MakeAvailable, to avoid ordering issues)
    if(STRATUMV_ENABLE_JOLT)
        FetchContent_MakeAvailable(JoltPhysics)
        # Force Jolt to use dynamic CRT (/MD) to match the rest of the project
        if(TARGET Jolt)
            set_target_properties(Jolt PROPERTIES
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endif()
endif()  # NOT STRATUMV_CORE_ONLY

# ── Optional graphics SDKs (skipped in STRATUMV_CORE_ONLY builds) ───
set(DLSS_FOUND  FALSE)
set(SHARC_FOUND FALSE)
set(NTC_FOUND   FALSE)

if(NOT STRATUMV_CORE_ONLY)
    # ── Optional: DLSS SDK ─────────────────────────────────────────
    set(DLSS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/dlss)
    if(STRATUMV_ENABLE_DLSS AND EXISTS ${DLSS_DIR}/include/nvsdk_ngx_vk.h)
        add_library(stratumv_dlss INTERFACE)
        target_include_directories(stratumv_dlss INTERFACE ${DLSS_DIR}/include)
        target_link_libraries(stratumv_dlss INTERFACE
            ${DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib)
        target_compile_definitions(stratumv_dlss INTERFACE DLSS_AVAILABLE=1)
        set(DLSS_FOUND TRUE)
    endif()

    # ── Optional: SHaRC headers ────────────────────────────────────
    if(STRATUMV_ENABLE_SHARC)
        FetchContent_Declare(sharc
            GIT_REPOSITORY https://github.com/NVIDIA-RTX/SHARC.git
            GIT_TAG        v1.6.5.0
            GIT_SHALLOW    TRUE
        )
        FetchContent_GetProperties(sharc)
        if(NOT sharc_POPULATED)
            FetchContent_Populate(sharc)
        endif()
        set(SHARC_FOUND TRUE)
    endif()

    # ── Optional: RTXNTC ──────────────────────────────────────────
    if(STRATUMV_ENABLE_NTC)
        set(NTC_WITH_VULKAN         ON  CACHE BOOL "" FORCE)
        set(NTC_WITH_DX12           OFF CACHE BOOL "" FORCE)
        set(NTC_WITH_CUDA           OFF CACHE BOOL "" FORCE)
        set(NTC_BUILD_SHARED        ON  CACHE BOOL "" FORCE)
        set(NTC_WITH_PREBUILT_SHADERS ON CACHE BOOL "" FORCE)
        set(LIBNTC_BIN_DIRECTORY    "${CMAKE_BINARY_DIR}/bin" CACHE PATH "" FORCE)
        FetchContent_Declare(rtxntc
            GIT_REPOSITORY https://github.com/NVIDIA-RTX/RTXNTC-Library.git
            GIT_TAG        7ab602cb9b42ee3856e63c7a6b5e9575af051a4c
        )
        FetchContent_GetProperties(rtxntc)
        if(NOT rtxntc_POPULATED)
            FetchContent_Populate(rtxntc)
        endif()
        if(MSVC)
            set(_ntc_save "${CMAKE_MSVC_RUNTIME_LIBRARY}")
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
        add_subdirectory("${rtxntc_SOURCE_DIR}" "${rtxntc_BINARY_DIR}")
        if(MSVC)
            if(_ntc_save)
                set(CMAKE_MSVC_RUNTIME_LIBRARY "${_ntc_save}")
            else()
                unset(CMAKE_MSVC_RUNTIME_LIBRARY)
            endif()
            if(TARGET libntc)
                set_target_properties(libntc PROPERTIES
                    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            endif()
        endif()
        set(NTC_FOUND TRUE)
    endif()
endif()  # NOT STRATUMV_CORE_ONLY

# ── stratumv_core static library ───────────────────────────────────
# Layer 4 networking + replication carve-out. Compiles against std::
# + nlohmann/json + msquic only — no Vulkan / glslang / glm / ozz /
# ImGui / miniaudio / ufbx. Built unconditionally when MsQuic is on,
# because both stratumv (full engine) and stratumv_server link against
# it, and we want a single canonical build of these translation units
# regardless of STRATUMV_CORE_ONLY.
if(STRATUMV_ENABLE_MSQUIC)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/stratumv_core_sources.cmake")

    add_library(stratumv_core STATIC ${STRATUMV_CORE_SOURCES})

    target_include_directories(stratumv_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/engine>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(stratumv_core PUBLIC
        STRATUMV_MSQUIC_AVAILABLE=1
    )

    target_link_libraries(stratumv_core PUBLIC
        nlohmann_json::nlohmann_json
        msquic
    )

    if(WIN32)
        # Windows crypto APIs for CertCreateSelfSignCertificate
        # (loopback test cert path in MsQuicTransport.cpp).
        target_link_libraries(stratumv_core PUBLIC ws2_32 crypt32 ncrypt)
    elseif(UNIX AND NOT APPLE)
        # Linux OpenSSL path for self-signed cert generation. MsQuic on
        # Linux already links against libcrypto for its TLS backend, so
        # this is the same shared library that libmsquic.so loaded.
        find_package(OpenSSL REQUIRED COMPONENTS Crypto)
        target_link_libraries(stratumv_core PUBLIC OpenSSL::Crypto)
    endif()

    if(MSVC)
        target_compile_definitions(stratumv_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        set_target_properties(stratumv_core PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

# ── StratumV full engine sources ────────────────────────────────────
# NOTE: The list below INTENTIONALLY excludes the core-only sources listed in
# cmake/stratumv_core_sources.cmake — stratumv links stratumv_core PUBLIC
# to pick them up, so we never double-compile a translation unit.
if(NOT STRATUMV_CORE_ONLY)
    set(STRATUMV_SOURCES
        # ── Vulkan abstraction + platform ──
        src/engine/vk/VkContext.cpp
        src/engine/vk/VkSwapchain.cpp
        src/engine/vk/VkShader.cpp
        src/engine/vk/VkBuffer.cpp
        src/engine/vk/VkTexture.cpp
        src/engine/vk/VkDescriptors.cpp
        src/engine/vk/VkPipeline.cpp
        src/engine/vk/VkComputePipeline.cpp
        src/engine/vk/VkRTPipeline.cpp
        src/engine/vk/VkClusterAS.cpp
        src/engine/vk/VkMesh.cpp
        src/engine/vk/GltfLoader.cpp
        src/engine/vk/FbxLoader.cpp
        src/engine/vk/CC5Sidecar.cpp
        src/engine/vk/VkAccelStructure.cpp
        src/engine/vk/VkNtcTexture.cpp
        src/engine/vk/PipelineRegistry.cpp
        src/engine/vk/PipelineCache.cpp   # VkPipelineCache persistence
        src/engine/DlssWrapper.cpp
        src/engine/Window.cpp
        src/engine/Types.cpp

        # ── Core engine services ──
        src/engine/Camera.cpp
        src/engine/FreeFlyCamera.cpp
        src/engine/FollowCamera.cpp
        src/engine/OrbitCamera.cpp
        src/engine/Input.cpp
        src/engine/InputAction.cpp
        src/engine/InputBindings.cpp
        src/engine/Audio.cpp
        src/engine/Config.cpp

        # ── Render graph + passes ──
        src/engine/PostProcess.cpp
        src/engine/graph/RenderGraph.cpp
        src/engine/graph/GraphResources.cpp
        src/engine/passes/ShadowPass.cpp

        # ── UI layer ──
        src/engine/ui/UiStyle.cpp
        src/engine/ui/ImGuiLayer.cpp
        src/engine/ui/AdminPanel.cpp

        # ── Plugin infrastructure + DevServer ──
        src/engine/DLLLoader.cpp
        src/engine/AssetWatcher.cpp
        src/engine/SystemRegistry.cpp
        src/engine/DevServer.cpp

        # ── Persistence + base context ──
        src/engine/WorldStateIO.cpp
        # src/engine/WorldStateResolver.cpp          # deferred — needs WorldClock extraction
        src/engine/SceneStatePersistence.cpp
        src/engine/SceneStateVersioning.cpp

        # ── Engine base ──
        src/engine/EngineBase.cpp

        # ── Scene loader ──
        src/engine/SceneLoader.cpp

        # ── Material pipeline ──
        src/engine/MaterialPipeline.cpp

        # ── Animation system ──
        src/engine/AnimationSystem.cpp

        # ── Animation state machine ──
        src/engine/AnimationStateMachine.cpp

        # ── Blend tree + partial blending ──
        src/engine/BlendTree.cpp

        # ── Skinned mesh rendering ──
        src/engine/passes/SkinnedMeshPass.cpp

        # ── Morph target types ──
        src/engine/MorphTargetTypes.cpp

        # ── Physics interface ──
        src/engine/IPhysicsContext.cpp

        # ── Frustum culling ──
        src/engine/FrustumCuller.cpp

        # ── Asset manifest ──
        src/engine/AssetManifest.cpp

        # ── Asset browser ──
        src/engine/AssetBrowser.cpp

        # ── Thumbnail cache ──
        src/engine/ThumbnailCache.cpp

        # ── Editor bridge (Windows plain-TCP gateway for Blender live
        # link). NOT in the core subset — the Linux dedicated-server
        # carve-out has no use for it, and it's winsock-only today.
        src/engine/net/EditorBridge.cpp

        # NOTE: INetworkContext, EngineLog, ReplicationRegistry,
        # NetTransform + ReplicationProtocol, and MsQuicTransport live
        # in stratumv_core (see cmake/stratumv_core_sources.cmake) and
        # flow into stratumv via target_link_libraries PUBLIC below.
    )

    # ── Target: stratumv static library ───────────────────────────
    add_library(stratumv STATIC ${STRATUMV_SOURCES})

    # Public include paths exposed to all consumers
    target_include_directories(stratumv PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/engine>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${imgui_SOURCE_DIR}>
        $<BUILD_INTERFACE:${imgui_SOURCE_DIR}/backends>
        $<BUILD_INTERFACE:$<$<BOOL:${NTC_FOUND}>:${rtxntc_SOURCE_DIR}/include>>
        $<BUILD_INTERFACE:$<$<BOOL:${SHARC_FOUND}>:${sharc_SOURCE_DIR}/include>>
        $<BUILD_INTERFACE:${miniaudio_SOURCE_DIR}>
        $<BUILD_INTERFACE:${tinygltf_SOURCE_DIR}>
        $<BUILD_INTERFACE:${glslang_SOURCE_DIR}/glslang/Include>
        $<BUILD_INTERFACE:${glslang_SOURCE_DIR}>
        $<BUILD_INTERFACE:${ozz-animation_SOURCE_DIR}/include>
    )

    target_compile_definitions(stratumv PUBLIC
        VK_USE_PLATFORM_WIN32_KHR
        VK_NO_PROTOTYPES
        IMGUI_IMPL_VULKAN_USE_VOLK
        $<$<BOOL:${DLSS_FOUND}>:DLSS_AVAILABLE=1>
        $<$<BOOL:${NTC_FOUND}>:NTC_AVAILABLE=1>
        $<$<BOOL:${SHARC_FOUND}>:SHARC_AVAILABLE=1>
        $<$<BOOL:${MSQUIC_FOUND}>:STRATUMV_MSQUIC_AVAILABLE=1>
    )

    target_link_libraries(stratumv PUBLIC
        glfw
        glm::glm
        volk
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        EnTT::EnTT
        nlohmann_json::nlohmann_json
        glslang::glslang
        glslang::SPIRV
        glslang::glslang-default-resource-limits
        ws2_32
        meshoptimizer
        ufbx
        ozz_base
        ozz_animation
        ozz_animation_offline
        $<$<BOOL:${DLSS_FOUND}>:stratumv_dlss>
        $<$<BOOL:${NTC_FOUND}>:libntc>
        # stratumv PUBLIC-links stratumv_core so everything in Layer 4
        # networking + replication flows to full-engine consumers
        # without duplicating the source list.
        $<$<BOOL:${MSQUIC_FOUND}>:stratumv_core>
    )

    # ── Optional: Jolt Physics concrete implementation ────────────
    if(STRATUMV_ENABLE_JOLT)
        target_sources(stratumv PRIVATE src/engine/JoltPhysicsContext.cpp)
        target_link_libraries(stratumv PUBLIC Jolt)
        target_compile_definitions(stratumv PUBLIC STRATUMV_JOLT_AVAILABLE=1)
        target_include_directories(stratumv PUBLIC
            $<BUILD_INTERFACE:${joltphysics_SOURCE_DIR}/..>
        )
    endif()
endif()  # NOT STRATUMV_CORE_ONLY

# ── Tools ──────────────────────────────────────────────────────────
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(engine_ref
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/engine_ref.py"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating docs/ENGINE_REF.md and docs/engine_state.json"
        VERBATIM
    )
endif()

# ── Install / Export ───────────────────────────────────────────────
# FetchContent targets (glslang, meshoptimizer, etc.) don't ship install(EXPORT)
# rules, so we can't use install(EXPORT StratumVTargets). Instead we install the
# library + headers + a hand-written Config that creates an IMPORTED target.
# Install rules only apply to the full `stratumv` library; the core
# carve-out is an internal build target, not a shipped consumer artifact.
if(NOT STRATUMV_CORE_ONLY)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS stratumv
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Engine headers
    install(DIRECTORY src/engine/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/stratumv
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.inl"
            PATTERN "*.h.in" EXCLUDE
    )

    # Generated version header
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/StratumVVersion.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/stratumv
    )

    # CMake package config
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StratumVConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/StratumVConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StratumV
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/StratumVConfigVersion.cmake"
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/StratumVConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/StratumVConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StratumV
    )
endif()  # NOT STRATUMV_CORE_ONLY

# ── stratumv_server ────────────────────────────────────────────────
# Headless dedicated server binary. Builds unconditionally (unless
# MsQuic is disabled — the subdirectory no-ops in that case). This
# is the first StratumV exe target that lives outside lab/ and
# tests/ and ships as a real user-facing binary. In core-only builds
# it links against stratumv_core; in full builds it links against
# stratumv_core too (never stratumv, because we don't need the full
# engine for a headless dedicated server). See src/stratumv_server/CMakeLists.txt.
if(STRATUMV_ENABLE_MSQUIC)
    add_subdirectory(src/stratumv_server)
endif()

# ── Lab test executables ──────────────────────────────────────────
# Must be added BEFORE tests so tests/CMakeLists.txt can reference the
# skinned_test target via the golden-image fixture. Lab needs the full
# engine (ImGui + Vulkan + ozz).
option(STRATUMV_BUILD_LAB "Build lab test executables" OFF)
if(STRATUMV_BUILD_LAB AND NOT STRATUMV_CORE_ONLY)
    add_subdirectory(lab/skinned_test)
endif()

# ── Example programs ───────────────────────────────────────────────
option(STRATUMV_BUILD_EXAMPLES "Build example programs (examples/)" OFF)
if(STRATUMV_BUILD_EXAMPLES AND NOT STRATUMV_CORE_ONLY)
    add_subdirectory(examples/hello_cube)
endif()

# ── Tests ──────────────────────────────────────────────────────────
if(STRATUMV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
