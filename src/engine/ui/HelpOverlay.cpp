// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "HelpOverlay.h"

#include "UiStyle.h"

#include <imgui.h>

namespace sv {

HelpOverlay::HelpOverlay()
{
    m_topics.push_back({
        "Getting started",
        "Welcome to StratumV. This overlay lists the controls and "
        "panels available in the current application.\n\n"
        "Press F1 at any time to show or hide it. Pick a topic on "
        "the left to read about it."});

    m_topics.push_back({
        "Camera",
        "Hold the right mouse button and move the mouse to look "
        "around.\n\n"
        "W, A, S and D fly forward, left, back and right. Space "
        "moves up, left Ctrl moves down, and holding left Shift "
        "moves faster."});
}

void HelpOverlay::addTopic(std::string title, std::string body)
{
    m_topics.push_back({std::move(title), std::move(body)});
}

void HelpOverlay::draw()
{
    if (!m_visible) return;
    if (m_topics.empty()) return;

    if (m_selected < 0)                          m_selected = 0;
    if (m_selected >= (int)m_topics.size())      m_selected = (int)m_topics.size() - 1;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 380.0f), ImGuiCond_Appearing);

    if (ImGui::Begin("Help", &m_visible, ImGuiWindowFlags_NoCollapse)) {
        // Topic list
        ImGui::BeginChild("##help_topics", ImVec2(170.0f, -28.0f), ImGuiChildFlags_None);
        for (int i = 0; i < (int)m_topics.size(); i++) {
            if (ImGui::Selectable(m_topics[i].title.c_str(), m_selected == i))
                m_selected = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Topic body
        ImGui::BeginChild("##help_body", ImVec2(0.0f, -28.0f), ImGuiChildFlags_None);
        ImGui::TextColored(style::kAccent, "%s", m_topics[m_selected].title.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_topics[m_selected].body.c_str());
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::TextDisabled("Press F1 to close this window.");
    }
    ImGui::End();
}

} // namespace sv
