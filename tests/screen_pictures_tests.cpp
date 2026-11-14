#include <string>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgproc.hpp>
#include "extensions.hpp"
#include "map_screen.hpp"
#include "screen_pictures.hpp"
#include "screens.hpp"
#include "test_extension.hpp"
#include "triggers.hpp"

using Permission = Extensions::ExtensionPermissionTypes;
using Screens::Screen;

namespace {
    // A game that hands over a new frame when told, 1920x1080, and textures
    // counted instead of made.
    struct FakeGame {
        uint64_t frame = 1;
        int grabs = 0;
        GLuint nextTexture = 100;
        int released = 0;
        double now = 0.0;
        // the game frames the map was seen up on
        uint64_t seenFirst = 1, seenLast = UINT64_MAX;

        decltype(ScreenPictures::grab) grab = ScreenPictures::grab;
        decltype(ScreenPictures::upload) upload = ScreenPictures::upload;
        decltype(ScreenPictures::release) release = ScreenPictures::release;
        decltype(ScreenPictures::seconds) seconds = ScreenPictures::seconds;
        decltype(ScreenPictures::shownOn) shownOn = ScreenPictures::shownOn;

        FakeGame() {
            ScreenPictures::clear();
            ScreenPictures::grab = [this](int width, cv::Mat& bgra, uint64_t& seen) {
                if (seen == frame) return false;
                grabs++;
                seen = frame;
                bgra = cv::Mat(width * 1080 / 1920, width, CV_8UC4, cv::Scalar(255, 255, 255, 255));
                return true;
            };
            ScreenPictures::upload = [this](const cv::Mat&, GLuint texture) { return texture != 0 ? texture : nextTexture++; };
            ScreenPictures::release = [this](GLuint) { released++; };
            ScreenPictures::seconds = [this] { return now; };
            ScreenPictures::shownOn = [this](Screen screen, uint64_t seen) {
                return screen == Screen::Map && seenFirst != 0 && seenFirst <= seen && seen <= seenLast;
            };
        }
        ~FakeGame() {
            ScreenPictures::clear();
            ScreenPictures::grab = grab;
            ScreenPictures::upload = upload;
            ScreenPictures::release = release;
            ScreenPictures::seconds = seconds;
            ScreenPictures::shownOn = shownOn;
        }
    };

    Screens::Up up(bool map, bool menu) {
        Screens::Up screens;
        screens.set(Screen::Map, map);
        screens.set(Screen::Menu, menu);
        return screens;
    }

    // the UID as measured on the game's screen, in 2000x1125 units: x0 y0 x1 y1
    struct Box {
        double x0, y0, x1, y1;
    };
    // on the map and outside it, from a 3840x2160 recording
    const Box UID = { 1753, 1100, 1937, 1116 };

    cv::Rect inPixels(const Box& box, cv::Size size) {
        double s = size.height / 1125.0;
        auto x = [&](double units) { return (int)(size.width - (2000.0 - units) * s); };
        return { x(box.x0), (int)(box.y0 * s), x(box.x1) - x(box.x0), (int)((box.y1 - box.y0) * s) };
    }
}

TEST_CASE("the UID is black in a map picture at 1080p and 4K, the rest untouched", "[screen pictures]") {
    cv::Mat map = MapScreen::screen("no_panel");
    for (cv::Size size : { cv::Size(1920, 1080), cv::Size(3840, 2160), cv::Size(1280, 720) }) {
        cv::Mat before;
        cv::resize(map, before, size);
        cv::cvtColor(before, before, cv::COLOR_BGR2BGRA);
        // white where the UID was measured, so a missed pixel shows
        before(inPixels(UID, size)).setTo(cv::Scalar::all(255));
        cv::Mat after = before.clone();
        Screens::blackOut(after, Screen::Map);

        std::vector<cv::Mat> channels;
        cv::split(after(inPixels(UID, size)), channels);
        CHECK(cv::countNonZero(channels[0] | channels[1] | channels[2]) == 0);
        // opaque black, not see-through
        CHECK(cv::countNonZero(channels[3] != 255) == 0);
        // the map and the game's UI above the UID, the exploration progress too, are as they were
        cv::Rect above(0, 0, size.width, (int)(1094 * size.height / 1125.0));
        CHECK(cv::norm(before(above), after(above), cv::NORM_INF) == 0);
        cv::Rect left(0, 0, (int)(size.width - 300 * size.height / 1125.0), size.height);
        CHECK(cv::norm(before(left), after(left), cv::NORM_INF) == 0);
    }
}

TEST_CASE("the UID is black in a 4:3 window, where the game fits its UI to the width", "[screen pictures]") {
    // measured in a 1440x1080 window, in its pixels
    const cv::Rect uid(1269, 1063, 124, 9);
    cv::Mat picture(1080, 1440, CV_8UC4, cv::Scalar::all(255));
    Screens::blackOut(picture, Screen::Map);
    std::vector<cv::Mat> channels;
    cv::split(picture(uid), channels);
    CHECK(cv::countNonZero(channels[0] | channels[1] | channels[2]) == 0);
    // the exploration progress above it stays
    CHECK(picture.at<cv::Vec4b>(1050, 1300) == cv::Vec4b(255, 255, 255, 255));
}

TEST_CASE("every screen with a picture hides the UID", "[screen pictures]") {
    for (const Screens::Known& known : Screens::list) {
        if (!known.picture) continue;
        cv::Mat picture(1080, 1920, CV_8UC4, cv::Scalar::all(255));
        Screens::blackOut(picture, known.screen);
        CHECK(picture.at<cv::Vec4b>(inPixels(UID, picture.size()).tl()) == cv::Vec4b(0, 0, 0, 255));
    }
}

TEST_CASE("one grab per turn and game frame, however many ask", "[screen pictures]") {
    FakeGame game;
    ScreenPictures::Picture first = ScreenPictures::get(Screen::Map, 1280, up(true, false));
    REQUIRE(first.texture != 0);
    CHECK(first.width == 1280);
    CHECK(first.height == 720);
    for (int i = 0; i < 100; i++) CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).texture == first.texture);
    CHECK(game.grabs == 1);

    // a new turn with no new game frame copies nothing
    ScreenPictures::nextTurn();
    ScreenPictures::get(Screen::Map, 1280, up(true, false));
    CHECK(game.grabs == 1);

    // a new frame in the same turn waits for the next turn
    game.frame++;
    ScreenPictures::get(Screen::Map, 1280, up(true, false));
    CHECK(game.grabs == 1);
    ScreenPictures::nextTurn();
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).texture == first.texture);
    CHECK(game.grabs == 2);

    // another width is its own picture
    ScreenPictures::Picture big = ScreenPictures::get(Screen::Map, 1920, up(true, false));
    CHECK(big.texture != first.texture);
    CHECK(big.width == 1920);
    CHECK(game.grabs == 3);
}

TEST_CASE("nothing for a screen that isn't up", "[screen pictures]") {
    FakeGame game;
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(false, true)).texture == 0);
    CHECK(ScreenPictures::get(Screen::Menu, 1280, up(true, false)).texture == 0);
    CHECK(game.grabs == 0);
    CHECK(ScreenPictures::count() == 0);
}

TEST_CASE("pictures nobody asked for in a second are freed, at most 8 are kept", "[screen pictures]") {
    FakeGame game;
    for (int width = 100; width < 100 + (int)ScreenPictures::MaxPictures; width++) {
        CHECK(ScreenPictures::get(Screen::Map, width, up(true, false)).texture != 0);
    }
    ScreenPictures::Refused refused = ScreenPictures::Refused::None;
    CHECK(ScreenPictures::get(Screen::Map, 999, up(true, false), nullptr, &refused).texture == 0);
    CHECK(refused == ScreenPictures::Refused::TooMany);
    // one already kept is still given
    CHECK(ScreenPictures::get(Screen::Map, 100, up(true, false), nullptr, &refused).texture != 0);
    CHECK(refused == ScreenPictures::Refused::None);

    game.now = 0.9;
    ScreenPictures::get(Screen::Map, 100, up(true, false));
    game.now = 1.5;
    ScreenPictures::nextTurn();
    CHECK(ScreenPictures::count() == 1);
    CHECK(game.released == (int)ScreenPictures::MaxPictures - 1);
    CHECK(ScreenPictures::owns(100));
    CHECK_FALSE(ScreenPictures::owns(101));
}

TEST_CASE("a frame is given only once the map was seen up on it", "[screen pictures]") {
    FakeGame game;
    // the map was seen up on frames 5 to 9, frame 10 isn't checked yet
    game.seenFirst = 5;
    game.seenLast = 9;
    game.frame = 10;
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).texture == 0);
    // newer frames don't take its place while it waits
    game.frame = 11;
    ScreenPictures::nextTurn();
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).texture == 0);
    CHECK(game.grabs == 1);

    game.seenLast = 10;
    ScreenPictures::nextTurn();
    ScreenPictures::Picture picture = ScreenPictures::get(Screen::Map, 1280, up(true, false));
    CHECK(picture.texture != 0);
    CHECK(picture.frame == 10);
    // the next turn takes the newest frame
    ScreenPictures::nextTurn();
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).frame == 10);
    CHECK(game.grabs == 2);
}

TEST_CASE("a frame from before the map opened is never given", "[screen pictures]") {
    FakeGame game;
    // grabbed on frame 3, the map was first seen up on frame 5
    game.seenFirst = 5;
    game.frame = 3;
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).texture == 0);
    // half a second later it's dropped and a new frame is taken
    game.frame = 6;
    for (double t : { 0.1, 0.3, 0.49 }) {
        game.now = t;
        ScreenPictures::nextTurn();
        CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).texture == 0);
    }
    game.now = 0.6;
    ScreenPictures::nextTurn();
    ScreenPictures::Picture picture = ScreenPictures::get(Screen::Map, 1280, up(true, false));
    CHECK(picture.texture != 0);
    CHECK(picture.frame == 6);
}

TEST_CASE("a picture from an earlier opening isn't given once the map opens again", "[screen pictures]") {
    FakeGame game;
    game.frame = 4;
    REQUIRE(ScreenPictures::get(Screen::Map, 1280, up(true, false)).frame == 4);
    // closed, opened again from frame 8, its newest frame 9 not checked yet
    game.seenFirst = 8;
    game.seenLast = 8;
    game.frame = 9;
    ScreenPictures::nextTurn();
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).texture == 0);
    game.seenLast = 9;
    ScreenPictures::nextTurn();
    CHECK(ScreenPictures::get(Screen::Map, 1280, up(true, false)).frame == 9);
}

TEST_CASE("one extension gets at most 2 pictures, the others still get theirs", "[screen pictures]") {
    FakeGame game;
    int first = 0, second = 0;
    ScreenPictures::Refused refused = ScreenPictures::Refused::None;
    CHECK(ScreenPictures::get(Screen::Map, 100, up(true, false), &first).texture != 0);
    CHECK(ScreenPictures::get(Screen::Map, 200, up(true, false), &first).texture != 0);
    CHECK(ScreenPictures::get(Screen::Map, 300, up(true, false), &first, &refused).texture == 0);
    CHECK(refused == ScreenPictures::Refused::TooManyOwn);
    // its own two still come
    CHECK(ScreenPictures::get(Screen::Map, 100, up(true, false), &first, &refused).texture != 0);
    CHECK(refused == ScreenPictures::Refused::None);
    // another one gets a new width, shares one the first has, and is held to 2 too
    CHECK(ScreenPictures::get(Screen::Map, 300, up(true, false), &second).texture != 0);
    CHECK(ScreenPictures::get(Screen::Map, 100, up(true, false), &second).texture != 0);
    CHECK(ScreenPictures::get(Screen::Map, 200, up(true, false), &second, &refused).texture == 0);
    CHECK(refused == ScreenPictures::Refused::TooManyOwn);
    CHECK(ScreenPictures::count() == 3);

    // a second after its last ask a width no longer counts for it
    game.now = 1.5;
    ScreenPictures::get(Screen::Map, 300, up(true, false), &second);
    ScreenPictures::nextTurn();
    CHECK(ScreenPictures::get(Screen::Map, 400, up(true, false), &first).texture != 0);
}

TEST_CASE("GetScreenPicture needs Display Read and knows only the known screens", "[screen pictures]") {
    FakeGame game;
    Extensions::Extension* ext = TestExtension::load("screen_picture_call", "");
    REQUIRE(ext != nullptr);
    Extensions::switchPermission(*ext, Permission::DisplayRead, false);
    REQUIRE(TestExtension::run(ext, "none = GetScreenPicture('map') == nil"));
    CHECK((*ext->luaState)["none"].get_or(false));
    CHECK(ext->refusalsLogged.count("GetScreenPicture") == 1);

    Extensions::switchPermission(*ext, Permission::DisplayRead, true);
    REQUIRE(TestExtension::run(ext, "none = GetScreenPicture('inventory') == nil"));
    CHECK((*ext->luaState)["none"].get_or(false));
    // the overlay's menu isn't in the picture, only what's behind it
    REQUIRE(TestExtension::run(ext, "none = GetScreenPicture('menu') == nil"));
    CHECK((*ext->luaState)["none"].get_or(false));
    CHECK(ScreenPictures::get(Screen::Menu, 1280, up(false, true)).texture == 0);
    REQUIRE(TestExtension::run(ext, "id = AddTrigger('ScreenPicture', function() end, 'menu')"));
    CHECK((*ext->luaState)["id"].get_or(-1) == 0);
    CHECK(game.grabs == 0);
    TestExtension::unloadAll();
}

TEST_CASE("ScreenPicture runs on each new picture while its screen is up", "[screen pictures][triggers]") {
    FakeGame game;
    Extensions::Extension* ext = TestExtension::load("screen_picture_trigger", R"(
        _G.runs = 0
        function Init()
            AddTrigger("ScreenPicture", function(picture, screen)
                _G.runs = _G.runs + 1
                _G.picture = picture
                _G.screen = screen
            end, "map")
        end
    )");
    REQUIRE(ext != nullptr);
    Extensions::runInit(*ext);
    Extensions::switchPermission(*ext, Permission::DisplayRead, true);
    auto runs = [&] { return (*ext->luaState)["runs"].get<int>(); };
    // the map up or not, and the game frame its picture shows
    auto state = [](bool map, uint64_t frame) {
        Triggers::State now;
        now.screens.set(Screen::Map, map);
        now.pictures[(size_t)Screen::Map] = frame;
        return now;
    };

    // the map open when it was set is the start, nothing runs
    Triggers::check(state(true, 5));
    Triggers::check(state(true, 5));
    CHECK(runs() == 0);

    Triggers::check(state(true, 6));
    CHECK(runs() == 1);
    CHECK((*ext->luaState)["screen"].get<std::string>() == "map");
    {
        // let go of before its Lua state closes
        sol::table picture = (*ext->luaState)["picture"];
        CHECK(picture["width"].get<int>() == ScreenPictures::DefaultWidth);
        CHECK(picture["texture"].get<GLuint>() != 0);
    }

    // the map closing isn't a picture
    Triggers::check(state(false, 7));
    CHECK(runs() == 1);
    Triggers::check(state(true, 8));
    CHECK(runs() == 2);

    // without Display Read it doesn't run
    Extensions::switchPermission(*ext, Permission::DisplayRead, false);
    Triggers::check(state(true, 9));
    Triggers::check(state(true, 10));
    CHECK(runs() == 2);
    TestExtension::unloadAll();
}
