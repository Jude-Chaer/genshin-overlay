#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgcodecs.hpp>
#include "layer_choice.hpp"
#include "locator/map_locator.hpp"
#include "map_screen.hpp"

namespace fs = std::filesystem;
using Layer = LayerChoice::Layer;
using From = LayerChoice::From;

static std::string env(const char* name) {
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string out = value;
    free(value);
    return out;
}

// The game's layer buttons as the reader sees them on a real screenshot
static GameUi::Switcher buttons(const std::string& name) {
    cv::Mat frame = MapScreen::screen(name);
    return GameUi::ReadSwitcher(frame, GameUi::ReadPanel(frame));
}

// The layer SIFT found each screenshot on, see "[locate-set]" below. The
// numbers are the shots in the set, the crops in data/map_screen are cut
// from them.
// Pardis Dhyai: 31 above ground lit, 35 the bottom floor lit, 36 an info
// panel open and the 4th button lit
static const Layer SHOT_31 = { 2, 0, 0 };       // five_floors_top_lit
static const Layer SHOT_35 = { 2, 19, 109 };    // five_floors_bottom_lit
static const Layer SHOT_36 = { 2, 19, 108 };    // info_panel
// Dragonspine: 27 its floor lit, 30 no layer buttons
static const Layer SHOT_27 = { 2, 26, 46 };     // two_floors
static const Layer SHOT_30 = { 2, 0, 0 };       // no_panel

static void read(LayerChoice& choice, const std::string& name) {
    choice.Read(buttons(name), 2);
}

static LayerChoice opened(const Layer& sift) {
    LayerChoice choice;
    choice.SetFloors(MapScreen::floors());
    choice.Guess(sift);
    return choice;
}

TEST_CASE("SIFT's area, the floor the buttons have lit", "[layer_choice]") {
    // the map opens above ground at Pardis Dhyai
    LayerChoice choice = opened(SHOT_31);
    CHECK(choice.Now() == Layer{ 2, 0, 0 });
    CHECK(choice.Why() == From::Guess);
    read(choice, "five_floors_top_lit");
    CHECK(choice.Now() == Layer{ 2, 0, 0 });
    CHECK(choice.Why() == From::Game);

    // the bottom floor clicked: SIFT still says above ground, so its answer
    // until it looks again
    read(choice, "five_floors_bottom_lit");
    CHECK(choice.Now() == Layer{ 2, 0, 0 });
    CHECK(choice.Why() == From::Guess);
    choice.Guess(SHOT_35);
    read(choice, "five_floors_bottom_lit");
    CHECK(choice.Now() == Layer{ 2, 19, 109 });
    CHECK(choice.Why() == From::Game);

    // an info panel opens, the switcher moves left of it, the 4th button lit
    read(choice, "info_panel");
    CHECK(choice.Now() == Layer{ 2, 19, 108 });
    CHECK(choice.Why() == From::Game);
}

TEST_CASE("the region list over the buttons keeps the floor", "[layer_choice]") {
    LayerChoice choice = opened(SHOT_36);
    read(choice, "info_panel");
    REQUIRE(choice.Now() == Layer{ 2, 19, 108 });
    REQUIRE(buttons("region_list").seen == GameUi::Seen::Unsure);
    read(choice, "region_list");
    CHECK(choice.Now() == Layer{ 2, 19, 108 });
    CHECK(choice.Why() == From::Game);
}

TEST_CASE("no buttons is above ground", "[layer_choice]") {
    LayerChoice choice = opened(SHOT_36);
    read(choice, "info_panel");
    REQUIRE(choice.Now() == Layer{ 2, 19, 108 });

    // over to Dragonspine, SIFT hasn't looked yet, no buttons there
    read(choice, "no_panel");
    CHECK(choice.Now() == Layer{ 2, 0, 0 });
    CHECK(choice.Why() == From::Game);

    choice.Guess(SHOT_30);
    choice.Guess(SHOT_27);
    read(choice, "two_floors");
    CHECK(choice.Now() == Layer{ 2, 26, 46 });
    CHECK(choice.Why() == From::Game);
}

TEST_CASE("a picked floor holds until the game's buttons are clicked", "[layer_choice]") {
    LayerChoice choice = opened(SHOT_35);
    read(choice, "five_floors_bottom_lit");
    choice.Pick({ 2, 19, 41 });
    CHECK(choice.Now() == Layer{ 2, 19, 41 });
    CHECK(choice.Why() == From::Picked);

    // the buttons still say what they said: the pick stays, it may be fixing a wrong read
    read(choice, "five_floors_bottom_lit");
    read(choice, "region_list");
    choice.Guess(SHOT_35);
    CHECK(choice.Now() == Layer{ 2, 19, 41 });
    CHECK(choice.Why() == From::Picked);

    // another button lit: a click in the game, it wins
    read(choice, "info_panel");
    CHECK(choice.Now() == Layer{ 2, 19, 108 });
    CHECK(choice.Why() == From::Game);

    // Auto goes back to the buttons
    choice.Pick({ 2, 19, 41 });
    choice.Auto();
    CHECK(choice.Now() == Layer{ 2, 19, 108 });
    CHECK(choice.Why() == From::Game);

    // closing the map forgets it all
    choice.Pick({ 2, 19, 41 });
    choice.Reset();
    CHECK(choice.Now() == Layer{});
    CHECK(choice.Why() == From::Guess);
}

TEST_CASE("picked before the buttons were read, their first read isn't a click", "[layer_choice]") {
    LayerChoice choice = opened(SHOT_35);
    choice.Pick({ 2, 19, 41 });
    read(choice, "five_floors_bottom_lit");
    CHECK(choice.Now() == Layer{ 2, 19, 41 });
    CHECK(choice.Why() == From::Picked);
    read(choice, "info_panel");
    CHECK(choice.Now() == Layer{ 2, 19, 108 });
}

// Reads recorded in game, in order: what the buttons read and what SIFT
// said while the floors were clicked.
static GameUi::Switcher lit(int count, int button) { return { GameUi::Seen::Read, count, button }; }
static const GameUi::Switcher UNSURE = { GameUi::Seen::Unsure, 0, -1 };

TEST_CASE("the Fortress of Meropide inside Fontaine's underwater floor", "[layer_choice]") {
    // SIFT matched the big underwater floor round the small lit one, all along
    LayerChoice choice;
    choice.SetFloors(MapScreen::floors());
    choice.Guess({ 2, 90, 78 });
    choice.Read(lit(7, 5), 2);
    CHECK(choice.Now() == Layer{ 2, 20, 112 });
    CHECK(choice.Why() == From::Game);
    choice.Guess({ 2, 90, 78 });
    choice.Read(UNSURE, 2);
    CHECK(choice.Now() == Layer{ 2, 20, 112 });
    // HoYoLAB's order isn't id order here
    choice.Read(lit(7, 2), 2);
    CHECK(choice.Now() == Layer{ 2, 20, 42 });
    choice.Read(lit(7, 1), 2);
    CHECK(choice.Now() == Layer{ 2, 20, 122 });
    choice.Read(lit(7, 6), 2);
    CHECK(choice.Now() == Layer{ 2, 20, 113 });
}

TEST_CASE("west of Sumeru city, SIFT catching up with a click", "[layer_choice]") {
    LayerChoice choice;
    choice.SetFloors(MapScreen::floors());
    choice.Guess({ 2, 0, 0 });
    choice.Read(lit(2, 0), 2);
    REQUIRE(choice.Now() == Layer{ 2, 0, 0 });
    // the floor clicked, SIFT hasn't looked yet
    choice.Read(lit(2, 1), 2);
    CHECK(choice.Now() == Layer{ 2, 0, 0 });
    CHECK(choice.Why() == From::Guess);
    // SIFT looked
    choice.Guess({ 2, 21, 43 });
    choice.Read(lit(2, 1), 2);
    CHECK(choice.Now() == Layer{ 2, 21, 43 });
    CHECK(choice.Why() == From::Game);
    // unsure on the same spot, and SIFT taking the floor for the ground: kept
    choice.Read(UNSURE, 2);
    choice.Guess({ 2, 0, 0 });
    choice.Read(lit(2, 1), 2);
    CHECK(choice.Now() == Layer{ 2, 21, 43 });
    CHECK(choice.Why() == From::Game);
}

TEST_CASE("the Electro Regisvine's cave under its panel", "[layer_choice]") {
    // SIFT can't see the cave, the panel covers it: above ground till it can
    LayerChoice choice;
    choice.SetFloors(MapScreen::floors());
    choice.Guess({ 2, 0, 0 });
    choice.Read(lit(2, 1), 2);
    CHECK(choice.Now() == Layer{ 2, 0, 0 });
    CHECK(choice.Why() == From::Guess);
    choice.Read(lit(2, 0), 2);
    CHECK(choice.Now() == Layer{ 2, 0, 0 });
    CHECK(choice.Why() == From::Game);
}

// Not part of the normal run, it gives the layers above. Set
// MAP_SCREEN_SET to the screenshots' folder and MAP_DATA to a map_data folder,
// then run tests.exe "[locate-set]": the locator on each shot, as the follower
// asks it.
TEST_CASE("the locator on every screenshot", "[.][locate-set]") {
    std::string set = env("MAP_SCREEN_SET"), data = env("MAP_DATA");
    REQUIRE(!set.empty());
    REQUIRE(!data.empty());
    static MapLocator::Index* index = new MapLocator::Index;
    REQUIRE(index->Load(fs::path(data) / "manifest.tsv"));
    for (const auto& entry : fs::directory_iterator(fs::path(set) / "shots")) {
        cv::Mat shot = MapScreen::readPicture(entry.path());
        if (shot.empty()) continue;
        MapLocator::Result r = index->Match(shot, shot.cols / 2.0, shot.rows / 2.0, true, false);
        std::cout << entry.path().stem().string() << "  found " << r.found << "  map " << r.mapId << "  group " << r.groupId
            << "  floor " << r.floorId << "  x " << r.lng << "  y " << r.lat << "  inliers " << r.inliers << "\n";
    }
}
