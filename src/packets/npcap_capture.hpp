#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Catches the game's network frames with Npcap, as a normal user. Npcap isn't
// bundled (its license doesn't allow it), the player installs it. wpcap.dll is
// loaded when Start is called, so the overlay runs fine without it.

namespace NpcapCapture {
    struct Frame {
        // unix ms when Npcap caught it
        uint64_t ms = 0;
        // which adapter, index into the names Start gave
        uint8_t adapter = 0;
        std::vector<uint8_t> data;
    };

    // Opens every ethernet adapter, only the game's ports get through. False
    // and why if Npcap is missing or no adapter opens. Adapters gets their names.
    extern bool Start(std::vector<std::string>& adapters, std::string& problem);
    extern void Stop();
    extern bool Running();

    // Waits up to milliseconds for frames, then hands over all caught since the
    // last call, oldest first.
    extern void Take(std::vector<Frame>& out, int milliseconds);
    // frames thrown away because nobody took them in time
    extern uint64_t Dropped();
}
