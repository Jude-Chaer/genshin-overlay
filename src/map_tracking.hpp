#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include "locator/map_locator.hpp"

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
        // screen x where a panel on the right starts, -1 with none open
        LONG panelLeft = -1;
        // the layer was picked by hand, see ForceLayer
        bool layerForced = false;
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
    // The user picked the layer: underground group and floor, 0 0 above
    // ground, on that map. The view stays on it whatever SIFT finds, until the
    // map closes or AutoLayer.
    extern void ForceLayer(int mapId, int groupId, int floorId);
    // back to the layer SIFT finds
    extern void AutoLayer();
    // what the download is doing, while the status is Downloading
    extern std::string GetDownloadStage(int& percent);
    // where the map is when what's drawn now shows up
    extern MapView GetView();
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
