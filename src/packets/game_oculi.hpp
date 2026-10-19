#pragma once
#include "game_chests.hpp"
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

// Oculi the player picked up. The game says "you got a Hydroculus, picked up in
// the world" and right after that the object leaves, so the object that left is
// the oculus, and where it was gives the oculus marker on HoYoLAB's Teyvat map.
// Crimson Agates, Spirit Carps and Khvarena fragments are picked up the same way.

namespace GameOculi {
    // the game's messages for getting an item and for an object leaving
    static constexpr uint16_t GOT_ITEM = 652;
    static constexpr uint16_t LEFT = 24935;

    // HoYoLAB's oculus label for the game's item, 0 when it's no oculus
    extern int LabelOf(uint32_t item);

    // the item in a GOT_ITEM message and where it came from (11 = picked up in
    // the world), false when the message has none
    extern bool ReadGotItem(std::string_view body, uint32_t& item, uint32_t& from);
    // the entity that left, and why (2 = taken or broken, the rest is going out of reach)
    extern bool ReadLeft(std::string_view body, uint32_t& entity, uint32_t& reason);

    // HoYoLAB's oculus markers on Teyvat
    extern bool LoadMarkers(const std::filesystem::path& positions, std::vector<GameChests::Marker>& out);
    // the marker that's surely this oculus, 0 when it isn't sure. nearby are the
    // other objects the game sent, an oculus of the same kind can sit at the marker too
    extern int64_t Match(const GameChests::Chest& oculus, int label, const std::vector<GameChests::Chest>& nearby,
        const std::vector<GameChests::Marker>& markers);

    struct Picked {
        GameChests::Chest object;
        int label = 0;
    };

    // Keeps the objects the game sent, and puts the oculus item and the object
    // that left together.
    class Tracker {
    public:
        // true when the message finishes an oculus pick up. ms is when it came
        bool Feed(uint16_t id, std::string_view body, uint64_t ms, Picked& picked);
        std::vector<GameChests::Chest> Objects() const;

    private:
        std::unordered_map<uint32_t, GameChests::Chest> m_objects;
        // the last oculus item got and when, 0 when none is waiting
        int m_label = 0;
        uint64_t m_ms = 0;
    };
}
