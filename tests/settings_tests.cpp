#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "settings.hpp"

static void noFile() {
    std::filesystem::remove(Settings::file);
}

static void defaults() {
    Settings::hideOnMapClose = false;
    Settings::lineUpWithMap = true;
    Settings::ignoreSea = true;
    Settings::steadyZoom = true;
    Settings::guessAhead = true;
    Settings::lookAgainStill = true;
    Settings::followOnThread = true;
    Settings::shrinkOnGpu = true;
    Settings::findSmallFirst = true;
}

TEST_CASE("settings come back after Save and Load") {
    noFile();
    defaults();
    Settings::hideOnMapClose = true;
    Settings::ignoreSea = false;
    Settings::findSmallFirst = false;
    Settings::Save();

    defaults();
    Settings::Load();
    CHECK(Settings::hideOnMapClose == true);
    CHECK(Settings::ignoreSea == false);
    CHECK(Settings::findSmallFirst == false);
    // the ones that weren't changed
    CHECK(Settings::lineUpWithMap == true);
    CHECK(Settings::guessAhead == true);
    defaults();
}

TEST_CASE("a setting that isn't in the file gets its default") {
    std::ofstream(Settings::file, std::ios::trunc) << R"({ "ignoreSea": false })";
    defaults();
    Settings::guessAhead = false;
    Settings::Load();
    CHECK(Settings::ignoreSea == false);
    CHECK(Settings::guessAhead == true);
    defaults();
}

TEST_CASE("no settings file or a broken one changes nothing") {
    noFile();
    defaults();
    Settings::ignoreSea = false;
    Settings::Load();
    CHECK(Settings::ignoreSea == false);

    std::ofstream(Settings::file, std::ios::trunc) << "{ this isn't json";
    Settings::Load();
    CHECK(Settings::ignoreSea == false);
    CHECK(Settings::LoadExtensions().empty());

    // saving over the broken file gives a good one again
    Settings::Save();
    defaults();
    Settings::Load();
    CHECK(Settings::ignoreSea == false);
    defaults();
}

TEST_CASE("Save doesn't wipe what the extensions keep in the same file") {
    noFile();
    defaults();
    Settings::SaveExtensions({ { "markers", { { "size", 3 } } } });
    Settings::Save();

    CHECK(Settings::LoadExtensions()["markers"]["size"] == 3);
}
