#pragma once
#include <vector>
#include "game_ui.hpp"
#include "map_data.hpp"

// Which layer of the big map the overlay shows: above ground or one of the
// underground floors. SIFT says where the map is and on which underground
// area, the game's layer buttons say which floor of it is lit.
//   picked  wins until the map closes, Auto is picked, or the game's buttons
//           are clicked (their read changes from what it was at the pick)
//   game    the floor the buttons last showed, in SIFT's area. No buttons
//           there means above ground. Covered or unclear keeps it, until SIFT
//           finds another area.
//   guess   SIFT's, until the buttons are read in its area.
// Owned by the follow thread, one per map open.
class LayerChoice {
public:
    enum class From { Guess, Game, Picked };
    struct Layer {
        int mapId = 0, groupId = 0, floorId = 0;
        bool operator==(const Layer& other) const {
            return mapId == other.mapId && groupId == other.groupId && floorId == other.floorId;
        }
        bool operator!=(const Layer& other) const { return !(*this == other); }
    };

    // HoYoLAB's floors, in its order
    void SetFloors(std::vector<MapData::Floor> floors) { m_floors = std::move(floors); }
    bool HasFloors() const { return !m_floors.empty(); }

    // the map closed, nothing from before counts
    void Reset();
    // SIFT found the map, on that layer
    void Guess(const Layer& layer);
    // A read of the layer buttons, every one even when nothing was seen
    void Read(const GameUi::Switcher& read, int mapId);
    void Pick(const Layer& layer);
    void Auto();

    Layer Now() const;
    From Why() const;
    // The underground area whose floors the overlay's picker lists, 0 for
    // none: the layer's own while its floors' box holds x, y (map units), or
    // else the smallest one that does. With the buttons read, one with as
    // many floors as they show goes first. Above ground the buttons don't
    // name the area, the map's place does.
    int Area(double x, double y) const;
    // "picked", "game" or "guess"
    static const char* Name(From from);

private:
    // the buttons read as that layer
    void take(const GameUi::Switcher& read, const Layer& layer);

    struct Buttons {
        bool seen = false;
        int count = 0, lit = 0;
        bool operator!=(const Buttons& other) const {
            return seen != other.seen || count != other.count || lit != other.lit;
        }
    };

    std::vector<MapData::Floor> m_floors;
    bool m_guessed = false, m_read = false, m_picked = false;
    Layer m_guess, m_onButtons, m_pick;
    // the buttons as last read, and as they were when the floor was picked
    Buttons m_buttons, m_buttonsAtPick;
};
