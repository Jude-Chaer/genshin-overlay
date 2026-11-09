#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "files.hpp"
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

TEST_CASE("a settings.json cut short is kept as settings.json.broken") {
    // the real file from tests/data, cut half way as a crash while saving would
    std::string whole = Files::readText(std::filesystem::path(__FILE__).parent_path() / "data" / "settings" / "old_permissions.json");
    std::string cut = whole.substr(0, whole.size() / 2);
    std::filesystem::path broken = Settings::file;
    broken += ".broken";
    std::filesystem::remove(broken);
    std::ofstream(Settings::file, std::ios::binary | std::ios::trunc) << cut;

    defaults();
    Settings::Load();
    CHECK(Settings::ignoreSea == true);
    CHECK(Files::readText(broken) == cut);
    CHECK_FALSE(std::filesystem::exists(Settings::file));

    // a save starts a new file and leaves the broken one alone
    Settings::Save();
    CHECK(Files::readText(broken) == cut);
    Settings::Load();
    CHECK(Settings::ignoreSea == true);
    std::filesystem::remove(broken);
    defaults();
}

TEST_CASE("an empty settings.json isn't kept as broken, an older broken one isn't overwritten") {
    std::filesystem::path broken = Settings::file, broken2 = Settings::file;
    broken += ".broken";
    broken2 += ".broken2";
    std::filesystem::remove(broken);
    std::filesystem::remove(broken2);

    std::ofstream(Settings::file, std::ios::trunc).close();
    defaults();
    Settings::ignoreSea = false;
    Settings::Load();
    CHECK(Settings::ignoreSea == false);
    CHECK(std::filesystem::exists(Settings::file));
    CHECK_FALSE(std::filesystem::exists(broken));

    std::ofstream(broken, std::ios::trunc) << "the first broken one";
    std::ofstream(Settings::file, std::ios::trunc) << "{ the second";
    Settings::Load();
    CHECK(Files::readText(broken) == "the first broken one");
    CHECK(Files::readText(broken2) == "{ the second");
    std::filesystem::remove(broken);
    std::filesystem::remove(broken2);
    defaults();
}

TEST_CASE("Save doesn't wipe what the extensions keep in the same file") {
    noFile();
    defaults();
    Settings::SaveExtensions({ { "markers", { { "size", 3 } } } });
    Settings::Save();

    CHECK(Settings::LoadExtensions()["markers"]["size"] == 3);
}

TEST_CASE("an extension's setting that isn't UTF-8 is saved with the bad bytes replaced") {
    // what DefineExtensionSetting("label", "Label", "string", "a\255b") keeps
    noFile();
    CHECK_NOTHROW(Settings::SaveExtensions({ { "markers", { { "label", "a\xff" "b" } } } }));
    CHECK(Settings::LoadExtensions()["markers"]["label"] == "a\xEF\xBF\xBD" "b");
    // the rest of the file is saved with it
    defaults();
    Settings::ignoreSea = false;
    CHECK_NOTHROW(Settings::Save());
    Settings::Load();
    CHECK(Settings::ignoreSea == false);
    defaults();
}
