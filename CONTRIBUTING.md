# Contributing to StratumV

Thanks for your interest in contributing! StratumV is a layered Vulkan 1.3
game engine written in C++20 and licensed under Apache-2.0.

## Prerequisites

- Windows 10/11 with a Vulkan 1.3-capable GPU and current drivers
  (the dedicated server also builds headless on Linux).
- Visual Studio 2022 (MSVC v143).
- CMake 3.24+.
- The [Vulkan SDK](https://vulkan.lunarg.com/) for validation layers and
  shader tooling.

Most third-party dependencies (GLFW, GLM, Vulkan-Headers, volk, VMA, EnTT,
glslang, ImGui, tinygltf, ufbx, meshoptimizer, miniaudio, ozz-animation,
MsQuic) are fetched automatically at configure time via CMake `FetchContent`.

## Building

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

## Running the test gate locally

Before opening a pull request, please make sure the build **and** the full
test suite pass locally. This is the same gate the project uses for changes:

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DSTRATUMV_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

A change is considered green only if the build succeeds **and** every
`ctest` test passes.

## CMake options

| Option | Default | Notes |
|--------|---------|-------|
| `STRATUMV_BUILD_TESTS` | `OFF` | Build and register the Catch2 test suite |
| `STRATUMV_ENABLE_MSQUIC` | `ON` | QUIC transport for networking/replication |
| `STRATUMV_ENABLE_JOLT` | `OFF` | Jolt physics backend |
| `STRATUMV_ENABLE_DLSS` / `_SHARC` / `_NTC` | `OFF` | Optional **proprietary** NVIDIA SDKs |
| `STRATUMV_CORE_ONLY` | `OFF` | Headless networking core + dedicated server |

## Pull requests

- Branch off `main` and keep PRs focused on a single concern.
- Describe **what** changed and **why**, and paste your local build + `ctest`
  summary so reviewers can see it passed the gate.
- Keep commits readable; use clear, conventional commit messages
  (e.g. `fix(lab): ...`, `ci: ...`, `docs: ...`).
- By contributing, you agree your contributions are licensed under the
  project's Apache-2.0 license.

## Reporting bugs and security issues

- For functional bugs, open a GitHub issue with reproduction steps.
- For **security** vulnerabilities, follow [`SECURITY.md`](SECURITY.md) and
  use private vulnerability reporting — do not open a public issue.
