#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "map_follow.hpp"
#include "test_map.hpp"

using Catch::Approx;
using TestMap::GAME;

static const cv::Mat& map() {
    static cv::Mat map = TestMap::make(2048, 1536);
    return map;
}

TEST_CASE("the five patches are squares inside the game") {
    RECT game = { 100, 50, 100 + 1920, 50 + 1080 };
    std::vector<RECT> rects = MapFollow::patchRects(game);
    REQUIRE(rects.size() == 5);
    for (const RECT& rect : rects) {
        CHECK(rect.right - rect.left == 324);
        CHECK(rect.bottom - rect.top == 324);
        CHECK(rect.left >= game.left);
        CHECK(rect.top >= game.top);
        CHECK(rect.right <= game.right);
        CHECK(rect.bottom <= game.bottom);
    }
    // the last one is in the middle
    CHECK((rects[4].left + rects[4].right) / 2 == (game.left + game.right) / 2);
    CHECK((rects[4].top + rects[4].bottom) / 2 == (game.top + game.bottom) / 2);
}

TEST_CASE("compare sees how far the map slid") {
    // looking 20 further left and 12 further down, so the map went right and up on screen
    MapFollow::Patches before = MapFollow::cut(TestMap::screen(map(), 1000, 700), GAME);
    MapFollow::Patches after = MapFollow::cut(TestMap::screen(map(), 980, 712), GAME);
    MapFollow::Motion motion = MapFollow::compare(before, after, GAME);
    REQUIRE(motion.found);
    CHECK(motion.moved.x == Approx(20.0).margin(1.0));
    CHECK(motion.moved.y == Approx(-12.0).margin(1.0));
    CHECK(motion.zoom == Approx(1.0).margin(0.005));
}

TEST_CASE("compare sees a zoom") {
    MapFollow::Patches before = MapFollow::cut(TestMap::screen(map(), 1000, 700, 1.0), GAME);
    MapFollow::Patches after = MapFollow::cut(TestMap::screen(map(), 1000, 700, 1.0 / 1.03), GAME);
    MapFollow::Motion motion = MapFollow::compare(before, after, GAME);
    REQUIRE(motion.found);
    CHECK(motion.zoom == Approx(1.03).margin(0.005));
    CHECK(motion.moved.x == Approx(0.0).margin(1.0));
    CHECK(motion.moved.y == Approx(0.0).margin(1.0));
}

TEST_CASE("a map that didn't move didn't move") {
    MapFollow::Patches patches = MapFollow::cut(TestMap::screen(map(), 1000, 700), GAME);
    MapFollow::Motion motion = MapFollow::compare(patches, patches, GAME);
    REQUIRE(motion.found);
    CHECK(motion.moved.x == Approx(0.0).margin(0.1));
    CHECK(motion.moved.y == Approx(0.0).margin(0.1));
}

TEST_CASE("flat patches are left out and nothing is found on them") {
    cv::Mat sea(720, 1280, CV_8UC3, cv::Scalar(120, 90, 60));
    MapFollow::Patches patches = MapFollow::cut(sea, GAME);
    REQUIRE(patches.size() == 5);
    for (const cv::Mat& patch : patches) CHECK(patch.empty());
    CHECK_FALSE(MapFollow::compare(patches, patches, GAME).found);
}

TEST_CASE("moveView follows what compare saw") {
    MapLocator::Result view;
    view.lng = 1000;
    view.lat = 700;
    view.unitsPerPixel = 2.0;

    MapFollow::Motion slide;
    slide.found = true;
    slide.moved = { 20, -12 };
    MapLocator::Result moved = MapFollow::moveView(view, slide);
    CHECK(moved.lng == Approx(960.0));
    CHECK(moved.lat == Approx(724.0));
    CHECK(moved.unitsPerPixel == Approx(2.0));

    MapFollow::Motion zoom;
    zoom.found = true;
    zoom.zoom = 2.0;
    moved = MapFollow::moveView(view, zoom);
    CHECK(moved.lng == Approx(1000.0));
    CHECK(moved.lat == Approx(700.0));
    CHECK(moved.unitsPerPixel == Approx(1.0));
}

TEST_CASE("compare and moveView together land on the new view") {
    MapFollow::Patches before = MapFollow::cut(TestMap::screen(map(), 1000, 700), GAME);
    MapFollow::Patches after = MapFollow::cut(TestMap::screen(map(), 1015, 690), GAME);
    MapLocator::Result view;
    view.lng = 1000;
    view.lat = 700;
    view.unitsPerPixel = 1.0;
    MapLocator::Result moved = MapFollow::moveView(view, MapFollow::compare(before, after, GAME));
    CHECK(moved.lng == Approx(1015.0).margin(1.0));
    CHECK(moved.lat == Approx(690.0).margin(1.0));
}
