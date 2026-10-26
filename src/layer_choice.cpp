#include "layer_choice.hpp"
#include <set>

void LayerChoice::Reset() {
    m_guessed = m_read = m_picked = false;
    m_buttons = {};
    m_buttonsAtPick = {};
}

void LayerChoice::Guess(const Layer& layer) {
    m_guess = layer;
    m_guessed = true;
    // another map, the region list can switch to one with the map open
    if (m_read && m_onButtons.mapId != layer.mapId) m_read = false;
    if (m_picked && m_pick.mapId != layer.mapId) m_picked = false;
    // Another underground area: the buttons' floor was one of the old one's.
    // SIFT saying above ground doesn't count, a small lit floor can look like
    // the ground round it.
    if (m_read && layer.groupId != 0 && m_onButtons.groupId != 0 && layer.groupId != m_onButtons.groupId
        && !MapData::groupInside(m_floors, layer.mapId, m_onButtons.groupId, layer.groupId)) {
        m_read = false;
    }
}

void LayerChoice::Read(const GameUi::Switcher& read, int mapId) {
    // covered or unclear: the floor stays
    if (read.seen == GameUi::Seen::Unsure) return;
    // no buttons, the game has nothing but above ground here
    if (read.seen == GameUi::Seen::None) {
        take(read, { mapId, 0, 0 });
        return;
    }
    // SIFT's area, or the one the buttons were last read in while SIFT
    // takes the lit floor for the ground
    int area = m_guessed && m_guess.mapId == mapId && m_guess.groupId != 0 ? m_guess.groupId
        : m_read && m_onButtons.mapId == mapId ? m_onButtons.groupId : 0;
    int groupId = 0, floorId = 0;
    if (GameUi::SwitcherFloor(read, m_floors, mapId, area, groupId, floorId)) {
        take(read, { mapId, groupId, floorId });
        return;
    }
    // a floor lit that SIFT hasn't seen yet: its answer till it looks again
    m_read = false;
}

void LayerChoice::take(const GameUi::Switcher& read, const Layer& layer) {
    Buttons now = { true, read.count, read.lit };
    if (m_picked) {
        // Picked while the buttons were never read: this read is what
        // they showed then, only a change from it is a click.
        if (!m_buttonsAtPick.seen) m_buttonsAtPick = now;
        else if (now != m_buttonsAtPick) m_picked = false;
    }
    m_buttons = now;
    m_onButtons = layer;
    m_read = true;
}

void LayerChoice::Pick(const Layer& layer) {
    m_pick = layer;
    m_picked = true;
    m_buttonsAtPick = m_buttons;
}

void LayerChoice::Auto() {
    m_picked = false;
}

LayerChoice::Layer LayerChoice::Now() const {
    if (m_picked) return m_pick;
    if (m_read) return m_onButtons;
    return m_guessed ? m_guess : Layer{};
}

LayerChoice::From LayerChoice::Why() const {
    if (m_picked) return From::Picked;
    if (m_read) return From::Game;
    return From::Guess;
}

int LayerChoice::Area(double x, double y) const {
    if (!m_guessed && !m_read) return 0;
    Layer now = Now();
    auto holds = [&](int groupId, int& floors, double& size) {
        MapData::Box b;
        if (!MapData::groupBox(m_floors, now.mapId, groupId, b)) return false;
        floors = (int)MapData::groupFloors(m_floors, now.mapId, groupId).size();
        size = (b.right - b.left) * (b.bottom - b.top);
        return x >= b.left && x <= b.right && y >= b.top && y <= b.bottom;
    };
    int floors = 0;
    double size = 0.0;
    if (now.groupId != 0 && holds(now.groupId, floors, size)) return now.groupId;

    std::set<int> groups;
    for (const MapData::Floor& floor : m_floors) {
        if (floor.mapId == now.mapId) groups.insert(floor.groupId);
    }
    int best = 0;
    bool bestFits = false;
    double bestSize = 0.0;
    for (int id : groups) {
        if (!holds(id, floors, size)) continue;
        bool fits = m_buttons.seen && floors + 1 == m_buttons.count;
        if (best == 0 || (fits && !bestFits) || (fits == bestFits && size < bestSize)) {
            best = id;
            bestFits = fits;
            bestSize = size;
        }
    }
    return best;
}

const char* LayerChoice::Name(From from) {
    switch (from) {
    case From::Picked: return "picked";
    case From::Game: return "game";
    default: return "guess";
    }
}
