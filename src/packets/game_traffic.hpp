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

    // when the game was started, unix ms, 0 when it isn't running or can't be read
    extern uint64_t GameStart();

    // starts listening on its own thread, the overlay doesn't wait for it
    extern void Start();
    extern void Stop();

    extern Status GetStatus();

    // the HoYoLAB points of the chests opened, oculi picked up and puzzles solved since the overlay started, from
    // the from-th on, gives back how many there are in all
    extern size_t OpenedPoints(size_t from, std::vector<int64_t>& out);
    // the HoYoLAB chest points the player came near since the overlay started: the ones whose
    // chest the game still has, and the ones it sent no chest for (probably opened)
    extern void ChestHints(std::vector<int64_t>& stillThere, std::vector<int64_t>& probablyOpened);

    // the status, inside the menu
    extern void DrawStatus();
    // a short note on screen when the player has to do something, like relog
    extern void DrawNotice();
    // a note is up, the overlay has to draw even with the map closed
    extern bool NeedsDrawing();
}
