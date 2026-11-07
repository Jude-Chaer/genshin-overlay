#pragma once
#include <string>
#include "packets/game_traffic.hpp"

// What the menu says about the game traffic, made from GameTraffic's codes.
// No ImGui here, so the tests can check every sentence.

namespace GameTrafficText {
    struct Line {
        std::string text;
        // greyed out, for off
        bool quiet = false;
        // a Download Npcap button under the line
        bool downloadNpcap = false;
        // and "Or run the overlay as admin." under the button
        bool orAdmin = false;
    };

    extern Line For(const GameTraffic::Status& status);

    // "2 min 10 s", "40 s"
    extern std::string TimeLeft(int seconds);

    // the player has to do something in game, the note on screen says the line
    extern bool NeedsRelog(const GameTraffic::Status& status);
}
