#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgcodecs.hpp>
#include "map_check.hpp"
#include "test_map.hpp"

using Catch::Approx;
using TestMap::GAME;

static constexpr int MAP_ID = 2;
static constexpr int TILE = 256, COLS = 8, ROWS = 6;

static const cv::Mat& map() {
    static cv::Mat map = TestMap::make(COLS * TILE, ROWS * TILE);
    return map;
}

// the test map cut into tiles like the map data has them, with its manifest
static std::filesystem::path dataFolder() {
    static std::filesystem::path folder;
    if (!folder.empty()) return folder;
    folder = std::filesystem::temp_directory_path() / "genshin-overlay-tests" / "map_data";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "tiles");
    for (int y = 0; y < ROWS; ++y) {
        for (int x = 0; x < COLS; ++x) {
            std::vector<uchar> bytes;
            cv::imencode(".webp", map()(cv::Rect(x * TILE, y * TILE, TILE, TILE)), bytes, { cv::IMWRITE_WEBP_QUALITY, 101 });
            std::ofstream file(folder / "tiles" / (std::to_string(x) + "_" + std::to_string(y) + ".webp"), std::ios::binary);
            file.write((const char*)bytes.data(), bytes.size());
        }
    }
    // no origin, so the map units are the tile pixels
    std::ofstream manifest(folder / "manifest.tsv");
    manifest << "grid\t" << MAP_ID << "\t" << COLS << "\t" << ROWS << "\t" << TILE << "\t0\t0\ttiles\n";
    return folder;
}

static MapLocator::Result viewAt(double x, double y) {
    MapLocator::Result view;
    view.found = true;
    view.mapId = MAP_ID;
    view.lng = x;
    view.lat = y;
    view.unitsPerPixel = 1.0;
    return view;
}

// Asks until the answer for this id is there. Check drops what's asked while
// the thread is busy, so it's asked again and again. False after 5 seconds.
static bool ask(int id, const MapFollow::Patches& patches, const MapLocator::Result& view, MapLocator::Result& out) {
    for (int i = 0; i < 500; ++i) {
        MapCheck::Check(id, patches, GAME, view);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int answered = 0;
        if (MapCheck::Take(answered, out) && answered == id) return true;
    }
    return false;
}

TEST_CASE("the tile check says where the map really is") {
    MapCheck::Start(dataFolder());
    // the game shows the map at 1010, 694 and the follower thinks it's at 1000, 700
    MapFollow::Patches patches = MapFollow::cut(TestMap::screen(map(), 1010, 694), GAME);
    MapLocator::Result out;
    bool answered = ask(1, patches, viewAt(1000, 700), out);
    MapCheck::Stop();

    REQUIRE(answered);
    CHECK(out.lng == Approx(1010.0).margin(0.5));
    CHECK(out.lat == Approx(694.0).margin(0.5));
    CHECK(out.unitsPerPixel == Approx(1.0).margin(0.005));
}

// issue #7, this used to close the whole program
TEST_CASE("the tile check gets through a view past the top or left edge of the map") {
    MapCheck::Start(dataFolder());
    MapFollow::Patches patches = MapFollow::cut(TestMap::screen(map(), 1000, 700), GAME);
    MapLocator::Result out;
    // the top patches are all above the first row of tiles, then the left ones all left of the first column
    MapCheck::Check(1, patches, GAME, viewAt(1000, -100));
    bool afterTop = ask(2, patches, viewAt(1000, 700), out);
    MapCheck::Check(3, patches, GAME, viewAt(100, 700));
    bool afterLeft = ask(4, patches, viewAt(1000, 700), out);
    MapCheck::Stop();

    // it still answers the next one
    CHECK(afterTop);
    CHECK(afterLeft);
}
