#pragma once
#include <vector>
#include <opencv2/core.hpp>

// Which underground floor the game draws lit. With a floor picked, the game
// draws it in full colour and fades the floors around it.

namespace MapLocator {

    struct FloorArt {
        int groupId = 0;
        int floorId = 0;
        // the map units the art is stretched over
        double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
        cv::Mat gray;   // CV_8U
        cv::Mat alpha;  // CV_8U, where the art is drawn
    };

    // where the screen is on the map: the units at its top left corner and per pixel
    struct ScreenView {
        double left = 0.0;
        double top = 0.0;
        double unitsPerPixel = 1.0;
    };

    // the art at most this many pixels across, enough to compare brightness
    constexpr int FLOOR_ART_SIZE = 512;

    // the art of one floor image (BGRA, BGR or gray) shrunk for LitFloor
    FloorArt MakeFloorArt(const cv::Mat& image, int groupId, int floorId, double left, double top, double right, double bottom);

    // The floor in arts the screen shows lit, -1 when none is. screenGray is the
    // game in gray, mask 0 over the game's UI (empty: all of it counts).
    int LitFloor(const cv::Mat& screenGray, const cv::Mat& mask, const ScreenView& view, const std::vector<const FloorArt*>& arts);

}
