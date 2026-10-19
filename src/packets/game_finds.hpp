#pragma once
#include "game_chests.hpp"
#include "game_packets.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Everything the player finds in the world, from the game's messages: chests
// opened, oculi picked up and puzzles solved, each with its HoYoLAB point, and
// the hints for the markers around the player. The overlay and the packet
// recorder both read the game through a Finder, so they always do the same.

namespace GameFinds {
    // something the player just found
    struct Found {
        // like "oculus 133502497 497001 picked up at -9066 4918", for the logs
        std::string what;
        // HoYoLAB's point, 0 when it isn't sure which
        int64_t point = 0;
        // the objects it was, they leave the hints
        std::vector<GameChests::Chest> objects;
        // what to tell the player when there's no point, nothing when no marker was
        // expected (a chest the game never said where it is, for one)
        const char* notMatched = nullptr;
    };

    // One kind of thing the player can find. The Finder hands it every message the
    // game sends, so no kind misses one because another kind used it first, and
    // what it finds leaves the hints the same way for every kind.
    class Kind {
    public:
        virtual ~Kind() = default;
        // ms is when the message came. Adds what it finishes to found
        virtual void Feed(uint16_t id, std::string_view body, uint64_t ms, std::vector<Found>& found) = 0;
    };

    class Finder {
    public:
        // positions is the item files' positions folder. The files can still be
        // downloading when the overlay starts, so they're read when first needed
        explicit Finder(std::filesystem::path positions);
        // reads one message and adds what the player found. True when the hints
        // were looked at again
        bool Feed(const GamePackets::Command& command, uint64_t ms, std::vector<Found>& found);
        const std::unordered_map<int64_t, GameChests::Hint>& Hints() const { return m_hints.All(); }

    private:
        std::filesystem::path m_positions;
        // the chests, spincrystals and oculi the game still has, for the hints
        GameChests::Tracker m_world;
        std::vector<GameChests::Marker> m_markers;
        GameChests::Hints m_hints;
        std::vector<std::unique_ptr<Kind>> m_kinds;
    };
}
