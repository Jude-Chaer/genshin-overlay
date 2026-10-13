#include <filesystem>
#include <fstream>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "minimap.hpp"
#include "test_map.hpp"

using Catch::Approx;

static constexpr int MAP_ID = 2;
static constexpr int TILE = 256, COLS = 8, ROWS = 6;
// a floor under part of the map, where it lies in map units
static constexpr int GROUP_ID = 9, FLOOR_ID = 7;
static constexpr int FLOOR_LEFT = 600, FLOOR_TOP = 400, FLOOR_WIDTH = 800, FLOOR_HEIGHT = 600;

// The made up map blown up 8 times: the minimap shows the map small, details
// of a few pixels would be gone on it.
static const cv::Mat& map() {
    static cv::Mat map;
    if (map.empty()) cv::resize(TestMap::make(COLS * TILE / 8, ROWS * TILE / 8), map, cv::Size(COLS * TILE, ROWS * TILE), 0, 0, cv::INTER_CUBIC);
    return map;
}

// the floor's picture: the map turned around, so it looks like nothing above it
static const cv::Mat& floorMap() {
    static cv::Mat floor;
    if (floor.empty()) {
        cv::Mat turned;
        cv::flip(map(), turned, -1);
        floor = turned(cv::Rect(300, 200, FLOOR_WIDTH, FLOOR_HEIGHT)).clone();
    }
    return floor;
}

// the map as tiles, like the map data has it
static void loadData() {
    static std::filesystem::path folder;
    if (folder.empty()) {
        folder = std::filesystem::temp_directory_path() / "genshin-overlay-tests" / "minimap_data";
        TestMap::save(map(), folder, MAP_ID, TILE);

        // the floor as a see-through picture, one pixel per map unit
        cv::Mat picture;
        cv::cvtColor(floorMap(), picture, cv::COLOR_GRAY2BGRA);
        std::filesystem::create_directories(folder / "overlays");
        cv::imwrite((folder / "overlays" / "floor.png").string(), picture);
        std::ofstream manifest(folder / "manifest.tsv", std::ios::app);
        manifest << "image\t" << MAP_ID << "\t" << GROUP_ID << "\t" << FLOOR_ID << "\t" << FLOOR_LEFT << "\t" << FLOOR_TOP
            << "\toverlays/floor.png\t" << FLOOR_LEFT + FLOOR_WIDTH << "\t" << FLOOR_TOP + FLOOR_HEIGHT << "\n";
    }
    MiniMap::Load(folder);
}

// what the minimap shows with the player at pixel (x, y) of a picture
static cv::Mat miniOf(const cv::Mat& picture, double x, double y, double zoom) {
    double perPixel = MiniMap::UNITS_ACROSS / zoom / MiniMap::SIZE;
    cv::Matx23d to(perPixel, 0.0, x + (0.5 - MiniMap::SIZE / 2.0) * perPixel - 0.5, 0.0, perPixel, y + (0.5 - MiniMap::SIZE / 2.0) * perPixel - 0.5);
    cv::Mat mini;
    cv::warpAffine(picture, mini, to, cv::Size(MiniMap::SIZE, MiniMap::SIZE), cv::INTER_AREA | cv::WARP_INVERSE_MAP);

    // the N the game has at the top of it
    cv::Mat big(88, 96, CV_8U, cv::Scalar(90)), letter;
    cv::putText(big, "N", cv::Point(18, 76), cv::FONT_HERSHEY_SIMPLEX, 3.0, cv::Scalar(255), 5, cv::LINE_AA);
    cv::resize(big, letter, cv::Size(12, 11), 0, 0, cv::INTER_AREA);
    letter.copyTo(mini(cv::Rect(46, 3, 12, 11)));
    return mini;
}

// with the player at map pixel (x, y)
static cv::Mat miniAt(double x, double y, double zoom = 1.0) {
    return miniOf(map(), x, y, zoom);
}

// a minimap of somewhere the map doesn't have, like in a domain
static cv::Mat miniElsewhere(double x, double y) {
    static cv::Mat other;
    if (other.empty()) {
        other = cv::Mat(600, 600, CV_8U);
        cv::RNG rng(7);
        rng.fill(other, cv::RNG::UNIFORM, 0, 255);
        cv::GaussianBlur(other, other, cv::Size(0, 0), 4.0);
    }
    return miniOf(other, x, y, 1.0);
}

// with the player at map pixel (x, y), down on the floor
static cv::Mat miniOnFloor(double x, double y, double zoom = 1.0) {
    return miniOf(floorMap(), x - FLOOR_LEFT, y - FLOOR_TOP, zoom);
}

TEST_CASE("the minimap's square grows with the game's height", "[minimap]") {
    RECT small = MiniMap::rect({ 0, 0, 1920, 1080 });
    CHECK(small.left == 64);
    CHECK(small.top == 21);
    CHECK(small.right - small.left == 208);
    CHECK(small.bottom - small.top == 208);

    RECT big = MiniMap::rect({ 100, 50, 100 + 3840, 50 + 2160 });
    CHECK(big.left == 100 + 128);
    CHECK(big.top == 50 + 42);
    CHECK(big.right - big.left == 416);
}

TEST_CASE("a grab of the minimap is shrunk to the compare size", "[minimap]") {
    cv::Mat grab(416, 416, CV_8UC3, cv::Scalar(10, 20, 30));
    cv::Mat mini = MiniMap::shrink(grab);
    CHECK(mini.cols == MiniMap::SIZE);
    CHECK(mini.rows == MiniMap::SIZE);
    CHECK(mini.channels() == 1);
}

TEST_CASE("the player is found around where they last were", "[minimap]") {
    loadData();
    cv::Mat mini = miniAt(900.0, 700.0);

    MiniMap::Spot spot = MiniMap::nearby(mini, MAP_ID, 700.0 + 20.0, 900.0 - 30.0);
    REQUIRE(spot.found);
    CHECK(spot.mapId == MAP_ID);
    CHECK(spot.lng == Approx(900.0).margin(1.5));
    CHECK(spot.lat == Approx(700.0).margin(1.5));
}

TEST_CASE("the player isn't found where the minimap doesn't show", "[minimap]") {
    loadData();
    cv::Mat mini = miniAt(900.0, 700.0);

    // far from there
    CHECK_FALSE(MiniMap::nearby(mini, MAP_ID, 400.0, 1500.0).found);
    // a map that isn't there
    CHECK_FALSE(MiniMap::nearby(mini, MAP_ID + 1, 700.0, 900.0).found);
    // a black screen
    cv::Mat black(MiniMap::SIZE, MiniMap::SIZE, CV_8U, cv::Scalar(0));
    CHECK_FALSE(MiniMap::nearby(black, MAP_ID, 700.0, 900.0).found);
}

TEST_CASE("the player is found on the whole map with no last spot", "[minimap]") {
    loadData();
    cv::Mat mini = miniAt(1400.0, 500.0);

    MiniMap::Spot spot = MiniMap::anywhere(mini);
    REQUIRE(spot.found);
    CHECK(spot.mapId == MAP_ID);
    CHECK(spot.lng == Approx(1400.0).margin(1.5));
    CHECK(spot.lat == Approx(500.0).margin(1.5));

    cv::Mat black(MiniMap::SIZE, MiniMap::SIZE, CV_8U, cv::Scalar(0));
    CHECK_FALSE(MiniMap::anywhere(black).found);
}

TEST_CASE("a wide look finds the player further from the last spot", "[minimap]") {
    loadData();
    cv::Mat mini = miniAt(900.0, 700.0);

    CHECK_FALSE(MiniMap::nearby(mini, MAP_ID, 700.0 + 90.0, 900.0 - 120.0).found);
    MiniMap::Spot spot = MiniMap::nearby(mini, MAP_ID, 700.0 + 90.0, 900.0 - 120.0, true);
    REQUIRE(spot.found);
    CHECK(spot.lng == Approx(900.0).margin(1.5));
    CHECK(spot.lat == Approx(700.0).margin(1.5));
}

TEST_CASE("the player is followed from one look to the next", "[minimap]") {
    loadData();
    MiniMap::Watcher watcher;
    bool searched = false;

    MiniMap::Spot spot = watcher.Look(miniAt(900.0, 700.0), 0.0, &searched);
    REQUIRE(spot.found);
    CHECK(searched);
    CHECK(spot.lng == Approx(900.0).margin(1.5));

    // a few steps on: found around the last spot, no search
    spot = watcher.Look(miniAt(915.0, 690.0), 0.2, &searched);
    REQUIRE(spot.found);
    CHECK_FALSE(searched);
    CHECK(spot.lng == Approx(915.0).margin(1.5));
    CHECK(spot.lat == Approx(690.0).margin(1.5));

    // no minimap: not found, but it remembers where it was
    cv::Mat black(MiniMap::SIZE, MiniMap::SIZE, CV_8U, cv::Scalar(0));
    spot = watcher.Look(black, 0.4, &searched);
    CHECK_FALSE(spot.found);
    CHECK(spot.lng == Approx(915.0).margin(1.5));

    // and it's found again after a teleport
    spot = watcher.Look(miniAt(1400.0, 500.0), 20.0, &searched);
    REQUIRE(spot.found);
    CHECK(searched);
    CHECK(spot.lng == Approx(1400.0).margin(1.5));
    CHECK(spot.lat == Approx(500.0).margin(1.5));
}

TEST_CASE("the player is followed into a city, where the minimap zooms in", "[minimap]") {
    loadData();
    MiniMap::Watcher watcher;

    MiniMap::Spot spot = watcher.Look(miniAt(900.0, 700.0), 0.0);
    REQUIRE(spot.found);
    CHECK(spot.zoom == 1.0);

    spot = watcher.Look(miniAt(905.0, 700.0, MiniMap::CITY_ZOOM), 0.2);
    REQUIRE(spot.found);
    CHECK(spot.zoom == MiniMap::CITY_ZOOM);
    CHECK(spot.lng == Approx(905.0).margin(1.5));
    CHECK(spot.lat == Approx(700.0).margin(1.5));

    spot = watcher.Look(miniAt(910.0, 700.0), 0.4);
    REQUIRE(spot.found);
    CHECK(spot.zoom == 1.0);
    CHECK(spot.lng == Approx(910.0).margin(1.5));
}

TEST_CASE("a zoom nobody told it about is worked out and kept", "[minimap]") {
    loadData();
    MiniMap::Watcher watcher;
    REQUIRE(watcher.Look(miniAt(900.0, 700.0), 0.0).found);

    MiniMap::Spot spot = watcher.Look(miniAt(905.0, 700.0, 1.8), 0.2);
    REQUIRE(spot.found);
    CHECK(spot.zoom == Approx(1.8).epsilon(0.02));
    CHECK(spot.lng == Approx(905.0).margin(2.0));
    CHECK(spot.lat == Approx(700.0).margin(2.0));

    // back out and in again, now without a search
    REQUIRE(watcher.Look(miniAt(910.0, 700.0), 0.4).found);
    bool searched = true;
    spot = watcher.Look(miniAt(915.0, 700.0, 1.8), 0.6, &searched);
    REQUIRE(spot.found);
    CHECK_FALSE(searched);
    CHECK(spot.lng == Approx(915.0).margin(2.0));
}

TEST_CASE("the player is followed down to a floor and back up", "[minimap]") {
    loadData();
    MiniMap::Watcher watcher;

    MiniMap::Spot spot = watcher.Look(miniAt(1000.0, 700.0), 0.0);
    REQUIRE(spot.found);
    CHECK(spot.floorId == 0);

    spot = watcher.Look(miniOnFloor(1005.0, 700.0), 0.2);
    REQUIRE(spot.found);
    CHECK(spot.mapId == MAP_ID);
    CHECK(spot.groupId == GROUP_ID);
    CHECK(spot.floorId == FLOOR_ID);
    CHECK(spot.lng == Approx(1005.0).margin(1.5));
    CHECK(spot.lat == Approx(700.0).margin(1.5));

    // walking on down there
    spot = watcher.Look(miniOnFloor(1015.0, 690.0), 0.4);
    REQUIRE(spot.found);
    CHECK(spot.floorId == FLOOR_ID);
    CHECK(spot.lng == Approx(1015.0).margin(1.5));
    CHECK(spot.lat == Approx(690.0).margin(1.5));

    spot = watcher.Look(miniAt(1020.0, 690.0), 0.6);
    REQUIRE(spot.found);
    CHECK(spot.groupId == 0);
    CHECK(spot.floorId == 0);
    CHECK(spot.lng == Approx(1020.0).margin(1.5));
}

TEST_CASE("the player is found on a floor with no last spot", "[minimap]") {
    loadData();

    MiniMap::Spot spot = MiniMap::anywhere(miniOnFloor(980.0, 720.0));
    REQUIRE(spot.found);
    CHECK(spot.floorId == FLOOR_ID);
    CHECK(spot.lng == Approx(980.0).margin(1.5));
    CHECK(spot.lat == Approx(720.0).margin(1.5));

    // up on the surface it's still the surface
    spot = MiniMap::anywhere(miniAt(980.0, 720.0));
    REQUIRE(spot.found);
    CHECK(spot.floorId == 0);
}

TEST_CASE("a floor with a zoom of its own is found", "[minimap]") {
    loadData();

    // walking down into it
    MiniMap::Watcher walked;
    REQUIRE(walked.Look(miniAt(1000.0, 700.0), 0.0).found);
    MiniMap::Spot spot = walked.Look(miniOnFloor(1005.0, 700.0, 2.0), 0.2);
    REQUIRE(spot.found);
    CHECK(spot.floorId == FLOOR_ID);
    CHECK(spot.zoom == Approx(2.0).epsilon(0.02));
    CHECK(spot.lng == Approx(1005.0).margin(2.0));
    CHECK(spot.lat == Approx(700.0).margin(2.0));

    // and with no last spot
    MiniMap::Watcher fresh;
    spot = fresh.Look(miniOnFloor(980.0, 720.0, 2.0), 0.0);
    REQUIRE(spot.found);
    CHECK(spot.floorId == FLOOR_ID);
    CHECK(spot.zoom == Approx(2.0).epsilon(0.02));
    CHECK(spot.lng == Approx(980.0).margin(2.0));
    CHECK(spot.lat == Approx(720.0).margin(2.0));
}

TEST_CASE("a spot says how sure it is of the player", "[minimap]") {
    loadData();
    MiniMap::Watcher watcher;
    cv::Mat black(MiniMap::SIZE, MiniMap::SIZE, CV_8U, cv::Scalar(0));

    // nothing on screen yet, and never seen
    MiniMap::Spot spot = watcher.Look(black, 0.0);
    CHECK(spot.state == MiniMap::State::None);
    CHECK(spot.mapId == 0);
    CHECK(spot.age == 0.0);

    spot = watcher.Look(miniAt(900.0, 700.0), 1.0);
    REQUIRE(spot.found);
    CHECK(spot.state == MiniMap::State::Found);
    CHECK(spot.age == 0.0);
    // standing still is found too
    spot = watcher.Look(miniAt(900.0, 700.0), 1.1);
    CHECK(spot.found);

    // The minimap stays and stops fitting. For a second the player is still
    // taken to be there.
    spot = watcher.Look(miniElsewhere(200.0, 200.0), 1.2);
    CHECK_FALSE(spot.found);
    CHECK(spot.state == MiniMap::State::Near);
    CHECK(spot.age == Approx(0.1));
    CHECK(spot.lng == Approx(900.0).margin(1.5));
    spot = watcher.Look(miniElsewhere(210.0, 200.0), 1.6);
    CHECK(spot.state == MiniMap::State::Near);

    // then the whole map is searched, and it's on none
    spot = watcher.Look(miniElsewhere(220.0, 200.0), 2.5);
    CHECK(spot.state == MiniMap::State::Nowhere);
    CHECK(spot.age == Approx(1.4));
    CHECK(spot.lng == Approx(900.0).margin(1.5));

    // covered by a menu
    spot = watcher.Look(black, 3.0);
    CHECK(spot.state == MiniMap::State::None);
    CHECK(spot.age == Approx(1.9));

    spot = watcher.Look(miniAt(915.0, 690.0), 4.0);
    REQUIRE(spot.found);
    CHECK(spot.state == MiniMap::State::Found);
    CHECK(spot.age == 0.0);
}
