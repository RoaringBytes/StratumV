# StratumV Unit Tests (S-T1)

Catch2 v3 test harness for engine logic that does not require a live Vulkan
device. Enabled with `-DSTRATUMV_BUILD_TESTS=ON`.

## Build and run

```sh
# From the repo root
cmake -B build -G "Visual Studio 17 2022" -A x64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DSTRATUMV_BUILD_TESTS=ON \
    -DSTRATUMV_ENABLE_SHARC=OFF -DSTRATUMV_ENABLE_NTC=OFF

cmake --build build --config Release --target sv_tests --parallel

# Run via ctest (parallel, stops on first failure)
ctest --test-dir build --output-on-failure -C Release

# Or run the test binary directly (lists tags and supports filtering)
./build/tests/Release/sv_tests.exe            # all tests
./build/tests/Release/sv_tests.exe [frustum]  # one tag
./build/tests/Release/sv_tests.exe "*AABB*"   # one test name pattern
./build/tests/Release/sv_tests.exe --list-tests
```

## What's covered

| File                          | Target                       | Cases |
|-------------------------------|------------------------------|-------|
| `test_FrustumCuller.cpp`      | `FrustumCuller`              | ~18   |
| `test_AnimationStateMachine.cpp` | `AnimationStateMachine`    | ~19   |
| `test_BlendTree.cpp`          | `BlendSpace1D`, `buildJointMask` | ~10 |
| `test_SceneNode.cpp`          | `SceneNode::localMatrix`, hierarchy math | ~11 |
| `test_CC5Sidecar.cpp`         | `parseCC5Sidecar`            | ~8    |
| `test_MeshImportData.cpp`     | `MeshImportData` invariants  | ~6    |
| `test_MockContext.cpp`        | `BaseSystemContext` mock pattern | ~3 |

## Fixtures

`tests/fixtures/` holds input JSON files. Tests reach them via the
`SV_TEST_FIXTURES_DIR` macro (set by `tests/CMakeLists.txt`) so they don't
depend on the working directory of the test runner.

Current fixtures:
- `cc5_sample.json` — 4-material CC5 export with hair + eyelash transparency
- `cc5_empty.json`  — malformed-but-parseable structure that yields an empty map

## Writing new tests

1. Add a `test_*.cpp` file under `tests/`.
2. Add its name to `SV_TEST_SOURCES` in `tests/CMakeLists.txt`.
3. Tag tests with short, grep-friendly labels: `[frustum]`, `[anim-sm]`,
   `[cc5]`, `[blendtree]`, `[scene-node]`, `[mesh-import]`, `[mock]`.
4. Prefer pure logic under test — anything that needs a `VkDevice`,
   `VkCtx`, or live Vulkan is out of scope for unit tests.

## Mocking `BaseSystemContext`

`BaseSystemContext` has 133 fields, most of which are `std::function`
callbacks. Do **not** build a full mock subclass — default-construct the
struct and only assign the lambdas your unit under test actually calls.

See `test_MockContext.cpp` for the reference pattern:

```cpp
BaseSystemContext ctx{};
int callCount = 0;
ctx.isKeyDown = [&callCount](int key) {
    callCount++;
    return key == 32;
};

MyPlugin plugin;
plugin.tick(ctx);
REQUIRE(callCount == 1);
```

Rules of thumb:
- `std::function` members default-construct to empty — check with `if (ctx.foo)` before calling.
- Leave every other field at its zero/null default.
- Capture by reference for call counters and argument recording.
- If a test needs many fields, extract a helper that builds a populated mock
  in one call.

## Why Catch2 v3

- Header-only API with `TEST_CASE` / `SECTION` macros.
- First-class CMake integration via `catch_discover_tests()` → individual
  test cases show up as separate ctest entries (parallel execution).
- Fetched via `FetchContent` (same pattern as the rest of the engine deps).

## Not yet covered

These require additional infrastructure and are deferred to follow-up sessions:

- Full `SceneLoader::loadFromFile` pipeline (needs `VkCtx`).
- `VkMesh` GPU upload path (needs `VkCtx`).
- `AnimationSystem` runtime jobs (needs `VkDevice`).
- DLL plugin hot-reload round-trip.
- FBX / glTF loaders end-to-end on real character assets.
