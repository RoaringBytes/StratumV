// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "Camera.h"
#include "FreeFlyCamera.h"

namespace sv {

void Camera::init(GLFWwindow* window)
{
    if (!m_mode) {
        auto fly = std::make_unique<FreeFlyCamera>();
        fly->init(window);
        m_mode = std::move(fly);
    }
}

void Camera::update(GLFWwindow* window, float dt)
{
    if (m_mode)
        m_mode->update(window, dt);
}

glm::mat4 Camera::viewMatrix() const
{
    return m_mode ? m_mode->viewMatrix() : glm::mat4(1.0f);
}

glm::mat4 Camera::projMatrix(float aspect) const
{
    return m_mode ? m_mode->projMatrix(aspect) : glm::perspective(glm::radians(70.0f), aspect, 0.5f, 10000.0f);
}

glm::vec3 Camera::position() const
{
    return m_mode ? m_mode->position() : glm::vec3(0.0f);
}

glm::vec3 Camera::lookDirection() const
{
    return m_mode ? m_mode->lookDirection() : glm::vec3(0.0f, 0.0f, -1.0f);
}

void Camera::setPosition(glm::vec3 p)
{
    if (m_mode)
        m_mode->setPosition(p);
}

std::unique_ptr<ICameraMode> Camera::setMode(std::unique_ptr<ICameraMode> mode)
{
    auto prev = std::move(m_mode);
    m_mode = std::move(mode);
    return prev;
}

} // namespace sv
