#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include "files.hpp"
#include "item_files.hpp"
#include "item_patch.hpp"

// update/ in tests/data/items holds real files of two builds, answers.md there
// says what changed between them.

namespace fs = std::filesystem;

namespace {
    const fs::path DATA = fs::path(__FILE__).parent_path() / "data" / "items";
    const fs::path WORK = fs::temp_directory_path() / "genshin-overlay-tests" / "item-patch";

    // the test folder with one side of update/ added, as a raw pack
    std::string side(const char* name) {
        fs::path folder = WORK / name;
        fs::remove_all(folder);
        fs::create_directories(WORK);
        fs::copy(DATA / "folder", folder, fs::copy_options::recursive);
        fs::copy(DATA / "update" / name, folder, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        std::string raw;
        REQUIRE(ItemFiles::pack(folder, raw));
        return raw;
    }

    std::string applied(const std::string& from, const std::string& patch) {
        std::string out;
        REQUIRE(ItemPatch::apply(from, patch, 1u << 30, out));
        return out;
    }
}

TEST_CASE("a patch turns the older pack into the newer one") {
    std::string before = side("before"), after = side("after");
    CHECK(applied(before, ItemPatch::make(before, after)) == after);
    CHECK(applied(after, ItemPatch::make(after, before)) == before);
}

TEST_CASE("a patch carries the points that changed, not the files they're in") {
    std::string before = side("before"), after = side("after");
    std::string patch = ItemPatch::make(before, after);
    // the four changed points, the new type's point and its icon, and the size lines of the files
    CHECK(patch.find("\"id\":60111,") != std::string::npos);
    CHECK(patch.find("\"id\":48113,") != std::string::npos);
    CHECK(patch.find("\"id\":105949,") != std::string::npos);
    CHECK(patch.find(Files::readText(DATA / "update" / "after" / "icons" / "830.png")) != std::string::npos);
    // a point that didn't change, in the same file as one that did
    CHECK(patch.find("\"id\":99029,") == std::string::npos);
    // 7570 of it is the icon
    CHECK(patch.size() < 9000);
    CHECK(after.size() > 40000);
}

TEST_CASE("a patch between the same packs only copies") {
    std::string before = side("before");
    std::string patch = ItemPatch::make(before, before);
    CHECK(patch.size() < 64);
    CHECK(applied(before, patch) == before);
}

TEST_CASE("a patch to or from an empty pack") {
    std::string after = side("after");
    CHECK(applied("", ItemPatch::make("", after)) == after);
    CHECK(applied(after, ItemPatch::make(after, "")).empty());
}

TEST_CASE("a patch that isn't whole or reaches past the old pack is refused") {
    std::string from = "0123456789", out;
    SECTION("good") {
        CHECK(ItemPatch::apply(from, "items patch 1\nc 2 3\na 2\nxy", 100, out));
        CHECK(out == "234xy");
    }
    SECTION("no header") {
        CHECK_FALSE(ItemPatch::apply(from, "c 0 3\n", 100, out));
    }
    SECTION("another version") {
        CHECK_FALSE(ItemPatch::apply(from, "items patch 2\nc 0 3\n", 100, out));
    }
    SECTION("a copy past the end") {
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nc 8 3\n", 100, out));
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nc 11 0\n", 100, out));
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nc 1 18446744073709551615\n", 100, out));
    }
    SECTION("added bytes past the end of the patch") {
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\na 5\nxy", 100, out));
    }
    SECTION("more than it may come to") {
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nc 0 10\na 2\nxy", 11, out));
    }
    SECTION("a step it doesn't know, or numbers that aren't") {
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nd 0 3\n", 100, out));
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nc -1 3\n", 100, out));
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nc 0 3", 100, out));
        CHECK_FALSE(ItemPatch::apply(from, "items patch 1\nc  3\n", 100, out));
    }
}
