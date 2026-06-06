# Third-Party Notices

StratumV is licensed under the Apache License 2.0 (see `LICENSE`). It builds
on a number of third-party open-source components, each the property of its
respective authors and used under the terms of its own license. Most are
fetched at configure time via CMake `FetchContent` rather than vendored into
this repository.

This file is provided for attribution and informational purposes. Nothing
here modifies the terms under which StratumV itself is distributed.

## Runtime / engine dependencies (fetched by default)

| Component | Version | License | Project |
|-----------|---------|---------|---------|
| nlohmann/json | v3.11.3 | MIT | https://github.com/nlohmann/json |
| GLFW | 3.4 | Zlib | https://github.com/glfw/glfw |
| GLM | 1.0.1 | MIT | https://github.com/g-truc/glm |
| Vulkan-Headers | v1.4.313 | Apache-2.0 | https://github.com/KhronosGroup/Vulkan-Headers |
| volk | vulkan-sdk-1.4.313.0 | MIT | https://github.com/zeux/volk |
| VulkanMemoryAllocator | v3.2.1 | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| EnTT | v3.14.0 | MIT | https://github.com/skypjack/entt |
| tinygltf | v2.9.5 | MIT | https://github.com/syoyo/tinygltf |
| meshoptimizer | v0.22 | MIT | https://github.com/zeux/meshoptimizer |
| ufbx | v0.21.3 | MIT | https://github.com/ufbx/ufbx |
| glslang | 15.1.0 | BSD-3-Clause (and others) | https://github.com/KhronosGroup/glslang |
| Dear ImGui | v1.91.9 | MIT | https://github.com/ocornut/imgui |
| miniaudio | 0.11.21 | Public Domain (Unlicense) / MIT-0 | https://github.com/mackron/miniaudio |
| ozz-animation | 0.16.0 | MIT | https://github.com/guillaumeblanc/ozz-animation |
| MsQuic | 2.5.6 | MIT | https://github.com/microsoft/msquic |

`tinygltf` transitively bundles `stb_image` / `stb_image_write` (public
domain / MIT) and a copy of nlohmann/json.

## Optional dependencies (OFF by default)

| Component | Version | License | Enabled by | Project |
|-----------|---------|---------|------------|---------|
| Jolt Physics | v5.2.0 | MIT | `STRATUMV_ENABLE_JOLT` | https://github.com/jrouwe/JoltPhysics |

### Proprietary NVIDIA components (OFF by default — not redistributed)

The following are **disabled by default** and are **not** included in this
repository. Enabling them fetches (or, for DLSS, requires you to supply)
code governed by NVIDIA's own license terms, which are **not** covered by
StratumV's Apache-2.0 license. You are responsible for accepting NVIDIA's
terms before enabling these features.

| Component | License | Enabled by | Project |
|-----------|---------|------------|---------|
| NVIDIA DLSS / NGX SDK | NVIDIA proprietary EULA | `STRATUMV_ENABLE_DLSS` (user-provided in `external/dlss/`) | https://developer.nvidia.com/dlss |
| NVIDIA SHARC | NVIDIA RTX SDK License | `STRATUMV_ENABLE_SHARC` | https://github.com/NVIDIA-RTX/SHARC |
| NVIDIA RTXNTC-Library | NVIDIA RTX SDK License | `STRATUMV_ENABLE_NTC` | https://github.com/NVIDIA-RTX/RTXNTC-Library |

## Test-only dependencies

| Component | Version | License | Project |
|-----------|---------|---------|---------|
| Catch2 | v3.7.1 | BSL-1.0 | https://github.com/catchorg/Catch2 |

Full license texts for each component are available at the linked project
repositories. Pinned versions are authoritative as declared in `CMakeLists.txt`.
