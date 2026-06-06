// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "WorldStateTypes.h"
#include <nlohmann/json.hpp>

namespace sv {

// Serialize/deserialize TimeOfDayAnchor, WeatherOverride, BiomeOverride.
// Uses only engine-owned types from WorldStateTypes.h.

nlohmann::json serializeAnchor(const TimeOfDayAnchor& a);
TimeOfDayAnchor deserializeAnchor(const nlohmann::json& j);

nlohmann::json serializeWeather(const WeatherOverride& w);
WeatherOverride deserializeWeather(const nlohmann::json& j);

nlohmann::json serializeBiome(const BiomeOverride& b);
BiomeOverride deserializeBiome(const nlohmann::json& j);

} // namespace sv
