// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "ICameraMode.h"
#include <glm/gtc/matrix_transform.hpp>

namespace sv {

// Free-fly camera: right-click look + WASD/Space/Ctrl movement.
// Exact reproduction of the original Camera behaviour.
class FreeFlyCamera : public ICameraMode {
public:
    void init(GLFWwindow* window);

    void update(GLFWwindow* window, float dt) override;

    glm::mat4 viewMatrix()  const override;
    glm::mat4 projMatrix(float aspect) const override;
    glm::vec3 position()    const override { return m_pos; }
    glm::vec3 lookDirection() const override;

    void setPosition(glm::vec3 p) override { m_pos = p; }
    void setFov(float degrees)    override { m_fov = degrees; }
    float fov() const             override { return m_fov; }

    // FreeFly-specific
    void setSensitivity(float s) { m_sensitivity = s; }
    void setSpeed(float s)       { m_speed = s; }
    float speed() const          { return m_speed; }

private:
    glm::vec3 forward() const;   // XZ plane only
    glm::vec3 right()   const;

    glm::vec3 m_pos{0.0f, 5.0f, 10.0f};
    float m_yaw        = 0.0f;
    float m_pitch      = 0.0f;
    float m_sensitivity = 0.15f;
    float m_speed      = 10.0f;
    float m_fov        = 70.0f;
    bool  m_captured   = false;
    double m_lastX     = 0.0;
    double m_lastY     = 0.0;
    bool  m_firstMouse = true;
};

} // namespace sv
