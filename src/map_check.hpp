#pragma once
#include <windows.h>
#include <filesystem>
#include "map_follow.hpp"
#include "locator/map_locator.hpp"

// Lines the follower up with the map itself. HoYoLAB's tiles are drawn where
// the follower has the map and compared with the same patches of the game,
// which says how far off it is. Comparing frame to frame adds up small errors,
// this doesn't.
//
// Runs on its own thread. The answer comes a frame or two later and is for the
// frame it was asked about, like SIFT's.

namespace MapCheck {
    // reads the tile grids from the map data's manifest and starts the thread
    extern void Start(const std::filesystem::path& dataFolder);
    extern void Stop();
    // Asks about one frame: its patches, and the view the follower has on it.
    // Dropped if the last one isn't done yet.
    extern void Check(int id, const MapFollow::Patches& patches, const RECT& game, const MapLocator::Result& view);
    // Where the map really was on frame id. False if there's no new answer,
    // also when the tiles couldn't tell.
    extern bool Take(int& id, MapLocator::Result& out);
}
