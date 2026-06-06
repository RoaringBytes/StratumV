# ── stratumv_server ────────────────────────────────────────────────
# Headless StratumV dedicated server exe. Minimal wrapper around
# sv::net::Transport — no Vulkan, no windowing, no rendering. The
# target links against `stratumv_core` (the carve-out) so it
# can build on Linux from just the Layer 4 networking + replication
# subset, without dragging the full graphics stack into the link.
#
# Requires: STRATUMV_ENABLE_MSQUIC=ON (the default). When MsQuic
# is disabled this subdirectory is a no-op — the root CMakeLists
# skips it via the same guard.

if(NOT STRATUMV_ENABLE_MSQUIC)
    message(STATUS "[StratumV] stratumv_server skipped "
        "(STRATUMV_ENABLE_MSQUIC=OFF)")
    return()
endif()

add_executable(stratumv_server
    main.cpp
)

# Link against stratumv_core (Layer 4 networking + replication
# carve-out). stratumv_core's PUBLIC include dirs + compile defs +
# link libs propagate through transparently — no need to also link
# against the full `stratumv` target even when it is present.
target_link_libraries(stratumv_server PRIVATE
    stratumv_core
)

# NOMINMAX + WIN32_LEAN_AND_MEAN prevent the stray std::min/std::max
# collisions from the MsQuic Windows headers (which transitively
# include <windows.h> and would otherwise define min/max macros).
# Harmless on Linux.
target_compile_definitions(stratumv_server PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(stratumv_server PRIVATE /bigobj)
    set_target_properties(stratumv_server PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ── Linux: rpath so libmsquic.so resolves from the tarball ───────
# STRATUMV_LINUX_MSQUIC_ROOT points at the extracted prebuilt MsQuic
# tarball — set the executable's RPATH to the lib dir inside it so
# the Linux dynamic loader finds libmsquic.so at runtime without the
# user having to set LD_LIBRARY_PATH by hand. docs/LINUX_SERVER.md
# explains the fallback LD_LIBRARY_PATH approach.
if(UNIX AND NOT APPLE AND STRATUMV_LINUX_MSQUIC_ROOT)
    # Pick the directory that actually contains libmsquic.so — the
    # MsQuic tarball uses bin/ in most releases but some roll lib/.
    set(_sv_msquic_rpath "")
    if(EXISTS "${STRATUMV_LINUX_MSQUIC_ROOT}/bin")
        list(APPEND _sv_msquic_rpath "${STRATUMV_LINUX_MSQUIC_ROOT}/bin")
    endif()
    if(EXISTS "${STRATUMV_LINUX_MSQUIC_ROOT}/lib")
        list(APPEND _sv_msquic_rpath "${STRATUMV_LINUX_MSQUIC_ROOT}/lib")
    endif()
    if(_sv_msquic_rpath)
        set_target_properties(stratumv_server PROPERTIES
            INSTALL_RPATH       "${_sv_msquic_rpath}"
            BUILD_RPATH         "${_sv_msquic_rpath}"
            BUILD_WITH_INSTALL_RPATH TRUE)
    endif()
endif()

# msquic.dll must ship next to the exe on Windows or LoadLibrary
# fails at startup. On Linux this function is a no-op because the
# RPATH above handles loader resolution. stratumv_copy_msquic_dll
# is defined by the root CMakeLists.txt inside the MsQuic block.
stratumv_copy_msquic_dll(stratumv_server)

message(STATUS "[StratumV] stratumv_server enabled (links stratumv_core)")
