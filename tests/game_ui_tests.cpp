#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/core.hpp>
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
