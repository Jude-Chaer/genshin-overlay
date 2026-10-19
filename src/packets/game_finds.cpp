#include "game_finds.hpp"
#include "game_oculi.hpp"
#include "game_puzzles.hpp"
#include <sstream>

namespace GameFinds {
    static constexpr const char* CHEST_NOT_MATCHED = "opened a chest or picked up a spincrystal it can't match to a marker, mark it yourself";
    static constexpr const char* OCULUS_NOT_MATCHED = "picked up an oculus, agate or carp it can't match to a marker, mark it yourself";

    static std::string where(const GameChests::Chest& object) {
        double x, y;
        GameChests::ToMap(object, x, y);
        std::ostringstream text;
        text << " at " << x << " " << y;
        return text.str();
    }

    // Chests opened and spincrystals picked up. They're kept in the world the
    // hints read, so this one reads them from there.
    class Chests : public Kind {
    public:
        Chests(GameChests::Tracker& world, std::vector<GameChests::Marker>& markers, const std::filesystem::path& positions)
            : m_world(world), m_markers(markers), m_positions(positions) {}

        void Feed(uint16_t id, std::string_view body, uint64_t, std::vector<Found>& found) override {
            GameChests::Chest chest;
            bool placed = false;
            if (!m_world.Feed(id, body, chest, placed))
                return;
            Found f;
            f.what = "chest " + std::to_string(chest.group) + " " + std::to_string(chest.config) + " opened";
            // inspecting things and places of interest come as opened too,
            // only the chests the game showed us have a gadget to tell
            if (placed) {
                if (m_markers.empty()) GameChests::LoadMarkers(m_positions, m_markers);
                f.what += where(chest);
                f.point = GameChests::Match(chest, m_world.Chests(), m_markers);
                f.objects = { chest };
                f.notMatched = CHEST_NOT_MATCHED;
            }
            found.push_back(f);
        }

    private:
        GameChests::Tracker& m_world;
        std::vector<GameChests::Marker>& m_markers;
        std::filesystem::path m_positions;
    };

    class Oculi : public Kind {
    public:
        explicit Oculi(const std::filesystem::path& positions) : m_positions(positions) {}

        void Feed(uint16_t id, std::string_view body, uint64_t ms, std::vector<Found>& found) override {
            GameOculi::Picked picked;
            if (!m_oculi.Feed(id, body, ms, picked))
                return;
            if (m_markers.empty()) GameOculi::LoadMarkers(m_positions, m_markers);
            Found f;
            f.what = "oculus " + std::to_string(picked.object.group) + " " + std::to_string(picked.object.config)
                + " picked up" + where(picked.object);
            f.point = GameOculi::Match(picked.object, picked.label, m_oculi.Objects(), m_markers);
            f.objects = { picked.object };
            f.notMatched = OCULUS_NOT_MATCHED;
            found.push_back(f);
        }

    private:
        GameOculi::Tracker m_oculi;
        std::vector<GameChests::Marker> m_markers;
        std::filesystem::path m_positions;
    };

    class Puzzles : public Kind {
    public:
        explicit Puzzles(const std::filesystem::path& positions) : m_positions(positions) {}

        void Feed(uint16_t id, std::string_view body, uint64_t ms, std::vector<Found>& found) override {
            std::vector<GamePuzzles::Done> done;
            m_puzzles.Feed(id, body, ms, done);
            if (!done.empty() && m_markers.empty()) GamePuzzles::LoadMarkers(m_positions, m_markers);
            for (const GamePuzzles::Done& d : done) {
                int64_t point = GamePuzzles::Match(d, m_markers);
                // only a guess from a chest opened, no marker there is normal
                if (point == 0 && d.loose) continue;
                Found f;
                f.what = "puzzle " + std::to_string(d.label) + " in group " + std::to_string(d.group)
                    + (d.already ? " done already" : " done");
                f.point = point;
                f.objects = d.parts;
                f.already = d.already;
                found.push_back(f);
            }
        }

    private:
        GamePuzzles::Tracker m_puzzles;
        std::vector<GameChests::Marker> m_markers;
        std::filesystem::path m_positions;
    };

    Finder::Finder(std::filesystem::path positions) : m_positions(std::move(positions)) {
        m_kinds.push_back(std::make_unique<Puzzles>(m_positions));
        m_kinds.push_back(std::make_unique<Oculi>(m_positions));
        m_kinds.push_back(std::make_unique<Chests>(m_world, m_markers, m_positions));
    }

    bool Finder::Feed(const GamePackets::Command& command, uint64_t ms, std::vector<Found>& found) {
        if (command.direction == GamePackets::Direction::Sent) {
            GameChests::Chest player;
            if (command.id != GameChests::MOVED || !m_world.OnTeyvat() || !m_hints.Due(ms)
                || !GameChests::ReadPlayer(command.body, player))
                return false;
            if (m_markers.empty()) GameChests::LoadMarkers(m_positions, m_markers);
            m_hints.Moved(player, ms, m_world.Chests(), m_markers);
            return true;
        }
        size_t before = found.size();
        for (const std::unique_ptr<Kind>& kind : m_kinds)
            kind->Feed(command.id, command.body, ms, found);
        // what came in done already only gives a hint, it isn't found just now
        bool hinted = false;
        auto it = found.begin() + before;
        while (it != found.end()) {
            if (!it->already) {
                ++it;
                continue;
            }
            if (it->point && m_already.emplace(it->point, GameChests::Hint::ProbablyOpened).second) hinted = true;
            it = found.erase(it);
        }
        // whatever was found isn't there any more
        for (size_t i = before; i < found.size(); i++)
            for (const GameChests::Chest& object : found[i].objects) m_world.Forget(object);
        return hinted;
    }

    std::unordered_map<int64_t, GameChests::Hint> Finder::Hints() const {
        std::unordered_map<int64_t, GameChests::Hint> all = m_already;
        for (const auto& [point, hint] : m_hints.All()) all[point] = hint;
        return all;
    }
}
