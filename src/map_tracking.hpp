#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include "locator/map_locator.hpp"
#include "game_ui.hpp"
#include "layer_choice.hpp"

namespace MapTracking {
    enum class Status {
        NoData,
        Downloading,
        Loading,
        Ready,
        Failed
    };

    struct MapView {
        bool visible = false;
        MapLocator::Result result;
        RECT gameRect = {};  // screen coords, the result's lat/lng is at its center
        double secondsSinceUpdate = 0.0;
        // moving, or stopped but SIFT didn't say exactly where yet
        bool moving = false;
        // a panel open on the right, its left in the game's pixels from gameRect's left
        GameUi::Panel panel;
        // who said the result's layer
        LayerChoice::From layerFrom = LayerChoice::From::Guess;
        // The floor picker's area and its floors from the top, in the order
        // of the game's buttons. 0 and empty where there are none.
        int pickerGroup = 0;
        std::vector<int> pickerFloors;
        // goes up every time the map opens, a picker click names the one it was made on
        int opening = 0;
    };

    struct Diamond
    {
        cv::Point2f center;
        float width;
        float height;
        cv::RotatedRect rect;
    };

    // gets the map data into dataFolder if needed, then loads it, all on a worker thread
    extern void Start(const std::filesystem::path& dataFolder);
    extern void Stop();
    // call every frame, tells the tracker which window is ours
    extern void Tick(HWND overlayWindow);
    extern Status GetStatus();
    // the zoom bar is on screen, so the in-game map is open
    extern bool IsMapOpen();
    // Call every frame while something draws the moving map. Without it
    // there's no view while the map moves, it's only followed loosely and
    // found exactly once it stops.
    extern void FollowWhileMoving();
    // what the download is doing, while the status is Downloading
    extern std::string GetDownloadStage(int& percent);
    // where the map is when what's drawn now shows up
    extern MapView GetView();
    // The overlay's floor picker: a floor of the map shown (group 0 is above
    // ground), or Auto to go back to the game's buttons. opening is the view's
    // the click was made on, the follow thread takes it on its next step only
    // if the map is still open from then.
    extern void PickLayer(int opening, const LayerChoice::Layer& layer);
    extern void PickAuto(int opening);
    // The big map's view as it was last found, also after it closed. Its
    // visible says nothing here.
    extern MapView LastView();
    // the game, or our own menu, is the window in front
    extern bool GameInFront();
    // Draw loop, after drawing a frame: sleeps until the follower has a new
    // view, or until the next screen refresh if there's no map.
    extern void WaitForDraw();
    // Where a picture of the map is, its middle as the spot. For the minimap,
    // the big map is found by the tracker itself.
    extern MapLocator::Result Locate(const cv::Mat& image);
    // how long the last screenshot took to match
    extern float GetLastStepMs();
    extern const char* GetMapName(int mapId);

    // Separate thread that watches for whether or not the map is open by scanning for zoom bar
    extern bool getZoomBarRect(HWND game, RECT& out);
    extern bool detectZoomBar(const cv::Mat& image);
	extern void loadZoomTemplate();
    extern void zoombarDetectorLoop();


}
