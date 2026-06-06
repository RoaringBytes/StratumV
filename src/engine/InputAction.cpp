// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "InputAction.h"
#include <cstring>

namespace sv {

static const char* s_actionNames[] = {
    "MoveForward",
    "MoveBack",
    "MoveLeft",
    "MoveRight",
    "Jump",
    "Sprint",
    "Crouch",
    "Interact",
    "Menu",
    "AdminToggle",
    "PlayerModeToggle"
};
static_assert(sizeof(s_actionNames) / sizeof(s_actionNames[0]) == (size_t)Action::COUNT,
              "s_actionNames must match Action enum");

const char* actionName(Action a)
{
    auto idx = (size_t)a;
    if (idx < (size_t)Action::COUNT) return s_actionNames[idx];
    return "Unknown";
}

Action actionFromName(const char* name)
{
    for (size_t i = 0; i < (size_t)Action::COUNT; i++) {
        if (strcmp(s_actionNames[i], name) == 0)
            return (Action)i;
    }
    return Action::COUNT; // invalid
}

} // namespace sv
