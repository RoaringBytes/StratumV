// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <cstdint>

// DLL-safe audio types — no miniaudio dependencies.
// Used by SystemContext function pointers across the DLL boundary.

namespace sv {

// Opaque handle to an active sound instance (0 = invalid)
using SoundHandle = uint32_t;
constexpr SoundHandle INVALID_SOUND = 0;

// Volume buses — each has independent volume control, all multiplied by Master
enum class SoundBus : int {
    Master  = 0,
    SFX     = 1,
    Ambient = 2,
    Music   = 3,
    UI      = 4,
    COUNT   = 5
};

// Variant file selection mode
enum class VariantMode : int {
    Random     = 0,  // uniform random pick
    RoundRobin = 1,  // sequential cycle
    Shuffle    = 2   // random permutation, no repeats until all played
};

} // namespace sv
