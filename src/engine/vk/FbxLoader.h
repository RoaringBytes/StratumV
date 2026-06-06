// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "MeshImportData.h"
#include <string>

namespace sv {

// Load an FBX file into CPU-side mesh data.
// Parses CC5 JSON sidecar automatically if present.
bool loadFbx(const std::string& path, bool loadTextures, MeshImportData& out);

} // namespace sv
