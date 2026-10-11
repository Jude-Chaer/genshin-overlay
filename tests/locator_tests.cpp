#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgproc.hpp>
#include "locator/map_locator.hpp"
#include "test_map.hpp"

using Catch::Approx;
using TestMap::GAME;

// The locator on the made up map: the index is built from its tiles, then
// screens cut from the same map have to be found where they were cut.
// This doesn't say how well it does on the game's real map.

static constexpr int MAP_ID = 2;

static const cv::Mat& map() {
    static cv::Mat map = TestMap::make(2048, 1536);
    return map;
}

static std::filesystem::path dataFolder() {
    static std::filesystem::path folder;
    if (!folder.empty()) return folder;
    folder = std::filesystem::temp_directory_path() / "genshin-overlay-tests" / "locator";
    TestMap::save(map(), folder, MAP_ID);
    return folder;
}

// Built once, the tests share it. It's never let go: as a plain static it would
// be freed when the program ends, after OpenCV's own things, and that crashes.
static MapLocator::Index& index() {
    static MapLocator::Index* index = new MapLocator::Index;
    if (!index->IsReady()) index->Load(dataFolder() / "manifest.tsv");
    return *index;
}

static MapLocator::Result locate(const cv::Mat& screen, bool fast = false) {
    return index().Match(screen, screen.cols / 2.0, screen.rows / 2.0, fast);
}

TEST_CASE("the locator finds where a screen is on the map") {
    REQUIRE(index().IsReady());
    MapLocator::Result result = locate(TestMap::screen(map(), 1000, 700));
    REQUIRE(result.found);
    CHECK(result.mapId == MAP_ID);
    CHECK(result.groupId == 0);
    CHECK(result.floorId == 0);
    CHECK(result.lng == Approx(1000.0).margin(2.0));
    CHECK(result.lat == Approx(700.0).margin(2.0));
    CHECK(result.unitsPerPixel == Approx(1.0).margin(0.01));

    // somewhere else on it
    result = locate(TestMap::screen(map(), 1300, 850));
    REQUIRE(result.found);
    CHECK(result.lng == Approx(1300.0).margin(2.0));
    CHECK(result.lat == Approx(850.0).margin(2.0));
}

TEST_CASE("the locator sees how far the map is zoomed out") {
    REQUIRE(index().IsReady());
    MapLocator::Result result = locate(TestMap::screen(map(), 1024, 768, 1.5));
    REQUIRE(result.found);
    CHECK(result.lng == Approx(1024.0).margin(3.0));
    CHECK(result.lat == Approx(768.0).margin(3.0));
    CHECK(result.unitsPerPixel == Approx(1.5).margin(0.015));
}

TEST_CASE("the quick match used while following finds it too") {
    REQUIRE(index().IsReady());
    MapLocator::Result result = locate(TestMap::screen(map(), 1000, 700), true);
    REQUIRE(result.found);
    CHECK(result.lng == Approx(1000.0).margin(3.0));
    CHECK(result.lat == Approx(700.0).margin(3.0));
}

TEST_CASE("the position is the one under the pixel that was asked for") {
    REQUIRE(index().IsReady());
    cv::Mat screen = TestMap::screen(map(), 1000, 700);
    // 200 right and 100 up from the middle of the screen
    MapLocator::Result result = index().Match(screen, screen.cols / 2.0 + 200, screen.rows / 2.0 - 100);
    REQUIRE(result.found);
    CHECK(result.lng == Approx(1200.0).margin(2.0));
    CHECK(result.lat == Approx(600.0).margin(2.0));
}

TEST_CASE("the locator finds nothing on a screen that isn't the map") {
    REQUIRE(index().IsReady());
    cv::Mat black = cv::Mat::zeros(GAME.bottom, GAME.right, CV_8UC3);
    CHECK(!locate(black).found);

    // looks like the map, but it's a different one
    cv::Mat noise(GAME.bottom, GAME.right, CV_8U), other;
    cv::RNG random(99);
    random.fill(noise, cv::RNG::UNIFORM, 0, 256);
    cv::GaussianBlur(noise, other, cv::Size(0, 0), 3.0);
    cv::normalize(other, other, 0, 255, cv::NORM_MINMAX);
    cv::cvtColor(other, other, cv::COLOR_GRAY2BGR);
    CHECK(!locate(other).found);
}

TEST_CASE("the index is kept in index.bin and read from there the next time") {
    REQUIRE(index().IsReady());
    std::filesystem::path manifestPath = dataFolder() / "manifest.tsv";
    std::ifstream file(manifestPath, std::ios::binary);
    std::string manifest((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    REQUIRE(std::filesystem::exists(dataFolder() / "index.bin"));
    CHECK(MapLocator::IndexMatchesManifest(dataFolder() / "index.bin", manifest));
    // built from another manifest, so it would be built again
    CHECK(!MapLocator::IndexMatchesManifest(dataFolder() / "index.bin", manifest + "grid\t3\t1\t1\t256\t0\t0\tother\n"));

    MapLocator::Index again;
    REQUIRE(again.Load(manifestPath));
    cv::Mat screen = TestMap::screen(map(), 1000, 700);
    MapLocator::Result first = locate(screen);
    MapLocator::Result second = again.Match(screen, screen.cols / 2.0, screen.rows / 2.0);
    REQUIRE(second.found);
    CHECK(second.lng == Approx(first.lng).margin(0.01));
    CHECK(second.lat == Approx(first.lat).margin(0.01));
}
