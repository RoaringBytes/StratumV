// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "MeshImportData.h"
#include <string>

namespace sv {

// Load a glTF/GLB file into CPU-side mesh data.
bool loadGltf(const std::string& path, bool loadTextures, MeshImportData& out);

} // namespace sv
