#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "item_catalog.hpp"
#include "files.hpp"
#include "item_files.hpp"
#include "map_data.hpp"
#include "net_stop.hpp"

// Every answer here is from tests/data/items/answers.md, read off the files by
// hand. The folder was cut from the item files of 2026-10-17.

namespace fs = std::filesystem;
using Catch::Matchers::WithinAbs;

namespace {
    const fs::path FOLDER = fs::path(__FILE__).parent_path() / "data" / "items" / "folder";
    const fs::path WORK = fs::temp_directory_path() / "genshin-overlay-tests" / "catalog";

    // a folder as the builder leaves it, packed and loaded like a downloaded pack
    std::shared_ptr<ItemCatalog::Catalog> loadFolder(const fs::path& folder, const Net::Stopper* stop = nullptr) {
        std::string raw;
        if (!ItemFiles::pack(folder, raw)) return nullptr;
        return ItemCatalog::load(raw, stop);
    }

    // the test pack's own catalog, never the one published to everyone
    std::shared_ptr<const ItemCatalog::Catalog> catalog() {
        static std::shared_ptr<const ItemCatalog::Catalog> loaded = loadFolder(FOLDER);
        REQUIRE(loaded);
        return loaded;
    }

    const ItemCatalog::Type* type(int id) {
        for (const auto& group : catalog()->groups) {
            for (const auto& t : group.types) if (t.id == id) return &t;
        }
        return nullptr;
    }

    std::vector<int64_t> ids(const ItemCatalog::Points& points) {
        std::vector<int64_t> out;
        for (const auto& point : points) out.push_back(point.id);
        return out;
    }

    // a copy of the test folder with positions/<type>.json replaced
    fs::path folderWith(int type, const std::string& positions) {
        fs::remove_all(WORK);
        fs::copy(FOLDER, WORK, fs::copy_options::recursive);
        Files::writeFile(WORK / "positions" / (std::to_string(type) + ".json"), positions);
        return WORK;
    }
}

TEST_CASE("the type list is HoYoLAB's tree, merged, with points per map") {
    using Counts = std::vector<std::pair<int, int>>;
    const auto& groups = catalog()->groups;
    REQUIRE(groups.size() == 4);

    struct Want { int group; std::string groupName; int type; std::string name; Counts counts; };
    std::vector<Want> want = {
        { 1, "Waypoints", 589, "Temple of Silence", {} },
        { 186, "Puzzle Chest", 509, "Fontaine Shrine of Depths", { { 2, 8 }, { 34, 2 } } },
        { 186, "Puzzle Chest", 231, "Light-Up Tile Puzzle", { { 2, 5 }, { 9, 1 } } },
        { 51, "Experience", 189, "Treasure Map", { { 2, 11 } } },
        { 232, "Fishing", 239, "Lunged Stickleback", { { 2, 7 }, { 7, 1 } } },
    };
    std::vector<Want> got;
    for (const auto& group : groups) {
        for (const auto& t : group.types) got.push_back({ group.id, group.name, t.id, t.name, t.counts });
    }
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < want.size(); i++) {
        CHECK(got[i].group == want[i].group);
        CHECK(got[i].groupName == want[i].groupName);
        CHECK(got[i].type == want[i].type);
        CHECK(got[i].name == want[i].name);
        CHECK(got[i].counts == want[i].counts);
    }
    CHECK(catalog()->pointCount() == 35);
    CHECK(catalog()->skippedPoints() == 0);
    CHECK(catalog()->skippedFiles() == 0);
}

TEST_CASE("the points of one type come in the file's order, with every field") {
    auto points = catalog()->points(231);
    REQUIRE(points.size() == 6);

    struct Want { int id, map; double x, y; int area, group, floor; };
    std::vector<Want> want = {
        { 47783, 2, 3008.75, 6754.75, 3, 84, 9 },
        { 17631, 2, 3498, 6556.5, 3, 0, 0 },
        { 17630, 2, 3530.5, 7074.5, 3, 0, 0 },
        { 17629, 2, 3275, 6796, 3, 0, 0 },
        { 17628, 2, 2998, 6608.5, 3, 0, 0 },
        { 33158, 9, 239, 426.5, 6, 0, 0 },
    };
    for (size_t i = 0; i < want.size(); i++) {
        CHECK(points[i].id == want[i].id);
        CHECK(points[i].map == want[i].map);
        CHECK(points[i].x == want[i].x);
        CHECK(points[i].y == want[i].y);
        CHECK(points[i].area == want[i].area);
        CHECK(points[i].group == want[i].group);
        CHECK(points[i].floor == want[i].floor);
        CHECK(points[i].type == 231);
    }
}

TEST_CASE("underground points keep their group and floor") {
    std::map<std::pair<int, int>, std::set<int64_t>> treasure;
    for (const auto& point : catalog()->points(189)) treasure[{ point.group, point.floor }].insert(point.id);
    CHECK(treasure[{ 90, 78 }] == std::set<int64_t>{ 56249, 56247, 56246, 56245, 56244, 56243, 56242 });
    CHECK(treasure[{ 109, 133 }] == std::set<int64_t>{ 56248 });
    CHECK(treasure[{ 0, 0 }] == std::set<int64_t>{ 82140, 82139, 82138 });
    CHECK(treasure.size() == 3);

    std::set<int64_t> shrineUnder;
    int shrineSurface = 0;
    for (const auto& point : catalog()->points(509)) {
        if (point.group == 0 && point.floor == 0) shrineSurface++;
        else {
            CHECK(point.group == 90);
            CHECK(point.floor == 78);
            shrineUnder.insert(point.id);
        }
    }
    CHECK(shrineUnder == std::set<int64_t>{ 54044, 50961 });
    CHECK(shrineSurface == 8);

    int onFloor = 0;
    for (const auto& point : catalog()->allPoints()) if (point.group == 90 && point.floor == 78) onFloor++;
    CHECK(onFloor == 9);
}

TEST_CASE("a point by id has its map, type and place") {
    const auto* fish = catalog()->point(23275);
    REQUIRE(fish);
    CHECK(fish->map == 7);
    CHECK(fish->type == 239);
    CHECK_THAT(fish->x, WithinAbs(-1178.8999938964844, 1e-9));
    CHECK_THAT(fish->y, WithinAbs(776.5053532876846, 1e-9));
    CHECK(fish->area == 5);

    const auto* map = catalog()->point(56248);
    REQUIRE(map);
    CHECK(map->map == 2);
    CHECK(map->type == 189);
    CHECK(map->group == 109);
    CHECK(map->floor == 133);

    const auto* shrine = catalog()->point(68808);
    REQUIRE(shrine);
    CHECK(shrine->map == 34);
    CHECK(shrine->type == 509);
    CHECK(shrine->x == 75.5);
    CHECK(shrine->y == 147.5);
    CHECK(shrine->area == 9);
}

TEST_CASE("the regions are there once each, with their boxes") {
    const auto& regions = catalog()->regions;
    CHECK(regions.size() == 14);
    std::set<int> seen;
    for (const auto& region : regions) CHECK(seen.insert(region.id).second);

    auto find = [&](int id) -> const ItemCatalog::Region* {
        for (const auto& region : regions) if (region.id == id) return &region;
        return nullptr;
    };
    const auto* mondstadt = find(1);
    REQUIRE(mondstadt);
    CHECK(mondstadt->name == "Mondstadt");
    CHECK(mondstadt->map == 2);
    CHECK(mondstadt->left == -1031);
    CHECK(mondstadt->top == -688);
    CHECK(mondstadt->right == 1195);
    CHECK(mondstadt->bottom == 1069);

    const auto* chasm = find(6);
    REQUIRE(chasm);
    CHECK(chasm->name == "The Chasm: Underground Mines");
    CHECK(chasm->map == 9);
    CHECK(chasm->left == -659);
    CHECK(chasm->top == -436);
    CHECK(chasm->right == 505);
    CHECK(chasm->bottom == 433);
}

TEST_CASE("a point's region is its area, even outside the region's box") {
    const auto* inazuma = [&]() -> const ItemCatalog::Region* {
        for (const auto& region : catalog()->regions) if (region.id == 3) return &region;
        return nullptr;
    }();
    REQUIRE(inazuma);
    const auto* puzzle = catalog()->point(17630);
    REQUIRE(puzzle);
    CHECK(puzzle->area == 3);
    CHECK(puzzle->y > inazuma->bottom);
}

TEST_CASE("points keep their appsample pair") {
    CHECK(catalog()->appsamplePair(47783) == 48401);
    CHECK(catalog()->appsamplePair(33158) == 42251);
    CHECK(catalog()->appsamplePair(23275) == 25865);
    for (const auto& point : catalog()->points(509)) CHECK(catalog()->appsamplePair(point.id) == 0);
}

TEST_CASE("icons are there only for the types that have one, as their png") {
    for (int type : { 231, 239 }) {
        INFO(type);
        std::string png = Files::readText(FOLDER / "icons" / (std::to_string(type) + ".png"));
        REQUIRE(!png.empty());
        CHECK(catalog()->icon(type) == png);
    }
    for (int type : { 189, 509, 589 }) CHECK(catalog()->icon(type).empty());
}

TEST_CASE("unknown ids give nothing, never an error") {
    CHECK(catalog()->points(999999).empty());
    CHECK(catalog()->icon(999999).empty());
    CHECK(type(999999) == nullptr);
    CHECK(catalog()->point(1) == nullptr);
    CHECK(catalog()->appsamplePair(1) == 0);
    // a type in the tree with no points at all
    CHECK(catalog()->points(589).empty());
    REQUIRE(type(589));
    CHECK(type(589)->counts.empty());
}

TEST_CASE("a point whose values don't fit is skipped, the others stay") {
    auto loaded = loadFolder(folderWith(231, R"([
        {"id":1,"map_id":2,"x_pos":1,"y_pos":2,"area_id":3,"point_group":null},
        {"id":4294967296,"map_id":2,"x_pos":1,"y_pos":2,"area_id":3,"point_group":null},
        {"id":2,"map_id":70000,"x_pos":1,"y_pos":2,"area_id":3,"point_group":null},
        {"id":3,"map_id":2,"x_pos":"1","y_pos":2,"area_id":3,"point_group":null},
        {"id":4,"map_id":2,"x_pos":1,"y_pos":2,"point_group":null},
        {"id":5,"map_id":2,"x_pos":1,"y_pos":2,"area_id":3,"point_group":{"group_id":1}},
        {"id":6,"map_id":2,"x_pos":1,"y_pos":2,"area_id":-1,"point_group":null},
        "not a point",
        {"id":7,"map_id":2,"x_pos":1,"y_pos":2,"area_id":3,"point_group":null,"appsample":4294967296}
    ])"));
    REQUIRE(loaded);
    CHECK(ids(loaded->points(231)) == std::vector<int64_t>{ 1, 7 });
    CHECK(loaded->skippedPoints() == 7);
    // each by the first field it fails on ("not a point" has no id)
    std::map<std::string, size_t> fields = { { "id", 2 }, { "map_id", 1 }, { "x_pos", 1 }, { "area_id", 2 }, { "point_group.floor_id", 1 } };
    CHECK(loaded->skippedFields() == fields);
    // a pair that doesn't fit costs the pair, not the point
    CHECK(loaded->appsamplePair(7) == 0);
    // the other types are untouched
    CHECK(loaded->points(189).size() == 11);
}

TEST_CASE("a positions file that isn't JSON costs only its type") {
    auto loaded = loadFolder(folderWith(231, "{ cut short"));
    REQUIRE(loaded);
    CHECK(loaded->points(231).empty());
    CHECK(loaded->points(239).size() == 8);
    CHECK(loaded->skippedFiles() == 1);
    // the icon is still there
    CHECK(!loaded->icon(231).empty());
}

TEST_CASE("no catalog from a pack without its tree or its positions, or one cut short") {
    CHECK(ItemCatalog::load("") == nullptr);
    fs::path folder = folderWith(231, "[]");
    fs::remove(folder / "tree.json");
    CHECK(loadFolder(folder) == nullptr);

    folder = folderWith(231, "[]");
    fs::remove_all(folder / "positions");
    CHECK(loadFolder(folder) == nullptr);

    std::string raw;
    REQUIRE(ItemFiles::pack(FOLDER, raw));
    CHECK(ItemCatalog::load(raw.substr(0, raw.size() - 1)) == nullptr);
}

TEST_CASE("a load that's told to stop gives up with no catalog") {
    std::string raw;
    REQUIRE(ItemFiles::pack(FOLDER, raw));
    Net::Stopper stop;
    stop.stop();
    CHECK(ItemCatalog::load(raw, &stop) == nullptr);
}

TEST_CASE("a group without a name keeps its types, a type in two groups shows in both") {
    fs::path folder = folderWith(231, Files::readText(FOLDER / "positions" / "231.json"));
    Files::writeFile(folder / "tree.json", R"([
        {"id":186,"name":null,"children":[{"id":231,"name":"Light-Up Tile Puzzle"}]},
        {"id":51,"name":"Experience","children":[{"id":231,"name":"Light-Up Tile Puzzle"},{"id":189}]}
    ])");
    auto loaded = loadFolder(folder);
    REQUIRE(loaded);
    REQUIRE(loaded->groups.size() == 2);
    CHECK(loaded->groups[0].name.empty());
    REQUIRE(loaded->groups[0].types.size() == 1);
    CHECK(loaded->groups[0].types[0].id == 231);
    REQUIRE(loaded->groups[1].types.size() == 2);
    CHECK(loaded->groups[1].types[0].id == 231);
    CHECK(loaded->groups[1].types[0].counts == loaded->groups[0].types[0].counts);
    CHECK(loaded->groups[1].types[1].name.empty());
}

TEST_CASE("only plain type numbers are type files") {
    fs::path folder = folderWith(231, Files::readText(FOLDER / "positions" / "231.json"));
    // not JSON, so they'd count as broken files if they were read as types
    Files::writeFile(folder / "positions" / "0231.json", "x");
    Files::writeFile(folder / "positions" / "notes.json", "x");
    auto loaded = loadFolder(folder);
    REQUIRE(loaded);
    CHECK(loaded->points(231).size() == 6);
    CHECK(loaded->pointCount() == 35);
    CHECK(loaded->skippedFiles() == 0);
}

TEST_CASE("each published catalog gets the next version and old ones stay readable") {
    uint64_t before = ItemCatalog::version();
    auto first = loadFolder(FOLDER);
    REQUIRE(first);
    ItemCatalog::publish(first);
    CHECK(ItemCatalog::version() == before + 1);
    auto held = ItemCatalog::current();
    REQUIRE(held);
    CHECK(held->version == before + 1);

    ItemCatalog::publish(loadFolder(FOLDER));
    CHECK(ItemCatalog::version() == before + 2);
    REQUIRE(ItemCatalog::current());
    CHECK(ItemCatalog::current()->version == before + 2);
    // the reader that took the first one still has it, whole
    CHECK(held->version == before + 1);
    CHECK(held->pointCount() == 35);

    // a failed load hands nothing over
    ItemCatalog::publish(nullptr);
    CHECK(ItemCatalog::version() == before + 2);

    // the same one twice is published once, a reader's copy never changes
    ItemCatalog::publish(first);
    CHECK(ItemCatalog::version() == before + 2);
    CHECK(held->version == before + 1);
}
