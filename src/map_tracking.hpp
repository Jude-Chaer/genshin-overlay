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
    };

    // gets the map data into dataFolder if needed, then loads it, all on a worker thread
    extern void Start(const std::filesystem::path& dataFolder);
    extern void Stop();
    // call every frame, grabs a new screenshot when the last one is done
    extern void Tick(HWND overlayWindow);
    extern Status GetStatus();
    // what the download is doing, while the status is Downloading
    extern std::string GetDownloadStage(int& percent);
    extern MapView GetView();
    // how long the last screenshot took to match
    extern float GetLastStepMs();
    extern const char* GetMapName(int mapId);
}
