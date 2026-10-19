#pragma once
#include "game_chests.hpp"
#include <cstdint>
#include <filesystem>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

// Puzzles the player solves: torch puzzles, elemental monuments, Tri-Yana Seeds,
// time trials and Seelies. The game sends every part of a puzzle with its state
// and each change of it, and all parts of one puzzle share a group, so the group
// says when the puzzle is done. Its HoYoLAB marker sits at one of the parts.
// Only puzzles done while the overlay listens count, the ones that come in
// already done are left alone.

namespace GamePuzzles {
    // the game's message for an object changing state
    static constexpr uint16_t CHANGED = 22292;

    // a puzzle that was just done, or one part of it that has its own marker
    struct Done {
        int label = 0;
        uint32_t group = 0;
        // the parts its marker can be at
        std::vector<GameChests::Chest> parts;
        // only a guess from a chest opened, no marker there is normal
        bool loose = false;
        // it was done before it came in, only good for a hint
        bool already = false;
    };

    // the entity and its new state in a CHANGED message, false when it has none
    extern bool ReadChanged(std::string_view body, uint32_t& entity, uint32_t& state);
    // HoYoLAB's markers of the puzzle kinds above on Teyvat
    extern bool LoadMarkers(const std::filesystem::path& positions, std::vector<GameChests::Marker>& out);
    // the marker that's surely this puzzle, 0 when it isn't sure
    extern int64_t Match(const Done& done, const std::vector<GameChests::Marker>& markers);

    class Tracker {
    public:
        // ms is when the message came. Messages of the same moment belong together
        // (a failed monument puzzle resets in the same moment its last monument
        // lights), so what they finish comes out with the first message of a later one
        void Feed(uint16_t id, std::string_view body, uint64_t ms, std::vector<Done>& done);

    private:
        void finishMoment(std::vector<Done>& done);
        void check(uint32_t group, bool opened, bool changed, std::vector<Done>& done);

        // by group and config
        std::unordered_map<uint64_t, GameChests::Chest> m_objects;
        std::unordered_map<uint32_t, uint64_t> m_entities;
        std::unordered_map<uint32_t, std::vector<uint64_t>> m_groups;
        // label, group and config of what's done already, so nothing comes out twice
        std::set<std::tuple<int, uint32_t, uint32_t>> m_done;
        // the same for what came in done already, said once for its hint
        std::set<std::tuple<int, uint32_t, uint32_t>> m_seenDone;
        // the moment being read and the groups it touched
        uint64_t m_ms = 0;
        std::set<uint32_t> m_arrived, m_changed, m_opened;
    };
}
