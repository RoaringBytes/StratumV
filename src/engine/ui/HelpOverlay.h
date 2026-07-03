// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <string>
#include <vector>

namespace sv {

// In-engine how-to overlay. Holds a list of help topics (engine
// defaults plus any game-added ones) and draws them in a dismissable
// ImGui window with a topic list on the left and the topic body on
// the right. Wire a key (typically F1) to toggle().
//
// The overlay is intentionally not drawn by the engine itself; the
// host application calls draw() once per ImGui frame so headless and
// golden-render paths stay untouched.
class HelpOverlay {
public:
    HelpOverlay();

    void setVisible(bool v) { m_visible = v; }
    void toggle()           { m_visible = !m_visible; }
    bool visible() const    { return m_visible; }

    // Append a game-specific topic after the engine defaults.
    void addTopic(std::string title, std::string body);

    // Draw the overlay if visible. Call once per ImGui frame.
    void draw();

private:
    struct Topic {
        std::string title;
        std::string body;
    };

    std::vector<Topic> m_topics;
    bool m_visible  = false;
    int  m_selected = 0;
};

} // namespace sv
