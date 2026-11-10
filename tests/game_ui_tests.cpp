#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "game_ui.hpp"
#include "map_screen.hpp"
#include "map_tracking.hpp"

namespace fs = std::filesystem;
using namespace GameUi;
using MapScreen::floors;
using MapScreen::readPicture;
using MapScreen::screen;

// Screenshots of the big map, cut to the part the readers look at and shrunk
// to 2000x1125. The answers were read off the screenshots by eye.

struct Answer {
    Panel panel;
    Switcher switcher;
};

static Answer read(const std::string& name) {
    cv::Mat frame = screen(name);
    Answer out;
    out.panel = ReadPanel(frame);
    out.switcher = ReadSwitcher(frame, out.panel);
    return out;
}

TEST_CASE("no panel and no layer switcher", "[game_ui]") {
    for (const char* name : { "no_panel", "snow", "pier" }) {
        INFO(name);
        Answer seen = read(name);
        CHECK(seen.panel.seen == GameUi::Seen::None);
        CHECK(seen.panel.left == -1);
        CHECK(seen.switcher.seen == GameUi::Seen::None);
    }
}

TEST_CASE("the layer switcher on its own", "[game_ui]") {
    Answer two = read("two_floors");
    CHECK(two.panel.seen == GameUi::Seen::None);
    CHECK(two.switcher.seen == GameUi::Seen::Read);
    CHECK(two.switcher.count == 2);
    CHECK(two.switcher.lit == 1);

    Answer top = read("five_floors_top_lit");
    CHECK(top.switcher.seen == GameUi::Seen::Read);
    CHECK(top.switcher.count == 5);
    CHECK(top.switcher.lit == 0);

    Answer bottom = read("five_floors_bottom_lit");
    CHECK(bottom.switcher.seen == GameUi::Seen::Read);
    CHECK(bottom.switcher.count == 5);
    CHECK(bottom.switcher.lit == 4);
}

TEST_CASE("an info panel, the layer switcher left of it", "[game_ui]") {
    Answer info = read("info_panel");
    CHECK(info.panel.seen == GameUi::Seen::Read);
    CHECK(info.panel.kind == PanelKind::Info);
    CHECK(info.panel.left == 1488);
    CHECK(info.switcher.seen == GameUi::Seen::Read);
    CHECK(info.switcher.count == 5);
    CHECK(info.switcher.lit == 3);

    Answer top = read("info_panel_top_lit");
    CHECK(top.panel.kind == PanelKind::Info);
    CHECK(top.switcher.count == 2);
    CHECK(top.switcher.lit == 0);

    Answer pin = read("custom_pin");
    CHECK(pin.panel.kind == PanelKind::Info);
    CHECK(pin.panel.left == 1488);
    CHECK(pin.switcher.count == 2);
    CHECK(pin.switcher.lit == 1);

    Answer statue = read("statue_panel");
    CHECK(statue.panel.kind == PanelKind::Info);
    CHECK(statue.switcher.seen == GameUi::Seen::None);
}

TEST_CASE("the region list hides the layer switcher", "[game_ui]") {
    Answer seen = read("region_list");
    CHECK(seen.panel.seen == GameUi::Seen::Read);
    CHECK(seen.panel.kind == PanelKind::RegionList);
    CHECK(seen.panel.left == 1338);
    // covered, so it can't tell
    CHECK(seen.switcher.seen == GameUi::Seen::Unsure);
}

TEST_CASE("the mouse's ring on a floor isn't that floor lit", "[game_ui]") {
    // above ground is lit, the mouse is on the second floor
    Answer seen = read("mouse_on_a_floor");
    CHECK(seen.switcher.seen == GameUi::Seen::Read);
    CHECK(seen.switcher.count == 7);
    CHECK(seen.switcher.lit == 0);
}

TEST_CASE("above ground in the middle of the buttons", "[game_ui]") {
    // 5 floors over it, 1 under, above ground lit: its button is bigger
    Answer seen = read("ground_in_the_middle");
    CHECK(seen.switcher.seen == GameUi::Seen::Read);
    CHECK(seen.switcher.count == 7);
    CHECK(seen.switcher.ground == 5);
    CHECK(seen.switcher.lit == 5);
}

TEST_CASE("a dark map underground isn't the region list", "[game_ui]") {
    // Kresnik Kristall Mines lit: the map round the strip is dark and flat
    Answer seen = read("dark_underground");
    CHECK(seen.panel.seen == GameUi::Seen::None);
    CHECK(seen.switcher.seen == GameUi::Seen::Read);
    CHECK(seen.switcher.count == 2);
    CHECK(seen.switcher.ground == 0);
    CHECK(seen.switcher.lit == 1);
}

TEST_CASE("the same at the game's size", "[game_ui]") {
    cv::Mat frame = screen("info_panel"), big;
    cv::resize(frame, big, cv::Size(3840, 2160), 0, 0, cv::INTER_CUBIC);
    Panel panel = ReadPanel(big);
    CHECK(panel.kind == PanelKind::Info);
    CHECK(panel.left == 2857);
    Switcher switcher = ReadSwitcher(big, panel);
    CHECK(switcher.count == 5);
    CHECK(switcher.lit == 3);
}

static const char* const SHOTS[] = { "custom_pin", "five_floors_bottom_lit", "five_floors_top_lit", "info_panel",
    "info_panel_top_lit", "mouse_on_a_floor", "no_panel", "pier", "region_list", "snow", "statue_panel", "two_floors",
    "ground_in_the_middle", "dark_underground" };

// only the parts the follower copies out of the game's frame
static Picture parts(const cv::Mat& frame) {
    Picture picture;
    picture.size = frame.size();
    std::vector<cv::Rect> rects = PanelParts(frame.size());
    for (const cv::Rect& rect : SwitcherParts(frame.size())) rects.push_back(rect);
    for (const cv::Rect& rect : rects) {
        picture.parts.push_back(frame(rect).clone());
        picture.at.push_back(rect.tl());
    }
    return picture;
}

TEST_CASE("the follower's parts of a frame read the same as all of it", "[game_ui]") {
    for (const char* name : SHOTS) {
        cv::Mat frame = screen(name), big;
        cv::resize(frame, big, cv::Size(3840, 2160), 0, 0, cv::INTER_CUBIC);
        for (const cv::Mat* f : { &frame, &big }) {
            INFO(name << " at " << f->cols);
            Panel whole = ReadPanel(*f);
            Picture some = parts(*f);
            Panel panel = ReadPanel(some);
            CHECK(panel.seen == whole.seen);
            CHECK(panel.kind == whole.kind);
            CHECK(panel.left == whole.left);
            Switcher all = ReadSwitcher(*f, whole);
            Switcher switcher = ReadSwitcher(some, panel);
            CHECK(switcher.seen == all.seen);
            CHECK(switcher.count == all.count);
            CHECK(switcher.lit == all.lit);
            CHECK(switcher.ground == all.ground);
        }
    }
}

TEST_CASE("the game's UI boxes follow the panel", "[game_ui]") {
    // the game at 4K, not at the screen's corner
    const RECT game = { 100, 50, 100 + 3840, 50 + 2160 };
    auto at4k = [](const char* name) {
        cv::Mat big;
        cv::resize(screen(name), big, cv::Size(3840, 2160), 0, 0, cv::INTER_CUBIC);
        return ReadPanel(big);
    };

    std::vector<RECT> none = Boxes(game, at4k("no_panel"));
    CHECK(none.size() == 6);
    // the region name, bottom right: 1530-2000 x 1015-1125, times 1.92
    CHECK(none[4].left == 100 + 3840 - 902);
    CHECK(none[4].top == 50 + 1949);
    CHECK(none[4].right == game.right);
    CHECK(none[4].bottom == game.bottom);
    // the switcher, 50 each side of its middle 80.5 from the right edge:
    // 130.5 and 30.5 from it, times 1.92
    CHECK(none[5].left == game.right - 251);
    CHECK(none[5].right == game.right - 59);

    Panel info = at4k("info_panel");
    REQUIRE(info.kind == PanelKind::Info);
    std::vector<RECT> withInfo = Boxes(game, info);
    REQUIRE(withInfo.size() == 3);
    // the switcher left of the panel, its middle 63.5 from it: its box ends
    // 13.5 from it, times 1.92. Then the panel itself.
    CHECK(withInfo[1].right == game.left + info.left - 26);
    CHECK(withInfo[2].left == game.left + info.left);
    CHECK(withInfo[2].right == game.right);

    // the region list hides the switcher, so it has no box
    Panel list = at4k("region_list");
    REQUIRE(list.kind == PanelKind::RegionList);
    std::vector<RECT> withList = Boxes(game, list);
    REQUIRE(withList.size() == 2);
    CHECK(withList[1].left == game.left + list.left);

    // not sure there's a panel: all the boxes
    CHECK(Boxes(game, { Seen::Unsure, PanelKind::None, -1 }).size() == 6);
}

TEST_CASE("the floor picker's spot left of the layer switcher", "[game_ui]") {
    const RECT game = { 100, 50, 100 + 3840, 50 + 2160 };
    auto at4k = [](const char* name) {
        cv::Mat big;
        cv::resize(screen(name), big, cv::Size(3840, 2160), 0, 0, cv::INTER_CUBIC);
        return ReadPanel(big);
    };
    // the switcher's box starts 130.5 from the right, the picker 10 left of
    // that, half way down at 564: times 1.92
    POINT at;
    REQUIRE(BesideSwitcher(game, at4k("no_panel"), at));
    CHECK(at.x == game.right - 251 - 19);
    CHECK(at.y == game.top + 1083);

    // left of the panel, where the switcher moves: its box starts 113.5
    // from it
    Panel info = at4k("info_panel");
    REQUIRE(info.kind == PanelKind::Info);
    REQUIRE(BesideSwitcher(game, info, at));
    CHECK(at.x == game.left + info.left - 218 - 19);
    CHECK(at.y == game.top + 1083);

    // the region list hides the switcher, and not sure there's a panel
    CHECK_FALSE(BesideSwitcher(game, at4k("region_list"), at));
    CHECK_FALSE(BesideSwitcher(game, { Seen::Unsure, PanelKind::None, -1 }, at));
}

TEST_CASE("the lit button is HoYoLAB's floor in its order", "[game_ui]") {
    int group = -1, floor = -1;
    // Pardis Dhyai, the game showed 5 buttons
    auto lit = [&](int button) {
        group = floor = -1;
        return SwitcherFloor({ GameUi::Seen::Read, 5, button, 0 }, floors(), 2, 19, 0, 0, group, floor);
    };
    REQUIRE(lit(0));
    CHECK((group == 0 && floor == 0));
    REQUIRE(lit(1));
    CHECK((group == 19 && floor == 41));
    REQUIRE(lit(2));
    CHECK((group == 19 && floor == 107));
    REQUIRE(lit(4));
    CHECK((group == 19 && floor == 109));

    // Dragonspine, 2 buttons
    REQUIRE(SwitcherFloor({ GameUi::Seen::Read, 2, 1, 0 }, floors(), 2, 26, 0, 0, group, floor));
    CHECK((group == 26 && floor == 46));

    // The Fortress of Meropide, 7 buttons, SIFT on Fontaine's underwater
    // floor round it. HoYoLAB lists its floors out of id order.
    REQUIRE(SwitcherFloor({ GameUi::Seen::Read, 7, 1, 0 }, floors(), 2, 90, 0, 0, group, floor));
    CHECK((group == 20 && floor == 122));
    REQUIRE(SwitcherFloor({ GameUi::Seen::Read, 7, 2, 0 }, floors(), 2, 90, 0, 0, group, floor));
    CHECK((group == 20 && floor == 42));
}

TEST_CASE("floors over above ground", "[game_ui]") {
    // Kuuvahki Experimental Design Bureau: 5 floors over above ground, 1 under
    int group = -1, floor = -1;
    REQUIRE(SwitcherFloor({ GameUi::Seen::Read, 7, 0, 5 }, floors(), 2, 185, 0, 0, group, floor));
    CHECK((group == 185 && floor == 252));
    REQUIRE(SwitcherFloor({ GameUi::Seen::Read, 7, 4, 5 }, floors(), 2, 185, 0, 0, group, floor));
    CHECK((group == 185 && floor == 257));
    REQUIRE(SwitcherFloor({ GameUi::Seen::Read, 7, 5, 5 }, floors(), 2, 185, 0, 0, group, floor));
    CHECK((group == 0 && floor == 0));
    // SIFT taking a lit floor for the ground: the group under the map's middle
    REQUIRE(SwitcherFloor({ GameUi::Seen::Read, 7, 2, 5 }, floors(), 2, 0, -10700.6, -973.7, group, floor));
    CHECK((group == 185 && floor == 255));
    // the one under isn't in HoYoLAB
    CHECK_FALSE(SwitcherFloor({ GameUi::Seen::Read, 7, 6, 5 }, floors(), 2, 0, -10700.6, -973.7, group, floor));
    // the one under isn't one of the 5
    CHECK_FALSE(SwitcherFloor({ GameUi::Seen::Read, 7, 6, 5 }, floors(), 2, 185, 0, 0, group, floor));
}

TEST_CASE("no floor when the buttons don't fit SIFT's area", "[game_ui]") {
    int group = -1, floor = -1;
    CHECK_FALSE(SwitcherFloor({ GameUi::Seen::Read, 3, 1, 0 }, floors(), 2, 19, 0, 0, group, floor));
    // above ground, SIFT has no area to go by
    CHECK_FALSE(SwitcherFloor({ GameUi::Seen::Read, 2, 1, 0 }, floors(), 2, 0, 0, 0, group, floor));
    CHECK_FALSE(SwitcherFloor({ GameUi::Seen::Unsure, 5, -1 }, floors(), 2, 19, 0, 0, group, floor));
}

// All the screenshots kept for the readers, full size. Not part of the normal
// run: set MAP_SCREEN_SET to the folder with answers.tsv and shots in it, then
// run tests.exe "[score]".
TEST_CASE("score on every screenshot", "[.][score]") {
    char* set = nullptr;
    size_t size = 0;
    REQUIRE(_dupenv_s(&set, &size, "MAP_SCREEN_SET") == 0);
    REQUIRE(set);
    std::string folder = set;
    free(set);
    std::ifstream answers(fs::path(folder) / "answers.tsv");
    REQUIRE(answers);
    int shots = 0, misses = 0;
    double took = 0.0;
    std::string line;
    while (std::getline(answers, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("shot", 0) == 0) continue;
        std::vector<std::string> f;
        std::stringstream fields(line);
        for (std::string field; std::getline(fields, field, '\t');) f.push_back(field);
        if (f.size() < 5) continue;
        cv::Mat frame = readPicture(fs::path(folder) / "shots" / (f[0] + ".png"));
        if (frame.empty()) continue;
        shots++;
        auto start = std::chrono::steady_clock::now();
        Panel panel = ReadPanel(frame);
        Switcher switcher = ReadSwitcher(frame, panel);
        took += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        double s = frame.rows / 1125.0;
        int left = panel.left < 0 ? -1 : (int)std::lround(2000 - (frame.cols - panel.left) / s);

        std::string kind = panel.seen == GameUi::Seen::Unsure ? "unsure"
            : panel.kind == PanelKind::Info ? "info" : panel.kind == PanelKind::RegionList ? "region list" : "none";
        int wantLeft = f[2] == "-" ? -1 : std::stoi(f[2]);
        int wantCount = std::stoi(f[3]);
        int wantLit = f[4] == "-" ? -1 : std::stoi(f[4]);
        int count = switcher.seen == GameUi::Seen::Read ? switcher.count : 0;
        bool good = kind == f[1] && left == wantLeft && count == wantCount && (wantCount == 0 || switcher.lit == wantLit);
        if (!good) misses++;
        const char* buttons = switcher.seen == GameUi::Seen::Read ? "" : switcher.seen == GameUi::Seen::Unsure ? " (unsure)" : " (none)";
        std::cout << f[0] << "  panel " << kind << " " << left << "  buttons " << count << buttons << " lit " << switcher.lit << " ground " << switcher.ground
            << (good ? "" : "   MISS") << "\n";
    }
    std::cout << shots << " shots, " << misses << " missed, " << (shots ? took / shots : 0.0) << " ms a shot to read both\n";
    CHECK(shots > 0);
    CHECK(misses == 0);
}

// The part getZoomBarRect gives, cut from the game's frames
static bool zoomBar(const std::string& name) {
    cv::Mat zoomTemplate, grey;
    cv::cvtColor(readPicture(fs::path(__FILE__).parent_path().parent_path() / "zoombar_template.png"), zoomTemplate, cv::COLOR_BGR2GRAY);
    cv::cvtColor(readPicture(MapScreen::DATA / "zoom_bar" / (name + ".png")), grey, cv::COLOR_BGR2GRAY);
    return MapTracking::zoomBarShows(grey, zoomTemplate);
}

TEST_CASE("the zoom bar at the game's height", "[game_ui]") {
    // the map open at 3840x2160, in a 1920x1080 window and a 1600x1200 one
    CHECK(zoomBar("open_2160"));
    CHECK(zoomBar("open_1080"));
    CHECK(zoomBar("open_4_3"));
    // the map closed in the 1920x1080 window
    CHECK_FALSE(zoomBar("closed_1080"));
}

// A 1600x1200 window: its right side, 536 wide, back in a black frame
static cv::Mat screen43(const std::string& name) {
    cv::Mat part = readPicture(MapScreen::DATA / "map_screen" / (name + ".png"));
    REQUIRE_FALSE(part.empty());
    cv::Mat frame(1200, 1600, CV_8UC3, cv::Scalar::all(0));
    part.copyTo(frame(cv::Rect(1600 - part.cols, 0, part.cols, part.rows)));
    return frame;
}

TEST_CASE("the layer switcher in a 4:3 window", "[game_ui]") {
    cv::Mat frame = screen43("two_floors_4_3");
    Panel panel = ReadPanel(frame);
    CHECK(panel.seen == GameUi::Seen::None);
    Switcher switcher = ReadSwitcher(frame, panel);
    CHECK(switcher.seen == GameUi::Seen::Read);
    CHECK(switcher.count == 2);
    CHECK(switcher.lit == 1);
}

TEST_CASE("the game's UI boxes in a 4:3 window", "[game_ui]") {
    // the UI fits the width (0.8), pieces in the middle of a side stay there
    RECT game = { 0, 0, 1600, 1200 };
    std::vector<RECT> boxes = Boxes(game, Panel{});
    REQUIRE(boxes.size() == 6);
    // event list from the top to above the zoom bar
    CHECK(boxes[0].top == 0);
    CHECK(boxes[0].bottom == 502);
    // zoom bar around the window's middle
    CHECK(boxes[1].top == 494);
    CHECK(boxes[1].bottom == 702);
    // gear and region name at the bottom
    CHECK(boxes[2].bottom == 1192);
    CHECK(boxes[4].bottom == 1200);
    CHECK(boxes[4].left == 1224);
    // resource bar at the top right
    CHECK(boxes[3].top == 12);
    CHECK(boxes[3].right == 1600);
}
