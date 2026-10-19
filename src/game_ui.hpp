#pragma once
#include <windows.h>
#include <vector>
#include <opencv2/core.hpp>
#include "map_data.hpp"

// Where the game's own UI is on the big map, so extensions can keep from
// drawing over it.
namespace GameUi {
    // The first column of a panel open on the right (the region list, a
    // point's info), in pixels of wide, the follower's shrunk gray frame. -1
    // when there's none.
    extern int FindPanel(const cv::Mat& wide);
    // The game UI on screen, in screen coords like game. panelLeft is where a
    // panel starts, in screen coords too, or -1 with none open. With a panel
    // open the panel itself is one of the boxes.
    extern std::vector<RECT> Boxes(const RECT& game, long panelLeft);

    // Where to grab the layer switcher, a narrow strip in screen coords.
    extern RECT SwitcherStrip(const RECT& game, long panelLeft);
    // What the layer switcher shows: how many buttons, the top one is above
    // ground and the floors follow in HoYoLAB's order, and which one is lit.
    // Count is 0 when there's no switcher or it can't be read.
    struct Switcher {
        int count = 0;
        int lit = -1;
    };
    // strip is the BGR grab of SwitcherStrip, gameHeight the game's height in pixels
    extern Switcher ReadSwitcher(const cv::Mat& strip, int gameHeight);
    // The layer a read switcher is on, as group and floor, 0 0 above ground.
    // floors are HoYoLAB's in its order. The switcher lists the floors of one
    // group under x y, the middle of the map, all of them or the ones whose
    // box holds x y. When two groups fit,
    // preferGroup if it's one of them, else the one x y is most in the middle
    // of. False if none fits.
    extern bool SwitcherLayer(const Switcher& read, const std::vector<MapData::Floor>& floors, int mapId,
        double x, double y, int preferGroup, int& groupId, int& floorId);
}
