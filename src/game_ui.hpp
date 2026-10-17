#pragma once
#include <windows.h>
#include <vector>
#include <opencv2/core.hpp>

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
}
