#include "layer_choice.hpp"
#include <set>

void LayerChoice::Reset() {
    m_guessed = m_read = m_picked = m_drawnLit = m_unmapped = false;
    m_covered = m_guessAfterCover = false;
    m_buttons = {};
    m_buttonsAtPick = {};
    m_buttonsAtGuess = {};
}

void LayerChoice::Guess(const Layer& layer, bool drawnLit) {
    m_guess = layer;
    m_guessed = true;
    m_drawnLit = drawnLit;
    m_buttonsAtGuess = m_buttons;
    if (m_covered) m_guessAfterCover = true;
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

void LayerChoice::Read(const GameUi::Switcher& read, int mapId, double x, double y) {
    // covered or unclear: the floor stays
    if (read.seen == GameUi::Seen::Unsure) {
        m_covered = true;
        return;
    }
    m_covered = m_guessAfterCover = false;
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
    if (GameUi::SwitcherFloor(read, m_floors, mapId, area, x, y, groupId, floorId)) {
        take(read, { mapId, groupId, floorId });
        return;
    }
    // Areas under x y, none with as many floors as the lit one's side:
    // HoYoLAB has no map for it. x y count once SIFT has found this map.
    bool located = m_guessed && m_guess.mapId == mapId;
    if (located && read.lit >= 0 && read.ground >= 0 && read.lit != read.ground) {
        bool above = read.lit < read.ground;
        int side = above ? read.ground : read.count - 1 - read.ground;
        std::set<int> groups;
        for (const MapData::Floor& floor : m_floors) {
            if (floor.mapId == mapId) groups.insert(floor.groupId);
        }
        bool here = false, fits = false;
        for (int id : groups) {
            MapData::Box b;
            if (!MapData::groupBox(m_floors, mapId, id, b) || x < b.left || x > b.right || y < b.top || y > b.bottom) continue;
            here = true;
            if ((int)MapData::groupFloors(m_floors, mapId, id).size() == side) fits = true;
        }
        if (here && !fits) {
            take(read, { mapId, 0, 0 });
            m_unmapped = true;
            m_unmappedAbove = above;
            m_unmappedPlace = above ? read.ground - read.lit : read.lit - read.ground;
            return;
        }
    }
    // a floor lit that SIFT hasn't seen yet: its answer till it looks again
    m_read = false;
}

void LayerChoice::take(const GameUi::Switcher& read, const Layer& layer) {
    Buttons now = { true, read.count, read.lit, read.ground };
    if (m_picked) {
        // Picked while the buttons were never read: this read is what
        // they showed then, only a change from it is a click.
        if (!m_buttonsAtPick.seen) m_buttonsAtPick = now;
        else if (now != m_buttonsAtPick) m_picked = false;
    }
    // SIFT answered before the buttons were read: this read is what they showed
    if (!m_buttonsAtGuess.seen) m_buttonsAtGuess = now;
    m_buttons = now;
    m_onButtons = layer;
    m_read = true;
    m_unmapped = false;
    if (layer.groupId != 0 && read.ground >= 0 && read.lit >= 0) m_over[layer.groupId] = read.lit < read.ground;
}

void LayerChoice::Pick(const Layer& layer) {
    m_pick = layer;
    m_picked = true;
    m_buttonsAtPick = m_buttons;
}

void LayerChoice::Auto() {
    m_picked = false;
}

bool LayerChoice::drawn() const {
    return m_guessed && m_drawnLit && !(m_buttons != m_buttonsAtGuess);
}

LayerChoice::Layer LayerChoice::Now() const {
    if (m_picked) return m_pick;
    if (drawn()) return m_guess;
    if (m_read && !m_guessAfterCover) return m_onButtons;
    return m_guessed ? m_guess : Layer{};
}

LayerChoice::From LayerChoice::Why() const {
    if (m_picked) return From::Picked;
    if (drawn()) return From::Game;
    if (m_read && !m_guessAfterCover) return From::Game;
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

bool LayerChoice::Unmapped(bool& above, int& place) const {
    if (m_picked || drawn() || !m_read || m_guessAfterCover || !m_unmapped) return false;
    above = m_unmappedAbove;
    place = m_unmappedPlace;
    return true;
}

bool LayerChoice::Over(int groupId) const {
    auto at = m_over.find(groupId);
    return at != m_over.end() && at->second;
}

const char* LayerChoice::Name(From from) {
    switch (from) {
    case From::Picked: return "picked";
    case From::Game: return "game";
    default: return "guess";
    }
}
