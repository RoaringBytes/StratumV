# hello_cube — minimal StratumV example (renders a procedural cube)
# Build: cmake -B build -G "Visual Studio 17 2022" -A x64 -DSTRATUMV_BUILD_EXAMPLES=ON
#        cmake --build build --config Release --target hello_cube

add_executable(hello_cube
    main.cpp

    # ImGui sources: stratumv.lib provides ImGui headers only, so consumers
    # compile the sources. hello_cube uses no ImGui itself, but linking the
    # engine pulls these symbols. (Same pattern as the lab + scaffold.)
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_link_libraries(hello_cube PRIVATE stratumv)

target_compile_definitions(hello_cube PRIVATE
    NOMINMAX
    GLM_FORCE_DEPTH_ZERO_TO_ONE      # Vulkan NDC depth is [0,1]
)

# Kill any running instance before linking (avoids LNK1104 on rebuild)
add_custom_command(TARGET hello_cube PRE_LINK
    COMMAND powershell -Command "Stop-Process -Name hello_cube -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 300" || ${CMAKE_COMMAND} -E true
    COMMENT "Stopping running hello_cube instance"
)

# Shaders are compiled from GLSL at runtime, loaded relative to the CWD.
# Copy them next to the exe so it runs from its own directory.
add_custom_command(TARGET hello_cube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hello_cube>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hello_cube.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hello_cube.frag"
        "$<TARGET_FILE_DIR:hello_cube>/shaders/"
    COMMENT "Copying hello_cube shaders"
)

# Ship msquic.dll next to the exe (stratumv.lib may pull MsQuicTransport).
# No-op when STRATUMV_ENABLE_MSQUIC=OFF.
stratumv_copy_msquic_dll(hello_cube)
