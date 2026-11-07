#pragma once
#include <cstdint>
#include <functional>
#include "packet_capture.hpp"
#include "messages.hpp"
#include "reader.hpp"

// Listens to the game's traffic on one worker thread while it's turned on:
// PacketCapture catches, Packets::Reader reads. The overlay only reads the
// status, the sentences for it are the menu's. Nothing is ever sent to the
// game, and nothing of the traffic is written anywhere.

namespace GameTraffic {
    // Listening hands over to the reader's own state and reason
    enum class State { Off, CantListen, Listening };

    struct Status {
        State state = State::Off;
        // why it can't listen: the capture's problem, or the keys files are
        // missing or don't load
        PacketCapture::Problem problem = PacketCapture::Problem::None;
        bool keysMissing = false;
        bool keysDamaged = false;
        // while listening
        Packets::State read = Packets::State::WaitingForLogin;
        Packets::Reason reason = Packets::Reason::None;
        PacketCapture::Way way = PacketCapture::Way::None;
        // while searching: 1 from the game's start, 2 every sample
        int searchPass = 0;
        // while searching, follows the PC's load. -1 when it can't say yet
        int secondsLeft = -1;
    };

    // the codes extensions get: off, cant_listen, or the reader's state, and
    // the capture's problem, keys_missing or the reader's reason
    extern const char* StateCode(const Status& status);
    extern const char* ReasonCode(const Status& status);

    // the status a reader gives, for the worker and for replays
    extern Status FromReader(const Packets::Reader& reader);

    // when the game was started, unix ms, 0 when it isn't running
    extern uint64_t GameStart();

    // starts the worker, the caller doesn't wait for it. Stop waits for it to
    // end, Stop(false) only tells it to (the menu, so it doesn't freeze)
    extern void Start();
    extern void Stop(bool wait = true);
    extern Status GetStatus();

    // Called on the worker thread for every message read, keep it short.
    // Gives an id for RemoveListener.
    using Listener = std::function<void(const Messages::Message&)>;
    extern int AddListener(Listener listener);
    extern void RemoveListener(int id);
}
