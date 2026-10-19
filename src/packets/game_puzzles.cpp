#include "game_puzzles.hpp"
#include "game_packets.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

namespace GamePuzzles {
    static constexpr int TEYVAT = 2;
    // HoYoLAB's labels
    static constexpr int SEELIE = 18, MONUMENT = 70, TORCH = 71, SEEDS = 407, TRIAL = 64;
    // the parts the game sends for each kind, from recorded traffic
    // every torch and fire basin the game has for puzzles in the world, from its gadget
    // names (FireBasin, ImmortalFire, TorchPuzzle and the regions' own), leaving out
    // enemies' torches, event domains, hide and seek, quests and Miliastra builds
    static constexpr uint32_t TORCHES[] = {
        70220031, 70310001, 70310002, 70310003, 70310011, 70310012, 70310015, 70310034, 70310147, 70310238,
        70310475, 70310479, 70320005, 70320020, 70330278, 70330335, 70330336, 70330394, 70330395, 70330411,
        70330704, 70330729, 70331421, 70331422, 70331536, 70331537, 70360338, 70900393, 73003038, 73005126,
        73054002, 73076016, 73122070,
    };
    // the elemental monuments (Tablet in the game's names), not the ones that follow the player
    static constexpr uint32_t MONUMENTS[] = {
        70310300, 70310301, 70330332, 70330400, 70330401, 70330402, 70330403, 70330404, 70330405, 70330441,
        70900007, 70900008, 70900009, 70900039, 70900049, 70900050, 70900051, 70900052, 70900053, 70900054,
        70900055, 70900056, 70900057, 70900058, 70900102,
    };
    // the time trial starts (Challengestarter), on land and under water, not the events' ones
    static constexpr uint32_t TRIAL_STARTS[] = {
        70330334, 70330343, 70330344, 70330438, 70330439, 70330440, 70330658, 70330659, 70330660, 70330735,
        70330736, 70330737, 70330763, 70330764, 70330765, 70350082, 70350083, 70350084, 70350092, 70350093,
        70350094, 70350357, 73003068,
    };
    static constexpr uint32_t SEED_GADGETS[] = { 70290526, 70320004 };
    // the Seelies HoYoLAB has under Seelie, Electro and Warming Seelies have their own markers
    static constexpr uint32_t SEELIES[] = { 70710010, 70330741, 74006087 };
    // where a Seelie flies home to (Lamp_Post)
    static constexpr uint32_t COURTS[] = { 70290293, 70330331, 70710006, 70900274 };
    // states: a locked chest, a lit torch or monument (201..299), a won trial,
    // a Seelie home at its court
    static constexpr uint32_t LOCKED = 101, LIT = 201, WON = 202, ARRIVED = 203;
    // a puzzle's marker is close to one of its parts, and the next one clearly further
    static constexpr double MATCH = 12.0;
    static constexpr double MARGIN = 5.0;
    // a long trip leaves a lot behind
    static constexpr size_t MAX_OBJECTS = 50000;

    template <size_t N>
    static bool isAny(uint32_t gadget, const uint32_t (&list)[N]) {
        return std::find(std::begin(list), std::end(list), gadget) != std::end(list);
    }

    bool ReadChanged(std::string_view body, uint32_t& entity, uint32_t& state) {
        entity = state = 0;
        GamePackets::Field field;
        while (GamePackets::NextField(body, field)) {
            if (field.wire != 0) continue;
            // a state of 0 isn't sent at all
            if (field.number == 3) entity = uint32_t(field.value);
            else if (field.number == 7) state = uint32_t(field.value);
        }
        return entity != 0;
    }

    bool LoadMarkers(const std::filesystem::path& positions, std::vector<GameChests::Marker>& out) {
        out.clear();
        for (int label : { SEELIE, MONUMENT, TORCH, SEEDS, TRIAL }) {
            std::ifstream in(positions / (std::to_string(label) + ".json"), std::ios::binary);
            if (!in) continue;
            nlohmann::json points = nlohmann::json::parse(in, nullptr, false);
            if (!points.is_array()) continue;
            for (const nlohmann::json& point : points) {
                if (!point.is_object() || point.value("map_id", 0) != TEYVAT) continue;
                GameChests::Marker marker;
                marker.id = point.value("id", int64_t(0));
                marker.label = label;
                marker.x = point.value("x_pos", 0.0);
                marker.y = point.value("y_pos", 0.0);
                const nlohmann::json& group = point.contains("point_group") ? point["point_group"] : nlohmann::json();
                if (group.is_object()) marker.floor = group.value("floor_id", 0);
                if (marker.id) out.push_back(marker);
            }
        }
        return !out.empty();
    }

    int64_t Match(const Done& done, const std::vector<GameChests::Marker>& markers) {
        for (const GameChests::Chest& part : done.parts) {
            double x, y;
            GameChests::ToMap(part, x, y);
            const GameChests::Marker* best = nullptr;
            double bestDistance = MATCH;
            for (const GameChests::Marker& marker : markers) {
                double d = std::hypot(marker.x - x, marker.y - y);
                if (marker.label == done.label && d < bestDistance) {
                    best = &marker;
                    bestDistance = d;
                }
            }
            if (!best) continue;
            double reach = std::max(2.0 * bestDistance, bestDistance + MARGIN);
            bool alone = true;
            for (const GameChests::Marker& marker : markers) {
                if (&marker != best && marker.label == done.label && std::hypot(marker.x - x, marker.y - y) < reach)
                    alone = false;
            }
            if (alone) return best->id;
        }
        return 0;
    }

    static uint64_t keyOf(uint32_t group, uint32_t config) { return uint64_t(group) << 32 | config; }

    void Tracker::Feed(uint16_t id, std::string_view body, uint64_t ms, std::vector<Done>& done) {
        if (ms != m_ms) {
            finishMoment(done);
            m_ms = ms;
        }
        if (id == GameChests::APPEARED) {
            std::vector<GameChests::Chest> objects;
            GameChests::ReadObjects(body, objects);
            if (m_objects.size() > MAX_OBJECTS) {
                m_objects.clear();
                m_entities.clear();
                m_groups.clear();
            }
            for (const GameChests::Chest& object : objects) {
                if (!GameChests::OnTeyvat(object)) continue;
                uint64_t key = keyOf(object.group, object.config);
                if (!m_objects.count(key)) m_groups[object.group].push_back(key);
                m_objects[key] = object;
                m_entities[object.entity] = key;
                m_arrived.insert(object.group);
            }
        }
        else if (id == CHANGED) {
            uint32_t entity, state;
            if (!ReadChanged(body, entity, state)) return;
            auto it = m_entities.find(entity);
            if (it == m_entities.end() || !m_objects.count(it->second)) return;
            GameChests::Chest& object = m_objects[it->second];
            object.state = state;
            m_changed.insert(object.group);
        }
        else if (id == GameChests::OPENED) {
            GameChests::Chest opened;
            if (GameChests::ReadOpened(body, opened) && m_objects.count(keyOf(opened.group, opened.config)))
                m_opened.insert(opened.group);
        }
    }

    void Tracker::finishMoment(std::vector<Done>& done) {
        std::set<uint32_t> groups = m_arrived;
        groups.insert(m_changed.begin(), m_changed.end());
        groups.insert(m_opened.begin(), m_opened.end());
        for (uint32_t group : groups)
            check(group, m_opened.count(group) > 0, m_changed.count(group) || m_opened.count(group), done);
        m_arrived.clear();
        m_changed.clear();
        m_opened.clear();
    }

    // A group that only came in is looked at too, so what's done already is
    // known, comes out as already done and isn't given out again later
    void Tracker::check(uint32_t group, bool opened, bool changed, std::vector<Done>& done) {
        std::vector<GameChests::Chest> parts, torches, monuments, starts, seelies;
        bool locked = false, court = false, seelie = false;
        for (uint64_t key : m_groups[group]) {
            const GameChests::Chest& object = m_objects[key];
            parts.push_back(object);
            if (object.state == LOCKED) locked = true;
            if (isAny(object.gadget, TORCHES)) torches.push_back(object);
            if (isAny(object.gadget, MONUMENTS)) monuments.push_back(object);
            if (isAny(object.gadget, TRIAL_STARTS)) starts.push_back(object);
            if (isAny(object.gadget, SEELIES)) seelies.push_back(object);
            if (isAny(object.gadget, COURTS)) seelie = true;
            if (isAny(object.gadget, COURTS) && object.state == ARRIVED) court = true;
        }
        auto add = [&](int label, uint32_t config, const std::vector<GameChests::Chest>& at, bool loose = false) {
            // one done while listening can still come in done after a relog, for its hint
            // but one that came in done is never done just now
            std::tuple<int, uint32_t, uint32_t> key{ label, group, config };
            bool fresh = changed ? !m_seenDone.count(key) && m_done.insert(key).second : m_seenDone.insert(key).second;
            if (!fresh) return;
            done.push_back({ label, group, at, loose, !changed });
        };
        auto lit = [](const GameChests::Chest& o) { return o.state >= LIT && o.state < 300; };

        if (!torches.empty() && !locked && std::all_of(torches.begin(), torches.end(), lit))
            add(TORCH, 0, torches);
        // each monument can have its own marker, and the whole puzzle one more
        if (!monuments.empty() && std::all_of(monuments.begin(), monuments.end(),
            [](const GameChests::Chest& o) { return o.state == LIT || o.state == WON; })) {
            for (const GameChests::Chest& monument : monuments) add(MONUMENT, monument.config, { monument });
        }
        // the seeds can be done in any order, each has its own marker
        for (const GameChests::Chest& part : parts)
            if (isAny(part.gadget, SEED_GADGETS) && part.state == LIT) add(SEEDS, part.config, { part });
        // a chest can show up during a trial and go again when the time runs out,
        // only a won start or the chest opened says it's done
        bool won = std::any_of(starts.begin(), starts.end(), [](const GameChests::Chest& o) { return o.state == WON; });
        if (won || (opened && !starts.empty()))
            add(TRIAL, 0, starts);
        // some trials have no start the game sends, just the things to collect
        else if (opened && torches.empty() && monuments.empty() && !seelie && seelies.empty())
            add(TRIAL, 0, parts, true);
        // a Seelie done long ago is gone, only its court is left, and the marker
        // sits where the Seelie started, so only the ones seen fly home count
        if (court && !seelies.empty())
            add(SEELIE, 0, seelies);
    }
}
