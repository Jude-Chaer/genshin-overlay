#pragma once
#include <GL/glew.h>
#include <filesystem>

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
    // A tile that's on the map's grid, at one of the sizes below. Anything else
    // is never downloaded: an extension asks with any numbers.
    extern bool IsTile(const Grid& grid, int x, int y, int size);
    // 0 until the tile is loaded, or if there's nothing there. size is 32, 64, 128 or 256.
    extern GLuint GetTile(int mapId, int x, int y, int size);
    // false until the image is loaded, or if the floor has no image
    extern bool GetFloor(int groupId, int floorId, Floor& out);
    // a tile's or floor's texture that's loaded now
    extern bool Owns(GLuint texture);
    // call once per frame after drawing, uploads finished tiles and frees unused ones
    extern void EndFrame();
}
