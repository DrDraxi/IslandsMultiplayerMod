#pragma once

#include <cstdint>
#include <string>

namespace IslandsMP
{
    // In-game chat, using the game's own party chat box (WBP_ChatBox), which shipped wired to
    // the official servers. Enter opens the input; messages go to everyone in the Steam
    // session. Join/leave lines are shown in the same feed. Game thread only.

    void ChatOpenRequested();                                         // Enter pressed
    void ChatCancel();                                                // Esc pressed
    void TickChat(float dt);
    void ChatShowMessage(const std::string& name, const std::string& text); // UTF-8
    void ChatSystemLine(const std::string& text);                     // UTF-8, no sender
    void ChatReset();                                                 // map load: widget is gone
}
