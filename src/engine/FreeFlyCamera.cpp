// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "FreeFlyCamera.h"
#include <GLFW/glfw3.h>
#include <algorithm>

namespace sv {

void FreeFlyCamera::init(GLFWwindow* window)
{
    glfwGetCursorPos(window, &m_lastX, &m_lastY);
    m_firstMouse = true;
}

void FreeFlyCamera::update(GLFWwindow* window, float dt)
{
    // ── Mouse look (right-click hold) ────────────────────────────
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
        m_pitch -= dy * m_sensitivity;  // inverted
        m_pitch  = std::clamp(m_pitch, -89.0f, 89.0f);
    }

    // ── WASD + Space/Ctrl movement ───────────────────────────────
    glm::vec3 move{0.0f};
    glm::vec3 fwd = forward();
    glm::vec3 rgt = right();
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)     move += fwd;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)     move -= fwd;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)     move += rgt;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)     move -= rgt;
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) move += up;
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) move -= up;

    if (glm::length(move) > 0.0f)
        move = glm::normalize(move);

    float speed = m_speed;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        speed *= 3.0f;

    m_pos += move * speed * dt;
}

glm::vec3 FreeFlyCamera::lookDirection() const
{
    float yawRad   = glm::radians(m_yaw);
    float pitchRad = glm::radians(m_pitch);
    return glm::normalize(glm::vec3(
        sinf(yawRad) * cosf(pitchRad),
        sinf(pitchRad),
        -cosf(yawRad) * cosf(pitchRad)
    ));
}

glm::vec3 FreeFlyCamera::forward() const
{
    float yawRad = glm::radians(m_yaw);
    return glm::normalize(glm::vec3(sinf(yawRad), 0.0f, -cosf(yawRad)));
}

glm::vec3 FreeFlyCamera::right() const
{
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 FreeFlyCamera::viewMatrix() const
{
    glm::vec3 look = lookDirection();
    return glm::lookAt(m_pos, m_pos + look, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 FreeFlyCamera::projMatrix(float aspect) const
{
    return glm::perspective(glm::radians(m_fov), aspect, 0.5f, 10000.0f);
}

} // namespace sv
