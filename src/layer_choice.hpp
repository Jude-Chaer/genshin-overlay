#pragma once
#include <map>
#include <vector>
#include "game_ui.hpp"
#include "map_data.hpp"

// Which layer of the big map the overlay shows: above ground or one of the
// underground floors. SIFT says where the map is and on which underground
// area. The floor lit is read from the screen: the one the game draws in full
// colour, or else the lit one of the game's layer buttons.
//   picked  wins until the map closes, Auto is picked, or the game's buttons
//           are clicked (their read changes from what it was at the pick)
//   game    SIFT's floor when it's the one drawn lit, until the buttons
//           change. Else the floor the buttons last showed, in SIFT's area.
//           No buttons there means above ground. Covered or unclear keeps it
//           until SIFT's next answer, the game can switch floors behind a panel.
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
    // SIFT found the map, on that layer. drawnLit: the floor is the one the
    // game draws lit.
    void Guess(const Layer& layer, bool drawnLit = false);
    // A read of the layer buttons, every one even when nothing was seen, and
    // the map position (map units) at the middle of the screen
    void Read(const GameUi::Switcher& read, int mapId, double x, double y);
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
    // The game's buttons show a floor lit that no HoYoLAB area fits: the
    // overlay is above ground then. above: the floor is over the ground,
    // place: its count from the ground's button, 1 next to it.
    bool Unmapped(bool& above, int& place) const;
    // the area's floors are over the ground's button, as the buttons last
    // showed. Under it until they're seen.
    bool Over(int groupId) const;

private:
    // SIFT's floor is the one drawn lit and the buttons stayed as they were
    bool drawn() const;
    // the buttons read as that layer
    void take(const GameUi::Switcher& read, const Layer& layer);

    struct Buttons {
        bool seen = false;
        int count = 0, lit = 0, ground = 0;
        bool operator!=(const Buttons& other) const {
            return seen != other.seen || count != other.count || lit != other.lit || ground != other.ground;
        }
    };

    std::vector<MapData::Floor> m_floors;
    // each area's side of the ground, kept while the overlay runs
    std::map<int, bool> m_over;
    bool m_unmapped = false, m_unmappedAbove = false;
    int m_unmappedPlace = 0;
    bool m_guessed = false, m_read = false, m_picked = false, m_drawnLit = false;
    // the buttons were covered since their last read, and SIFT answered since
    bool m_covered = false, m_guessAfterCover = false;
    Layer m_guess, m_onButtons, m_pick;
    // the buttons as last read, and as they were when the floor was picked
    // and when SIFT last answered
    Buttons m_buttons, m_buttonsAtPick, m_buttonsAtGuess;
};
