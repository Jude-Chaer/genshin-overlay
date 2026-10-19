#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

// Chests the game says were opened, and their HoYoLAB points. The game sends
// every chest near the player with where it is, so an opened one is matched to
// the chest marker at that spot on HoYoLAB's Teyvat map, only when there's no
// other marker or chest close enough to mix it up with.

namespace GameChests {
    // the game's messages for objects showing up near the player, and a chest opened
    static constexpr uint16_t APPEARED = 27685;
    static constexpr uint16_t OPENED = 28616;

    struct Chest {
        // the game's number for it while it's near, 0 when not known
        uint32_t entity = 0;
        uint32_t group = 0;
        uint32_t config = 0;
        uint32_t gadget = 0;
        // the game's meters, y is up
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct Marker {
        int64_t id = 0;
        int label = 0;
        // HoYoLAB's x_pos / y_pos
        double x = 0.0, y = 0.0;
        // HoYoLAB's underground floor, 0 above ground
        int floor = 0;
    };

    // the objects of the world (with a group and config) in an APPEARED message,
    // chests or not
    extern void ReadObjects(std::string_view body, std::vector<Chest>& out);
    // the chests in an APPEARED message
    extern void ReadAppeared(std::string_view body, std::vector<Chest>& out);
    // group and config of the opened chest, false when the message has none
    extern bool ReadOpened(std::string_view body, Chest& out);

    // HoYoLAB's chest markers on Teyvat, from the item files' positions folder
    extern bool LoadMarkers(const std::filesystem::path& positions, std::vector<Marker>& out);
    // Teyvat's groups start with 131 to 133, the Chasm underground's with 166
    // and a domain's with 220, so another world's chest isn't put on Teyvat
    extern bool OnTeyvat(const Chest& chest);
    // where a chest is on HoYoLAB's Teyvat map
    extern void ToMap(const Chest& chest, double& x, double& y);
    // the marker that's surely this chest, 0 when it isn't sure. nearby are the
    // other chests the game sent, they can sit at the marker too
    extern int64_t Match(const Chest& chest, const std::vector<Chest>& nearby, const std::vector<Marker>& markers);

    // Keeps the chests the game sent, so an opened one is known by where it was.
    class Tracker {
    public:
        // true when the message says a chest was opened. placed is false when
        // the game never said where that chest is
        bool Feed(uint16_t id, std::string_view body, Chest& opened, bool& placed);
        std::vector<Chest> Chests() const;
        // the last objects the game sent were on Teyvat
        bool OnTeyvat() const { return m_teyvat; }

    private:
        std::unordered_map<uint64_t, Chest> m_chests;
        bool m_teyvat = false;
    };

    // the game's message with where the player moved, one the game gets from us
    static constexpr uint16_t MOVED = 2246;
    // where the player is, false when the message doesn't say
    extern bool ReadPlayer(std::string_view body, Chest& where);

    // What the game shows about a chest marker. Opened chests aren't sent at
    // all, so a chest marker the player came close to with no chest sent near
    // it is probably opened. Only probably: a chest behind a puzzle or a quest
    // isn't there either until that's done, so it's never marked from this.
    enum class Hint { None, StillThere, ProbablyOpened };

    // StillThere when a chest the game sent is surely this marker's,
    // ProbablyOpened when the game sent no chest near it at all
    extern Hint HintFor(const Marker& marker, const std::vector<Chest>& chests, const std::vector<Marker>& markers);

    // The hints for the chest markers around the player, kept as the player moves.
    class Hints {
    public:
        // true when it's time to look again, about once a second
        bool Due(uint64_t ms) const { return ms >= m_last + 1000; }
        // ms is when the player was there, chests the ones the game sent and not opened
        void Moved(const Chest& player, uint64_t ms, const std::vector<Chest>& chests, const std::vector<Marker>& markers);
        const std::unordered_map<int64_t, Hint>& All() const { return m_hints; }

    private:
        // when the player first came close to a marker
        std::unordered_map<int64_t, uint64_t> m_near;
        std::unordered_map<int64_t, Hint> m_hints;
        uint64_t m_last = 0;
    };
}
