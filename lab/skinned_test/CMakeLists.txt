# Skinned mesh T-pose test
# Build: cmake -B build -G "Visual Studio 17 2022" -A x64 -DSTRATUMV_BUILD_LAB=ON
#        cmake --build build --config Release --target skinned_test

add_executable(skinned_test
    main.cpp

    # ImGui sources (header-only in stratumv.lib — consumers compile)
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(skinned_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}   # SceneUBO.h
)

target_link_libraries(skinned_test PRIVATE stratumv)

# Mirror the test fixture tree into the lab build dir so the
# AssetBrowser demo can bake .thumb.png siblings without polluting the
# committed source tree (invariant: thumbnails must never leak
# back into the asset_browser/ fixture tree).
set(LAB_DEMO_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/asset_browser_demo")
target_compile_definitions(skinned_test PRIVATE
    NOMINMAX
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    STRATUMV_LAB_ASSET_DIR="${LAB_DEMO_ASSETS}"
)

# Kill any running instance before linking (avoids LNK1104 on rebuild)
add_custom_command(TARGET skinned_test PRE_LINK
    COMMAND powershell -Command "Stop-Process -Name skinned_test -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500" || ${CMAKE_COMMAND} -E true
    COMMENT "Stopping running skinned_test instance"
)

# Copy shaders + config to output directory
add_custom_command(TARGET skinned_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:skinned_test>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/shaders/skinned.vert"
        "${CMAKE_SOURCE_DIR}/shaders/skinned.frag"
        "${CMAKE_SOURCE_DIR}/shaders/skinnedPBR.frag"
        "$<TARGET_FILE_DIR:skinned_test>/shaders/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
        "$<TARGET_FILE_DIR:skinned_test>/config.json"
    COMMENT "Copying shaders and config for skinned_test"
)

# Mirror the asset_browser fixture tree into the build dir so bakes
# write to a writable copy. POST_BUILD instead of one-shot at
# configure time so the copy refreshes whenever the lab is rebuilt and
# the source fixtures have changed (copy_directory is idempotent).
add_custom_command(TARGET skinned_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/tests/fixtures/asset_browser"
        "${LAB_DEMO_ASSETS}"
    COMMENT "Mirroring asset_browser fixtures for skinned_test"
)

# Ship msquic.dll next to skinned_test so consumers that incidentally
# pull MsQuicTransport.obj through stratumv.lib don't
# hit a missing-DLL loader error. No-op if STRATUMV_ENABLE_MSQUIC=OFF.
stratumv_copy_msquic_dll(skinned_test)
