#include "game_traffic_text.hpp"

namespace GameTrafficText {
    using GameTraffic::State;
    using PacketCapture::Problem;
    using Packets::Reason;

    static constexpr const char* RELOG = " Log out to the door and back in once.";

    std::string TimeLeft(int seconds) {
        if (seconds < 60)
            return std::to_string(seconds) + " s";
        return std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " s";
    }

    static Line cantListen(const GameTraffic::Status& status) {
        if (status.keysMissing)
            return { "The packets folder next to the overlay is missing. Install the overlay again." };
        if (status.keysDamaged)
            return { "The overlay's key files are damaged. Install the overlay again." };
        switch (status.problem) {
        case Problem::NpcapMissing:
            return { "Needs Npcap, a free program that lets the overlay read the game's traffic without running as admin.",
                false, true, true };
        case Problem::NpcapTooOld:
            return { "Npcap is too old. Install the new one.", false, true };
        case Problem::NpcapAdminOnly:
            return { "Npcap is set to admins only. Run the overlay as admin, or install Npcap again without "
                     "\"Restrict to administrators\" (then any program on this PC can read network traffic).", false, true };
        default:
            return { "Couldn't listen to the network. Check that you're online, then restart the overlay." };
        }
    }

    static Line listening(const GameTraffic::Status& status) {
        switch (status.read) {
        case Packets::State::WaitingForLogin:
            return { "Listening. If you're logged in already, log out to the door and back in once." };
        case Packets::State::Searching:
            if (status.secondsLeft < 0)
                return { "Finding the game's key..." };
            return { "Finding the game's key, about " + TimeLeft(status.secondsLeft) + " left" };
        case Packets::State::Reading:
            if (status.way == PacketCapture::Way::RawSockets)
                return { "Working (as admin, without Npcap)" };
            return { "Working (through Npcap)" };
        case Packets::State::NeedsRelog:
            switch (status.reason) {
            case Reason::StartedLate: return { std::string("The game was already logged in.") + RELOG };
            case Reason::CompressedLogin: return { std::string("Couldn't read this login.") + RELOG };
            case Reason::KeyStoppedFitting: return { std::string("The game's key changed.") + RELOG };
            default: return { std::string("Lost some of the game's traffic.") + RELOG };
            }
        case Packets::State::NoKey:
            if (status.reason == Reason::UnknownKeyId)
                return { "This game version is new. Its key can take a few hours to come. Log in again later." };
            return { "Couldn't find the game's key. Restart the game, then log in." };
        }
        return {};
    }

    Line For(const GameTraffic::Status& status) {
        switch (status.state) {
        case State::Off: return { "Off", true };
        case State::CantListen: return cantListen(status);
        case State::Listening: return listening(status);
        }
        return {};
    }

    bool NeedsRelog(const GameTraffic::Status& status) {
        return status.state == State::Listening && status.read == Packets::State::NeedsRelog;
    }
}
