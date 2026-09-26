#pragma once
#include <windows.h>
#include <filesystem>
#include "locator/map_locator.hpp"

namespace MapTracking {
    enum class Status {
        NoData,
        Loading,
        Ready,
        Failed
    };

    struct MapView {
        bool visible = false;
        MapLocator::Result result;
        RECT gameRect = {};  // screen coords, the result's lat/lng is at its center
    };

    extern void Start(const std::filesystem::path& manifestPath);
    extern void Stop();
    // call every frame, grabs a new screenshot when the last one is done
    extern void Tick(HWND overlayWindow);
    extern Status GetStatus();
    extern MapView GetView();
    extern const char* GetMapName(int mapId);
}
