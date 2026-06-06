// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "OrbitCamera.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

namespace sv {

void OrbitCamera::applyScrollZoom(float scrollDelta)
{
    m_distance -= scrollDelta * m_zoomSpeed;
    m_distance  = std::clamp(m_distance, m_minDist, m_maxDist);
}

void OrbitCamera::update(GLFWwindow* window, float dt)
{
    (void)dt;

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
        m_pitch  = std::clamp(m_pitch, -89.0f, 89.0f);
    }

    // Clamp distance
    m_distance = std::clamp(m_distance, m_minDist, m_maxDist);
}

glm::vec3 OrbitCamera::computeEyePos() const
{
    float yawRad   = glm::radians(m_yaw);
    float pitchRad = glm::radians(m_pitch);

    glm::vec3 offset{
        sinf(yawRad) * cosf(pitchRad),
        sinf(pitchRad),
        cosf(yawRad) * cosf(pitchRad)
    };

    return m_center + offset * m_distance;
}

glm::vec3 OrbitCamera::position() const
{
    return computeEyePos();
}

glm::vec3 OrbitCamera::lookDirection() const
{
    return glm::normalize(m_center - computeEyePos());
}

void OrbitCamera::setPosition(glm::vec3 p)
{
    m_center = p;
}

glm::mat4 OrbitCamera::viewMatrix() const
{
    glm::vec3 eye = computeEyePos();
    return glm::lookAt(eye, m_center, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::projMatrix(float aspect) const
{
    return glm::perspective(glm::radians(m_fov), aspect, 0.5f, 10000.0f);
}

} // namespace sv
