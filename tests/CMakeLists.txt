# ── StratumV engine unit tests ─────────────────────────────────────
# Builds Catch2 v3 test executables linked against stratumv.lib or
# stratumv_core. Tests are registered with ctest via catch_discover_tests().
#
# Enable with: -DSTRATUMV_BUILD_TESTS=ON
# Run with:    ctest --test-dir build --output-on-failure
#
# ── Two-binary test split ──────────────────────────────────────────
# The test lineup is split into two executables so the boundary-probe
# TU (test_StratumVCore.cpp) can be compiled with an isolated include
# path that REALLY does not see Vulkan/glm/ImGui/ozz:
#
#   sv_core_tests (always built when MSQUIC is on):
#     Links ONLY against stratumv_core. Include path is limited to
#     src/, src/engine/, generated/, and MSQUIC_INCLUDE_DIR — nothing
#     else. The boundary-probe TU's __has_include(<volk.h>) check
#     therefore correctly evaluates to false on full builds too. The
#     Layer 4 tests (ReplicationRegistry, ReplicationWire, MsQuic-
#     Transport, StratumVCore) all live here so the Linux carve-out
#     can run the same binary.
#
#   sv_tests (only built when NOT STRATUMV_CORE_ONLY):
#     Links against stratumv (which transitively inherits the full
#     Vulkan/ImGui/ozz include stack via its PUBLIC include dirs).
#     Holds the graphics-facing tests (FrustumCuller, BlendTree,
#     AnimationStateMachine, AssetBrowser, PerformanceContext,
#     NetworkContext, ... plus everything that pulls BaseSystemContext.h
#     into its translation unit).
#
# Both binaries register with the same ctest. On a full build the dev
# runs `ctest` and gets all tests from both binaries. On a core-only
# build only sv_core_tests exists.

include(FetchContent)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# Match stratumv's MSVC runtime (MD/MDd) to avoid CRT mismatch warnings.
if(MSVC)
    if(TARGET Catch2)
        set_target_properties(Catch2 PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    if(TARGET Catch2WithMain)
        set_target_properties(Catch2WithMain PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

enable_testing()

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(CTest)
include(Catch)

# ── sv_core_tests: Layer 4 core subset (always built) ─────────────
# Links ONLY against stratumv_core so the boundary-probe TU's
# __has_include() checks see an accurate picture of the core surface.
if(STRATUMV_ENABLE_MSQUIC)
    set(SV_CORE_TEST_SOURCES
        test_ReplicationRegistry.cpp
        test_ReplicationWire.cpp
        test_MsQuicTransport.cpp
        test_StratumVCore.cpp         # core boundary probe
        test_EditTransaction.cpp      # edit tx + undo log
                                      # + welcome + generic payload dispatch
        test_WorldPersistence.cpp     # server-side world snapshot
                                      # binary format round-trip + error paths
        test_AssetPersistence.cpp     # SHA-256 + CAS + Announce/
                                      # Chunk/Ack wire + upload client
        test_LightComponent.cpp       # LightComponent
                                      # registration + Editor authority +
                                      # full/partial mask SetField round-trip
        test_CameraComponent.cpp      # CameraComponent
                                      # — same shape as LightComponent
        test_MaterialComponent.cpp    # MaterialComponent
                                      # — single-demo override (basecolor +
                                      # strength), Editor authority
    )

    add_executable(sv_core_tests ${SV_CORE_TEST_SOURCES})

    target_link_libraries(sv_core_tests PRIVATE
        stratumv_core
        Catch2::Catch2WithMain
    )

    target_compile_definitions(sv_core_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    if(MSVC)
        target_compile_options(sv_core_tests PRIVATE /bigobj)
        target_compile_options(sv_core_tests PRIVATE /wd4244 /wd4267)
        set_target_properties(sv_core_tests PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    stratumv_copy_msquic_dll(sv_core_tests)

    catch_discover_tests(sv_core_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TEST_PREFIX "core:"
    )

    message(STATUS "[StratumV] sv_core_tests enabled (Layer 4 subset, "
        "links stratumv_core only)")
endif()

# ── sv_tests: Graphics-facing tests (only on full builds) ─────────
# Holds every test that pulls BaseSystemContext.h or any other
# graphics-stack header into its TU. Linked against the full
# `stratumv` target. Only built when the full engine is present.
if(NOT STRATUMV_CORE_ONLY)
    set(SV_TEST_SOURCES
        test_FrustumCuller.cpp
        test_AnimationStateMachine.cpp
        test_BlendTree.cpp
        test_SceneNode.cpp
        test_CC5Sidecar.cpp
        test_MeshImportData.cpp
        test_MockContext.cpp
        test_SceneLoader.cpp          # getSpawnPoints filter
        test_AssetManifest.cpp        # manifest JSON parse + lookup
        test_Config_WorldBounds.cpp   # Config::worldBounds + vec3 helpers
        test_AssetBrowser.cpp         # directory scan + import cache
        test_AssetWatcher.cpp         # recursive directory watch
        test_ThumbnailCache.cpp       # thumbnail cache + sibling files
        test_PerformanceContext.cpp   # PerformanceBudget + NetworkStats
        test_SceneStateVersioning.cpp # migration chains + range clamping
        test_PipelineCacheIO.cpp      # VkPipelineCache header + file I/O
        test_NetworkContext.cpp       # NetworkContext sub-struct shape
                                      #   (pulls BaseSystemContext.h → volk/glm/ImGui)
        test_EditorBridgeWire.cpp     # explicit bridge wire
                                      # types (asset announce/chunk + setparent)
                                      # — pulls EditorBridge.h which is a full-
                                      # engine target, not in the core subset.
    )

    add_executable(sv_tests ${SV_TEST_SOURCES})

    target_link_libraries(sv_tests PRIVATE
        stratumv
        Catch2::Catch2WithMain
    )

    # Pass the fixtures directory to tests so they can locate JSON fixtures
    # without depending on the working directory of the test runner.
    target_compile_definitions(sv_tests PRIVATE
        SV_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        SV_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    if(MSVC)
        target_compile_options(sv_tests PRIVATE /bigobj)
        target_compile_options(sv_tests PRIVATE /wd4244 /wd4267)
        set_target_properties(sv_tests PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    stratumv_copy_msquic_dll(sv_tests)

    catch_discover_tests(sv_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    message(STATUS "[StratumV] sv_tests enabled (graphics-facing subset, "
        "links stratumv)")
endif()

# ── Golden-image regression tests ──────────────────────────────────
# The golden tests live in a SEPARATE exe (sv_golden_tests) so we
# can register them as plain add_test() entries with a CTest
# fixture. catch_discover_tests defers test registration to build
# time, which makes set_tests_properties(FIXTURES_REQUIRED) on
# individual Catch2 cases fail at configure time — the separate
# binary sidesteps the issue entirely.
#
# Gating: STRATUMV_GOLDEN_TESTS is OFF by default — enable with
# -DSTRATUMV_GOLDEN_TESTS=ON. Requires STRATUMV_BUILD_LAB=ON so
# the skinned_test target exists. Also requires NOT
# STRATUMV_CORE_ONLY (skinned_test needs the full engine).
option(STRATUMV_GOLDEN_TESTS "Enable render-regression golden image tests" OFF)

if(STRATUMV_GOLDEN_TESTS AND NOT STRATUMV_CORE_ONLY)
    if(NOT TARGET skinned_test)
        message(WARNING "[StratumV] STRATUMV_GOLDEN_TESTS=ON but skinned_test "
            "target is not defined — enable STRATUMV_BUILD_LAB to build the "
            "capture rig. Golden tests will be skipped.")
    else()
        add_executable(sv_golden_tests
            test_RenderGolden.cpp
        )
        target_link_libraries(sv_golden_tests PRIVATE
            stratumv
            Catch2::Catch2WithMain
        )
        target_compile_definitions(sv_golden_tests PRIVATE
            SV_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
        if(MSVC)
            target_compile_options(sv_golden_tests PRIVATE /bigobj)
            target_compile_options(sv_golden_tests PRIVATE /wd4244 /wd4267)
        endif()

        # Ensure msquic.dll ships next to the golden test binary too —
        # stratumv.lib carries the transport import refs.
        stratumv_copy_msquic_dll(sv_golden_tests)

        set(SV_GOLDEN_CAPTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/golden_captures")
        file(MAKE_DIRECTORY "${SV_GOLDEN_CAPTURE_DIR}")

        # The capture step renders a known skinned mesh; that input
        # (assets/sample_character.fbx) is a binary asset that is not
        # shipped with the public repository. Register the golden tests
        # only when the mesh is present so a fresh clone stays green
        # (the suite is simply absent) instead of reporting hard
        # failures. Drop the mesh in and re-run CMake to enable it. As a
        # second line of defence the capture also self-skips at runtime
        # via SKIP_RETURN_CODE if the mesh disappears after configure.
        set(SV_GOLDEN_INPUT_MESH "${CMAKE_SOURCE_DIR}/assets/sample_character.fbx")
        if(NOT EXISTS "${SV_GOLDEN_INPUT_MESH}")
            message(STATUS "[StratumV] Golden image tests DISABLED — input mesh "
                "'assets/sample_character.fbx' not present. Provide it and "
                "re-run CMake to enable the render-regression suite.")
        else()
        add_test(
            NAME render_golden_capture
            COMMAND $<TARGET_FILE:skinned_test> --render-golden "${SV_GOLDEN_CAPTURE_DIR}"
            # WORKING_DIRECTORY is the REPO ROOT so the relative font
            # path ImGui loads ("build/_deps/imgui-src/misc/fonts/
            # Roboto-Medium.ttf") resolves the same way it does when a
            # developer manually regenerates goldens from the repo
            # root via `skinned_test --render-golden tests/golden/`.
            # A different CWD loads ImGui's embedded default font,
            # producing a ~44 RMS mismatch in imgui_layer.png.
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(render_golden_capture PROPERTIES
            FIXTURES_SETUP   golden_fixture
            ENVIRONMENT      "SV_GOLDEN_CAPTURE_DIR=${SV_GOLDEN_CAPTURE_DIR}"
            SKIP_RETURN_CODE 125
            TIMEOUT          60
            LABELS           "golden"
        )

        # One ctest entry per Catch2 test case, all routed through
        # sv_golden_tests and filtered by name. This preserves the
        # per-pass granularity in ctest output (four PASS/FAIL lines
        # instead of one) while keeping the configure-time wiring
        # simple (no catch_discover_tests for these cases).
        function(_sv_add_golden_test name catch_spec)
            add_test(
                NAME ${name}
                COMMAND $<TARGET_FILE:sv_golden_tests> ${catch_spec}
            )
            set_tests_properties(${name} PROPERTIES
                FIXTURES_REQUIRED golden_fixture
                ENVIRONMENT       "SV_GOLDEN_CAPTURE_DIR=${SV_GOLDEN_CAPTURE_DIR}"
                LABELS            "golden"
                TIMEOUT           30
            )
        endfunction()

        _sv_add_golden_test(render_golden_skinned_mesh
            "render_golden: SkinnedMeshPass bake matches reference")
        _sv_add_golden_test(render_golden_shadow_pass
            "render_golden: ShadowPass depth readback matches reference")
        _sv_add_golden_test(render_golden_post_process
            "render_golden: PostProcess CPU tonemap matches reference")
        _sv_add_golden_test(render_golden_imgui_layer
            "render_golden: ImGuiLayer offscreen render matches reference")

        message(STATUS "[StratumV] Golden tests enabled "
            "(capture dir: ${SV_GOLDEN_CAPTURE_DIR})")
        endif()
    endif()
endif()
