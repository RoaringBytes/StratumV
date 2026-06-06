// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "InputAction.h"
#include <array>
#include <string>

namespace sv {

// Stores the full action→binding map. Supports JSON persistence.
class InputBindings {
public:
    void loadDefaults();
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path) const;

    const ActionBinding& get(Action a) const { return m_bindings[(size_t)a]; }
    void set(Action a, const ActionBinding& b) { m_bindings[(size_t)a] = b; }

private:
    std::array<ActionBinding, (size_t)Action::COUNT> m_bindings{};
};

} // namespace sv
