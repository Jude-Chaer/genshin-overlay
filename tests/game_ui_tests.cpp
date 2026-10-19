#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include "game_ui.hpp"

// A map in the follower's wide size: changes slowly along a row, a lot down the rows.
static cv::Mat wideMap() {
    cv::Mat wide(135, 240, CV_8U);
    for (int y = 0; y < wide.rows; y++)
        for (int x = 0; x < wide.cols; x++)
            wide.at<uchar>(y, x) = (uchar)(110 + 60 * std::sin(y / 7.0) + 4 * std::sin(x / 5.0));
    return wide;
}

TEST_CASE("a panel on the right is found where it starts", "[game_ui]") {
    cv::Mat wide = wideMap();
    // an info panel, from 1485 of 2000
    wide.colRange(178, wide.cols).setTo(40);
    REQUIRE(GameUi::FindPanel(wide) == 178);
}

TEST_CASE("the map alone has no panel", "[game_ui]") {
    REQUIRE(GameUi::FindPanel(wideMap()) == -1);
}

TEST_CASE("a flat strip past where panels start isn't one", "[game_ui]") {
    cv::Mat wide = wideMap();
    // dark space right of the layer switcher
    wide.colRange(226, wide.cols).setTo(20);
    REQUIRE(GameUi::FindPanel(wide) == -1);
}

TEST_CASE("an open panel hides most of the game UI", "[game_ui]") {
    RECT game = { 100, 50, 2020, 1130 };
    REQUIRE(GameUi::Boxes(game, -1).size() == 6);

    auto boxes = GameUi::Boxes(game, 1500);
    REQUIRE(boxes.size() == 3);
    // the panel itself, to the game's right edge
    REQUIRE(boxes.back().left == 1500);
    REQUIRE(boxes.back().right == 2020);
    REQUIRE(boxes.back().bottom == 1130);
    // the layer switcher sits just left of it
    REQUIRE(boxes[1].right < 1500);
    REQUIRE(boxes[1].left > 1350);
}

// The layer switcher as the game draws it, grabbed from a game height high:
// count buttons 64 apart around the middle, each a white icon. A lit floor's
// icon is cyan, lit above ground is a cyan ring around the top one. The
// little cyan person by button person, the mouse's thin ring around hover.
static cv::Mat switcher(const RECT& game, int count, int lit, int person, int hover = -1) {
    RECT strip = GameUi::SwitcherStrip(game, -1);
    double s = (game.bottom - game.top) / 1125.0;
    cv::Mat image(strip.bottom - strip.top, strip.right - strip.left, CV_8UC3, cv::Scalar(60, 70, 50));
    auto at = [&](double x, double y) { return cv::Point((int)(x * s), (int)(y * s)); };
    double middle = 45.0, top = 562.5 - 250.0 - (count - 1) * 63.6 / 2.0;
    for (int i = 0; i < count; i++) {
        double y = top + i * 63.6;
        cv::Scalar cyan(255, 220, 90);
        cv::rectangle(image, at(middle - 13, y - 10), at(middle + 13, y + 10), i == lit && i > 0 ? cyan : cv::Scalar(235, 235, 235), cv::FILLED);
        if (i == lit && i == 0) cv::circle(image, at(middle, y), (int)(32 * s), cyan, (int)std::lround(3.3 * s));
        if (i == hover) cv::circle(image, at(middle, y), (int)(28 * s), cyan, (int)std::lround(2.3 * s));
        if (i == person) cv::rectangle(image, at(middle + 18, y - 22), at(middle + 27, y - 13), cv::Scalar(255, 210, 60), cv::FILLED);
    }
    return image;
}

TEST_CASE("the lit layer is read off the switcher", "[game_ui]") {
    RECT game = { 0, 0, 3840, 2160 };
    for (int lit = 0; lit < 5; lit++) {
        GameUi::Switcher read = GameUi::ReadSwitcher(switcher(game, 5, lit, 0), 2160);
        CHECK(read.count == 5);
        CHECK(read.lit == lit);
    }
    GameUi::Switcher two = GameUi::ReadSwitcher(switcher({ 0, 0, 1920, 1080 }, 2, 1, 0), 1080);
    CHECK(two.count == 2);
    CHECK(two.lit == 1);
}

TEST_CASE("the mouse's ring on a button doesn't light it", "[game_ui]") {
    RECT game = { 0, 0, 3840, 2160 };
    GameUi::Switcher floor = GameUi::ReadSwitcher(switcher(game, 7, 5, 0, 1), 2160);
    CHECK(floor.count == 7);
    CHECK(floor.lit == 5);
    GameUi::Switcher above = GameUi::ReadSwitcher(switcher(game, 7, 0, 0, 3), 2160);
    CHECK(above.count == 7);
    CHECK(above.lit == 0);
}

TEST_CASE("no switcher, or one off the middle, isn't read", "[game_ui]") {
    RECT game = { 0, 0, 3840, 2160 };
    REQUIRE(GameUi::ReadSwitcher(switcher(game, 5, -1, 0), 2160).count == 0);
    // a white marker just under the last button
    cv::Mat image = switcher(game, 3, 0, 0);
    double s = 2160 / 1125.0;
    cv::rectangle(image, cv::Point((int)(32 * s), (int)(430 * s)), cv::Point((int)(58 * s), (int)(450 * s)), cv::Scalar(240, 240, 240), cv::FILLED);
    REQUIRE(GameUi::ReadSwitcher(image, 2160).count == 0);
}

TEST_CASE("the switcher's buttons are HoYoLAB's floors in its order", "[game_ui]") {
    // one group listed out of id order, another nearby with fewer floors
    std::vector<MapData::Floor> floors = {
        { 2, 20, 122, 0, 0, 100, 100 }, { 2, 20, 42, 0, 0, 100, 100 }, { 2, 20, 110, 10, 10, 50, 50 },
        { 2, 26, 46, 80, 80, 150, 150 },
    };
    int group = -1, floor = -1;
    REQUIRE(GameUi::SwitcherLayer({ 4, 2 }, floors, 2, 50, 50, -1, group, floor));
    CHECK(group == 20);
    CHECK(floor == 42);
    REQUIRE(GameUi::SwitcherLayer({ 4, 0 }, floors, 2, 50, 50, -1, group, floor));
    CHECK(group == 0);
    CHECK(floor == 0);
    REQUIRE(GameUi::SwitcherLayer({ 2, 1 }, floors, 2, 90, 90, -1, group, floor));
    CHECK(group == 26);
    CHECK(floor == 46);
    // a little outside the box still counts, far away doesn't
    REQUIRE(GameUi::SwitcherLayer({ 2, 1 }, floors, 2, 170, 120, -1, group, floor));
    CHECK(!GameUi::SwitcherLayer({ 2, 1 }, floors, 2, 400, 400, -1, group, floor));
    // no group with that many floors, or another map
    CHECK(!GameUi::SwitcherLayer({ 3, 1 }, floors, 2, 50, 50, -1, group, floor));
    CHECK(!GameUi::SwitcherLayer({ 4, 1 }, floors, 7, 50, 50, -1, group, floor));
}

TEST_CASE("the switcher only has the floors under the middle of the map", "[game_ui]") {
    // Natlan by the Sulfurous Veins: a group with one floor overlaps one with
    // two, of which only the first is under the spot. The game shows one floor.
    std::vector<MapData::Floor> floors = {
        { 2, 134, 174, -9524, 3458, -9138, 3826 },
        { 2, 146, 189, -9556, 3539, -8813, 4049 }, { 2, 146, 190, -9499, 3927, -9301, 4200 },
    };
    int group = -1, floor = -1;
    // the spot is further in the middle of 146's box
    REQUIRE(GameUi::SwitcherLayer({ 2, 1 }, floors, 2, -9215, 3773, -1, group, floor));
    CHECK(group == 146);
    CHECK(floor == 189);
    // the one SIFT found goes first
    REQUIRE(GameUi::SwitcherLayer({ 2, 1 }, floors, 2, -9215, 3773, 134, group, floor));
    CHECK(group == 134);
    CHECK(floor == 174);
    // further north both of 146's floors are under it
    REQUIRE(GameUi::SwitcherLayer({ 3, 2 }, floors, 2, -9400, 4000, -1, group, floor));
    CHECK(group == 146);
    CHECK(floor == 190);
}

TEST_CASE("the switcher can have all of a group's floors", "[game_ui]") {
    // six floors, only two under the spot, the game shows all six
    std::vector<MapData::Floor> floors = {
        { 2, 20, 122, 0, 0, 100, 100 }, { 2, 20, 42, 0, 0, 100, 100 }, { 2, 20, 110, 200, 0, 300, 100 },
        { 2, 20, 111, 200, 0, 300, 100 }, { 2, 20, 112, 200, 0, 300, 100 }, { 2, 20, 113, 200, 0, 300, 100 },
    };
    int group = -1, floor = -1;
    REQUIRE(GameUi::SwitcherLayer({ 7, 5 }, floors, 2, 50, 50, -1, group, floor));
    CHECK(group == 20);
    CHECK(floor == 112);
    REQUIRE(GameUi::SwitcherLayer({ 3, 2 }, floors, 2, 50, 50, -1, group, floor));
    CHECK(floor == 42);
    CHECK(!GameUi::SwitcherLayer({ 5, 1 }, floors, 2, 50, 50, -1, group, floor));
}
