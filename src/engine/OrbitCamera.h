// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "ICameraMode.h"
#include <glm/gtc/matrix_transform.hpp>

namespace sv {

// Orbit camera: orbits around a fixed point.
// Right-click to orbit.  Scroll to zoom in/out.
class OrbitCamera : public ICameraMode {
public:
    void update(GLFWwindow* window, float dt) override;

    glm::mat4 viewMatrix()  const override;
    glm::mat4 projMatrix(float aspect) const override;
    glm::vec3 position()    const override;
    glm::vec3 lookDirection() const override;

    void setPosition(glm::vec3 p) override;   // sets orbit center
    void setFov(float degrees)    override { m_fov = degrees; }
    float fov() const             override { return m_fov; }

    // Orbit-specific configuration
    void  setCenter(glm::vec3 c)   { m_center = c; }
    void  setDistance(float d)     { m_distance = d; }
    void  setMinDistance(float d)  { m_minDist = d; }
    void  setMaxDistance(float d)  { m_maxDist = d; }
    void  setSensitivity(float s)  { m_sensitivity = s; }
    void  setZoomSpeed(float s)    { m_zoomSpeed = s; }
    void  setYawPitch(float yaw, float pitch) { m_yaw = yaw; m_pitch = pitch; }

    glm::vec3 center()   const { return m_center; }
    float     distance() const { return m_distance; }

    // Call with scroll delta each frame (positive = zoom in)
    void applyScrollZoom(float scrollDelta);

private:
    glm::vec3 computeEyePos() const;

    glm::vec3 m_center{0.0f};
    float m_distance    = 15.0f;
    float m_yaw         = 0.0f;
    float m_pitch       = 20.0f;
    float m_sensitivity = 0.15f;
    float m_zoomSpeed   = 1.5f;
    float m_fov         = 70.0f;
    float m_minDist     = 1.0f;
    float m_maxDist     = 200.0f;

    // Mouse capture state
    bool   m_captured   = false;
    double m_lastX      = 0.0;
    double m_lastY      = 0.0;
    bool   m_firstMouse = true;
};

} // namespace sv
