// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

namespace sv {

// Camera mode interface — games can implement custom modes.
// Engine provides: FreeFlyCamera, FollowCamera, OrbitCamera.
class ICameraMode {
public:
    virtual ~ICameraMode() = default;

    // Per-frame update.  Modes read input from window directly.
    virtual void update(GLFWwindow* window, float dt) = 0;

    // Output matrices / vectors
    virtual glm::mat4 viewMatrix()  const = 0;
    virtual glm::mat4 projMatrix(float aspect) const = 0;
    virtual glm::vec3 position()    const = 0;
    virtual glm::vec3 lookDirection() const = 0;

    // Setters the owning Camera may call
    virtual void setPosition(glm::vec3 p) = 0;
    virtual void setFov(float degrees) = 0;
    virtual float fov() const = 0;
};

} // namespace sv
