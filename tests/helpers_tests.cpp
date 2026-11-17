#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include "helpers.hpp"

static std::filesystem::path folder() {
    std::filesystem::path folder = std::filesystem::temp_directory_path() / "genshin-overlay-tests" / "helpers";
    std::filesystem::create_directories(folder);
    return std::filesystem::canonical(folder);
}

TEST_CASE("hashString is SHA-512 in hex") {
    CHECK(Helpers::hashString("abc") ==
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(Helpers::hashString("abc") != Helpers::hashString("abd"));
}

TEST_CASE("hashFile gives the same as hashString of what's in the file") {
    std::string text = "print('hello')\n";
    std::filesystem::path path = folder() / "main.lua";
    std::ofstream(path, std::ios::binary) << text;
    CHECK(Helpers::hashFile(path) == Helpers::hashString(text));
    CHECK(Helpers::hashFile(folder() / "not_there.lua") == "");
}

TEST_CASE("resolveRelativePath cleans the path up") {
    std::filesystem::path base = folder();
    CHECK(Helpers::resolveRelativePath(base, "a/../b.txt") == base / "b.txt");
    CHECK(Helpers::resolveRelativePath(base, "./a/b.txt") == base / "a" / "b.txt");
    // an extension can ask for a file outside its folder this way
    CHECK(Helpers::resolveRelativePath(base, "../b.txt") == base.parent_path() / "b.txt");
}

TEST_CASE("a picture too big is refused before it's unpacked") {
    // a real icon whose header now says 60000 x 60000, a few KB that would unpack to 14 GB
    std::ifstream icon(std::filesystem::path(__FILE__).parent_path() / "data" / "items" / "folder" / "icons" / "231.png", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(icon)), std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 24);
    REQUIRE(bytes.substr(12, 4) == "IHDR");
    for (int at : { 16, 20 }) {
        bytes[at] = 0; bytes[at + 1] = 0; bytes[at + 2] = (char)0xEA; bytes[at + 3] = (char)0x60;
    }
    std::filesystem::path huge = folder() / "huge.png";
    std::ofstream(huge, std::ios::binary | std::ios::trunc) << bytes;

    // the refusals come before anything is sent to the graphics card, so no window is needed
    int width = -1, height = -1;
    CHECK(Helpers::loadTextureFromFile(huge, width, height) == 0);
    CHECK(width == 0);
    CHECK(height == 0);

    // a file over 16 MB is read no further than one byte past it
    std::filesystem::path big = folder() / "big.png";
    {
        std::ofstream out(big, std::ios::binary | std::ios::trunc);
        out << bytes.substr(0, 24);
        out << std::string(17 * 1024 * 1024, '\0');
    }
    std::ostringstream log;
    std::streambuf* before = std::cerr.rdbuf(log.rdbuf());
    CHECK(Helpers::loadTextureFromFile(big, width, height) == 0);
    std::cerr.rdbuf(before);
    CHECK(log.str().find("bigger than 16 MB") != std::string::npos);
    // and something that isn't a picture
    std::filesystem::path text = folder() / "text.png";
    std::ofstream(text, std::ios::binary | std::ios::trunc) << "not a picture";
    CHECK(Helpers::loadTextureFromFile(text, width, height) == 0);
}

TEST_CASE("moveOldData moves an older overlay's files once, never over newer ones") {
    namespace fs = std::filesystem;
    fs::path from = folder() / "old", to = folder() / "data";
    fs::remove_all(from);
    fs::remove_all(to);
    fs::create_directories(from / "backups");
    fs::create_directories(from / "map_data");
    fs::create_directories(to);
    std::ofstream(from / "settings.json", std::ios::binary) << "{\"size\":110}";
    std::ofstream(from / "backups" / "marks-1.json", std::ios::binary) << "[]";
    std::ofstream(from / "map_data" / "manifest.tsv", std::ios::binary) << "# map";
    std::ofstream(from / "log.txt", std::ios::binary) << "old log";
    // the data folder's own imgui.ini stays, the old one isn't there
    std::ofstream(to / "imgui.ini", std::ios::binary) << "[Window]";

    std::vector<std::string> moved = Helpers::moveOldData(from, to);
    CHECK(moved == std::vector<std::string>{ "settings.json", "backups", "map_data" });
    CHECK(fs::exists(to / "settings.json"));
    CHECK(fs::exists(to / "backups" / "marks-1.json"));
    CHECK(fs::exists(to / "map_data" / "manifest.tsv"));
    CHECK_FALSE(fs::exists(from / "settings.json"));
    CHECK(fs::exists(from / "log.txt"));
    CHECK_FALSE(fs::exists(to / "log.txt"));

    // a later start with an old settings.json back in the exe's folder
    std::ofstream(from / "settings.json", std::ios::binary) << "{\"size\":80}";
    CHECK(Helpers::moveOldData(from, to).empty());
    std::ifstream kept(to / "settings.json", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) == "{\"size\":110}");
    kept.close();
    fs::remove_all(from);
    fs::remove_all(to);
}
