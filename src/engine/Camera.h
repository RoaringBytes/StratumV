// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "ICameraMode.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

struct GLFWwindow;

namespace sv {

class FreeFlyCamera;   // default mode

// Camera — owns an active ICameraMode, delegates all queries.
// Default mode is FreeFlyCamera (exact reproduction of original behavior).
class Camera {
public:
    void init(GLFWwindow* window);
    void update(GLFWwindow* window, float dt);

    glm::mat4 viewMatrix() const;
    glm::mat4 projMatrix(float aspect) const;
    glm::vec3 position() const;
    glm::vec3 lookDirection() const;

    void setPosition(glm::vec3 p);

    // ── Mode management ──────────────────────────────────────────
    // Takes ownership of the mode.  Returns the previous mode.
    std::unique_ptr<ICameraMode> setMode(std::unique_ptr<ICameraMode> mode);

    ICameraMode*       activeMode()       { return m_mode.get(); }
    const ICameraMode* activeMode() const { return m_mode.get(); }

    // Convenience: construct a mode in-place
    template<typename T, typename... Args>
    T* emplaceMode(Args&&... args) {
        auto m = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = m.get();
        setMode(std::move(m));
        return ptr;
    }

private:
    std::unique_ptr<ICameraMode> m_mode;
};

} // namespace sv
