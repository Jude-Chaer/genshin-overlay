#pragma once
#include <windows.h>
#include <vector>
#include <opencv2/core.hpp>
#include "map_data.hpp"

// What the game shows on the big map besides the map: a panel open on the
// right, the layer switcher, and where its own UI sits so extensions can keep
// off it. Nothing here keeps state, every answer comes from one picture.
namespace GameUi {
    // None: not there. Unsure: can't tell, something covers it or it's unclear.
    enum class Seen { None, Read, Unsure };

    enum class PanelKind { None, RegionList, Info };
    struct Panel {
        Seen seen = Seen::None;
        PanelKind kind = PanelKind::None;
        // where the panel starts, in the picture's pixels, -1 with none
        int left = -1;
    };

    // The layer switcher: the top button is above ground, the floors follow in
    // HoYoLAB's order.
    struct Switcher {
        Seen seen = Seen::None;
        int count = 0;
        // 0 above ground, 1 and on the floors from the top, -1 when it can't tell
        int lit = -1;
    };

    // Parts of one game picture, each where it sits in it, BGR or BGRA. The
    // readers only need the parts PanelParts and SwitcherParts name, so the
    // follower copies just those. A whole picture is one part at 0 0.
    struct Picture {
        cv::Size size;
        std::vector<cv::Mat> parts;
        std::vector<cv::Point> at;
    };
    extern Picture Whole(const cv::Mat& frame);
    // What each reader looks at, in the pixels of a picture that big
    extern std::vector<cv::Rect> PanelParts(cv::Size size);
    extern std::vector<cv::Rect> SwitcherParts(cv::Size size);

    extern Panel ReadPanel(const Picture& picture);
    extern Switcher ReadSwitcher(const Picture& picture, const Panel& panel);
    inline Panel ReadPanel(const cv::Mat& frame) { return ReadPanel(Whole(frame)); }
    inline Switcher ReadSwitcher(const cv::Mat& frame, const Panel& panel) { return ReadSwitcher(Whole(frame), panel); }

    // The game UI on screen, as boxes in the same coords as game. The panel's
    // left is in the game picture's pixels, as ReadPanel gives it. With a
    // panel open the panel itself is one of the boxes.
    extern std::vector<RECT> Boxes(const RECT& game, const Panel& panel);

    // The floor a read switcher has lit, as group and floor, 0 0 above ground.
    // area is the underground group SIFT found the map on: the switcher lists
    // its floors, or those of a smaller group inside it (a small floor lit
    // inside a big one, SIFT matches the big one round it). False when the
    // buttons fit neither, or with no area to go by.
    extern bool SwitcherFloor(const Switcher& read, const std::vector<MapData::Floor>& floors, int mapId,
        int area, int& groupId, int& floorId);
}
