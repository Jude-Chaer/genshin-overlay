#include "game_oculi.hpp"
#include "game_packets.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

namespace GameOculi {
    static constexpr int TEYVAT = 2;
    static constexpr uint32_t PICKED_UP = 11;
    static constexpr uint32_t TAKEN = 2;
    // the item and the object leaving come together, in the same moment
    static constexpr uint64_t TOGETHER_MS = 200;
    // like the chests: the marker at most this far, the next one clearly further
    static constexpr double MATCH = 20.0;
    static constexpr double MARGIN = 5.0;

    // the game's oculus items and HoYoLAB's labels for them, and the other things
    // picked up the same way. Cryoculi aren't here, their item number isn't known yet
    static constexpr struct { uint32_t item; int label; } OCULI[] = {
        { 107001, 5 },   // Anemoculus
        { 107003, 6 },   // Geoculus
        { 107014, 194 }, // Electroculus
        { 107017, 403 }, // Dendroculus
        { 107023, 508 }, // Hydroculus
        { 107028, 626 }, // Pyroculus
        { 107030, 694 }, // Lunoculus
        { 107010, 141 }, // Crimson Agate
        { 107015, 472 }, // Khvarena Inscription Fragment
        { 107026, 554 }, // Spirit Carp
    };

    int LabelOf(uint32_t item) {
        for (const auto& oculus : OCULI)
            if (oculus.item == item) return oculus.label;
        return 0;
    }

    bool ReadGotItem(std::string_view body, uint32_t& item, uint32_t& from) {
        item = from = 0;
        GamePackets::Field field;
        while (GamePackets::NextField(body, field)) {
            if (field.number == 11 && field.wire == 0) {
                from = uint32_t(field.value);
            }
            else if (field.number == 13 && field.wire == 2) {
                std::string_view got = field.bytes;
                GamePackets::Field g;
                while (GamePackets::NextField(got, g))
                    if (g.number == 1 && g.wire == 0) item = uint32_t(g.value);
            }
        }
        return item != 0;
    }

    bool ReadLeft(std::string_view body, uint32_t& entity, uint32_t& reason) {
        entity = reason = 0;
        GamePackets::Field field;
        while (GamePackets::NextField(body, field)) {
            if (field.wire != 0) continue;
            if (field.number == 5) entity = uint32_t(field.value);
            else if (field.number == 11) reason = uint32_t(field.value);
        }
        return entity != 0;
    }

    bool LoadMarkers(const std::filesystem::path& positions, std::vector<GameChests::Marker>& out) {
        out.clear();
        for (const auto& oculus : OCULI) {
            std::ifstream in(positions / (std::to_string(oculus.label) + ".json"), std::ios::binary);
            if (!in) continue;
            nlohmann::json points = nlohmann::json::parse(in, nullptr, false);
            if (!points.is_array()) continue;
            for (const nlohmann::json& point : points) {
                if (!point.is_object() || point.value("map_id", 0) != TEYVAT) continue;
                GameChests::Marker marker;
                marker.id = point.value("id", int64_t(0));
                marker.label = oculus.label;
                marker.x = point.value("x_pos", 0.0);
                marker.y = point.value("y_pos", 0.0);
                const nlohmann::json& group = point.contains("point_group") ? point["point_group"] : nlohmann::json();
                if (group.is_object()) marker.floor = group.value("floor_id", 0);
                if (marker.id) out.push_back(marker);
            }
        }
        return !out.empty();
    }

    int64_t Match(const GameChests::Chest& oculus, int label, const std::vector<GameChests::Chest>& nearby,
        const std::vector<GameChests::Marker>& markers) {
        if (!label || !GameChests::OnTeyvat(oculus))
            return 0;
        double x, y;
        GameChests::ToMap(oculus, x, y);
        const GameChests::Marker* best = nullptr;
        double bestDistance = MATCH;
        for (const GameChests::Marker& marker : markers) {
            double d = std::hypot(marker.x - x, marker.y - y);
            if (marker.label == label && d < bestDistance) {
                best = &marker;
                bestDistance = d;
            }
        }
        if (!best)
            return 0;
        double reach = std::max(2.0 * bestDistance, bestDistance + MARGIN);
        for (const GameChests::Marker& marker : markers) {
            if (&marker != best && marker.label == label && std::hypot(marker.x - x, marker.y - y) < reach)
                return 0;
        }
        // the same kind of object is the same kind of oculus
        for (const GameChests::Chest& other : nearby) {
            if (other.entity == oculus.entity || other.gadget != oculus.gadget) continue;
            double ox, oy;
            GameChests::ToMap(other, ox, oy);
            if (std::hypot(best->x - ox, best->y - oy) < reach)
                return 0;
        }
        return best->id;
    }

    bool Tracker::Feed(uint16_t id, std::string_view body, uint64_t ms, Picked& picked) {
        if (id == GameChests::APPEARED) {
            std::vector<GameChests::Chest> objects;
            GameChests::ReadObjects(body, objects);
            // a long trip leaves a lot behind
            if (m_objects.size() > 50000) m_objects.clear();
            for (const GameChests::Chest& object : objects) m_objects[object.entity] = object;
            return false;
        }
        if (id == GOT_ITEM) {
            uint32_t item, from;
            if (ReadGotItem(body, item, from) && from == PICKED_UP && LabelOf(item)) {
                m_label = LabelOf(item);
                m_ms = ms;
            }
            return false;
        }
        uint32_t entity, reason;
        if (id != LEFT || !ReadLeft(body, entity, reason))
            return false;
        auto it = m_objects.find(entity);
        if (it == m_objects.end())
            return false;
        GameChests::Chest object = it->second;
        m_objects.erase(it);
        if (!m_label || reason != TAKEN || ms < m_ms || ms - m_ms > TOGETHER_MS)
            return false;
        picked.object = object;
        picked.label = m_label;
        m_label = 0;
        return true;
    }

    std::vector<GameChests::Chest> Tracker::Objects() const {
        std::vector<GameChests::Chest> out;
        out.reserve(m_objects.size());
        for (const auto& [entity, object] : m_objects) out.push_back(object);
        return out;
    }
}
