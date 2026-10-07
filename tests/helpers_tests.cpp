#include <filesystem>
#include <fstream>
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
