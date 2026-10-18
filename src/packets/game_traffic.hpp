#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Listens to the game's traffic in the background, only when the player turned
// it on in the menu, so the login is seen and the messages can be read. Npcap
// does the catching (NpcapCapture), the Sniffer the reading. Nothing is ever
// sent to the game.

namespace GameTraffic {
    enum class State {
        // not started, or stopped
        Off,
        // Npcap missing, set to administrators only, or the keys aren't there
        CantListen,
        // listening, nothing from the game yet
        Waiting,
        // the login was missed, the key is searched for
        Searching,
        // the game's messages read fine
        Reading,
        // the game talks but it can't be read, the text says what to do
        CantRead,
    };

    struct Status {
        State state = State::Off;
        // what the menu shows, empty while it all works
        std::string text;
    };

    // starts listening on its own thread, the overlay doesn't wait for it
    extern void Start();
    extern void Stop();

    extern Status GetStatus();

    // the HoYoLAB points of the chests opened since the overlay started, from
    // the from-th on, gives back how many there are in all
    extern size_t OpenedPoints(size_t from, std::vector<int64_t>& out);

    // the status, inside the menu
    extern void DrawStatus();
    // a short note on screen when the player has to do something, like relog
    extern void DrawNotice();
    // a note is up, the overlay has to draw even with the map closed
    extern bool NeedsDrawing();
}
