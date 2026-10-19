#pragma once
#include <GL/glew.h>
#include <filesystem>
#include <utility>
#include <vector>

// HoYoLAB's map tiles as textures, for extensions that draw the map.
// Tiles are downloaded in the background, kept on disk in map_data/tiles and
// shrunk to the size they're drawn at, so a zoomed out map doesn't need
// thousands of full size textures. Tiles that aren't drawn for a while are freed.

namespace MapTiles {
    struct Grid {
        int cols = 0;
        int rows = 0;
        int tileSize = 256;
        double originX = 0.0;  // map units = tile pixels - origin
        double originY = 0.0;
    };

    // an underground floor image and the box of map units it covers
    struct Floor {
        GLuint texture = 0;
        double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
    };

    extern void Init(const std::filesystem::path& dataFolder);
    extern void Shutdown();

    extern bool GetGrid(int mapId, Grid& out);
    // 0 until the tile is loaded, or if there's nothing there. size is 32, 64, 128 or 256.
    extern GLuint GetTile(int mapId, int x, int y, int size);
    // false until the image is loaded, or if the floor has no image
    extern bool GetFloor(int groupId, int floorId, Floor& out);
    // the underground floors on that map whose box holds the spot, as group and
    // floor, in HoYoLAB's order
    extern std::vector<std::pair<int, int>> FloorsAt(int mapId, double x, double y);
    // call once per frame after drawing, uploads finished tiles and frees unused ones
    extern void EndFrame();
}
