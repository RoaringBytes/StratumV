// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "ICameraMode.h"
#include <glm/gtc/matrix_transform.hpp>

namespace sv {

// Third-person follow camera: tracks a target position with configurable offset and smoothing.
// Right-click rotates the view around the target.
class FollowCamera : public ICameraMode {
public:
    void update(GLFWwindow* window, float dt) override;

    glm::mat4 viewMatrix()  const override;
    glm::mat4 projMatrix(float aspect) const override;
    glm::vec3 position()    const override { return m_currentPos; }
    glm::vec3 lookDirection() const override;

    void setPosition(glm::vec3 p) override { m_currentPos = p; }
    void setFov(float degrees)    override { m_fov = degrees; }
    float fov() const             override { return m_fov; }

    // Follow-specific configuration
    void  setTarget(glm::vec3 t)          { m_target = t; }
    void  setOffset(glm::vec3 o)          { m_offset = o; }
    void  setSmoothSpeed(float s)         { m_smoothSpeed = s; }
    void  setSensitivity(float s)         { m_sensitivity = s; }
    void  setMinDistance(float d)          { m_minDist = d; }
    void  setMaxDistance(float d)          { m_maxDist = d; }

    glm::vec3 target()      const { return m_target; }
    glm::vec3 offset()      const { return m_offset; }
    float     smoothSpeed() const { return m_smoothSpeed; }

private:
    // Target being followed (game sets this each frame)
    glm::vec3 m_target{0.0f};

    // Offset from target in local space (y = height, z = behind)
    glm::vec3 m_offset{0.0f, 3.0f, 8.0f};

    // Smoothed camera position
    glm::vec3 m_currentPos{0.0f, 8.0f, 18.0f};

    // Orbit angles (right-click to adjust)
    float m_yaw        = 0.0f;
    float m_pitch      = 15.0f;
    float m_sensitivity = 0.15f;
    float m_smoothSpeed = 5.0f;
    float m_fov        = 70.0f;
    float m_minDist    = 2.0f;
    float m_maxDist    = 50.0f;

    // Mouse capture state
    bool   m_captured   = false;
    double m_lastX      = 0.0;
    double m_lastY      = 0.0;
    bool   m_firstMouse = true;
};

} // namespace sv
