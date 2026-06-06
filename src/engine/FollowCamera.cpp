// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "FollowCamera.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

namespace sv {

void FollowCamera::update(GLFWwindow* window, float dt)
{
    // ── Right-click orbit control ────────────────────────────────
    bool rightDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    if (rightDown && !m_captured) {
        m_captured = true;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(window, &m_lastX, &m_lastY);
        m_firstMouse = true;
    } else if (!rightDown && m_captured) {
        m_captured = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    if (m_captured) {
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);

        if (m_firstMouse) {
            m_lastX = mx;
            m_lastY = my;
            m_firstMouse = false;
        }

        float dx = (float)(mx - m_lastX);
        float dy = (float)(my - m_lastY);
        m_lastX = mx;
        m_lastY = my;

        m_yaw   += dx * m_sensitivity;
        m_pitch -= dy * m_sensitivity;
        m_pitch  = std::clamp(m_pitch, -60.0f, 80.0f);
    }

    // ── Scroll to adjust distance ────────────────────────────────
    // (Scroll handled externally via Input system; offset.z adjustable by game)

    // ── Compute desired camera position ──────────────────────────
    float yawRad   = glm::radians(m_yaw);
    float pitchRad = glm::radians(m_pitch);

    float dist = glm::length(m_offset);
    dist = std::clamp(dist, m_minDist, m_maxDist);

    glm::vec3 offsetDir{
        sinf(yawRad) * cosf(pitchRad),
        sinf(pitchRad),
        cosf(yawRad) * cosf(pitchRad)
    };

    glm::vec3 desiredPos = m_target + offsetDir * dist;
    desiredPos.y += m_offset.y;  // additional height offset

    // ── Smooth interpolation ─────────────────────────────────────
    float t = 1.0f - expf(-m_smoothSpeed * dt);
    m_currentPos = glm::mix(m_currentPos, desiredPos, t);
}

glm::vec3 FollowCamera::lookDirection() const
{
    return glm::normalize(m_target - m_currentPos);
}

glm::mat4 FollowCamera::viewMatrix() const
{
    return glm::lookAt(m_currentPos, m_target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 FollowCamera::projMatrix(float aspect) const
{
    return glm::perspective(glm::radians(m_fov), aspect, 0.5f, 10000.0f);
}

} // namespace sv
