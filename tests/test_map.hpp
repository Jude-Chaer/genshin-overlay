#pragma once
#include <windows.h>
#include <opencv2/core.hpp>

// A made up map to test with, so the tests need no game and no map data.

namespace TestMap {
    // the game's window in the tests
    constexpr RECT GAME = { 0, 0, 1280, 720 };

    // gray map of that size, the same one every time
    extern cv::Mat make(int width, int height);
    // What the game would show (BGR) with map pixel (x, y) in the middle of the
    // screen, one screen pixel being perPixel map pixels. Black off the map.
    extern cv::Mat screen(const cv::Mat& map, double x, double y, double perPixel = 1.0);
}
