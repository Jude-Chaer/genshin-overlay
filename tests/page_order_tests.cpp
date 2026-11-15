#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include "page_order.hpp"
#include "settings.hpp"

using PageOrder::List;
using PageOrder::Place;

// "minimap [Folder: map markers] fps", a - after a closed folder's name
static std::string show(const List& list) {
    std::string text;
    for (const auto& item : list) {
        if (!text.empty()) text += " ";
        if (!item.folder) {
            text += item.key;
            continue;
        }
        text += "[" + item.key + (item.open ? "" : "-") + ":";
        for (const auto& key : item.inside) text += " " + key;
        text += "]";
    }
    return text;
}

static List folderOf(const std::string& name, std::vector<std::string> inside, bool open = true) {
    return { { name, true, open, inside } };
}

static List make(std::initializer_list<List> parts) {
    List list;
    for (const auto& part : parts) list.insert(list.end(), part.begin(), part.end());
    return list;
}

static List one(const std::string& key) {
    return { { key } };
}

TEST_CASE("the saved order is kept, missing extensions dropped and new ones put at the end") {
    List saved = make({ one("minimap"), folderOf("Maps", { "map", "markers" }, false), one("fps") });

    CHECK(show(PageOrder::Arrange(saved, { "map", "markers", "minimap", "fps" })) == "minimap [Maps-: map markers] fps");
    CHECK(show(PageOrder::Arrange(saved, { "fps", "map", "minimap", "markers", "chests" })) == "minimap [Maps-: map markers] fps chests");
    CHECK(show(PageOrder::Arrange(saved, { "map", "minimap", "markers" })) == "minimap [Maps-: map markers]");
    // a folder with one left stays, an empty one goes
    CHECK(show(PageOrder::Arrange(saved, { "map", "minimap", "fps" })) == "minimap [Maps-: map] fps");
    CHECK(show(PageOrder::Arrange(saved, { "minimap", "fps" })) == "minimap fps");
    // nothing saved yet: load order
    CHECK(show(PageOrder::Arrange({}, { "map", "minimap", "markers" })) == "map minimap markers");
}

TEST_CASE("an extension saved twice shows once, where it was first") {
    List saved = make({ one("map"), folderOf("Maps", { "minimap", "map" }), one("minimap") });
    CHECK(show(PageOrder::Arrange(saved, { "map", "minimap" })) == "map [Maps: minimap]");
    CHECK(show(PageOrder::Arrange(folderOf("", { "map" }), { "map" })) == "[Folder: map]");
}

TEST_CASE("the order and folders come back after saving") {
    std::filesystem::remove(Settings::file);

    List list = make({ one("minimap"), folderOf("Maps", { "map", "markers" }, false), one("fps") });
    Settings::SavePageList(PageOrder::ToJson(list));
    CHECK(show(PageOrder::FromJson(Settings::LoadPageList())) == "minimap [Maps-: map markers] fps");
    CHECK(PageOrder::ToJson(list).dump() == R"(["minimap",{"folder":"Maps","items":["map","markers"],"open":false},"fps"])");

    Settings::SavePageList(PageOrder::ToJson({}));
    CHECK(Settings::LoadPageList().empty());
    std::filesystem::remove(Settings::file);
}

TEST_CASE("a saved list that isn't what the overlay writes gives what it can") {
    auto parse = [](const char* text) { return show(PageOrder::FromJson(nlohmann::json::parse(text))); };
    CHECK(parse(R"({"map": 1})") == "");
    CHECK(parse(R"(["map", 3, null, {"items": ["fps"]}, {"folder": "Maps", "open": "no", "items": ["minimap", 4]}])") == "map [Maps: minimap]");
}

TEST_CASE("up, down, top and bottom move within the item's own level") {
    List list = make({ one("a"), folderOf("F", { "b", "c", "d" }), one("e") });

    CHECK(PageOrder::Up(list, { 0, -1 }) == Place{ -1, -1 });
    CHECK(PageOrder::Down(list, { 0, -1 }) == Place{ 1, -1 });
    CHECK(show(list) == "[F: b c d] a e");
    CHECK(PageOrder::Down(list, { 2, -1 }) == Place{ -1, -1 });
    CHECK(PageOrder::ToTop(list, { 2, -1 }) == Place{ 0, -1 });
    CHECK(show(list) == "e [F: b c d] a");
    CHECK(PageOrder::ToBottom(list, { 1, -1 }) == Place{ 2, -1 });
    CHECK(show(list) == "e a [F: b c d]");

    CHECK(PageOrder::Up(list, { 2, 2 }) == Place{ 2, 1 });
    CHECK(show(list) == "e a [F: b d c]");
    CHECK(PageOrder::Down(list, { 2, 0 }) == Place{ 2, 1 });
    CHECK(show(list) == "e a [F: d b c]");
    CHECK(PageOrder::Down(list, { 2, 2 }) == Place{ -1, -1 });
    CHECK(PageOrder::ToBottom(list, { 2, 0 }) == Place{ 2, 2 });
    CHECK(PageOrder::ToTop(list, { 2, 1 }) == Place{ 2, 0 });
    CHECK(show(list) == "e a [F: c b d]");
    CHECK(PageOrder::ToTop(list, { 2, 0 }) == Place{ -1, -1 });
    CHECK(PageOrder::Up(list, { 2, 0 }) == Place{ -1, -1 });
    CHECK(show(list) == "e a [F: c b d]");
}

TEST_CASE("an extension goes into a folder at its end and comes out right after it") {
    List list = make({ one("a"), folderOf("F", { "b" }), one("c"), folderOf("G", { "d" }) });

    CHECK(PageOrder::IntoFolder(list, { 0, -1 }, 1) == Place{ 0, 1 });
    CHECK(show(list) == "[F: b a] c [G: d]");
    CHECK(PageOrder::IntoFolder(list, { 0, 0 }, 2) == Place{ 2, 1 });
    CHECK(show(list) == "[F: a] c [G: d b]");
    CHECK(PageOrder::IntoFolder(list, { 2, 0 }, 2) == Place{ -1, -1 });
    CHECK(PageOrder::IntoFolder(list, { 1, -1 }, 1) == Place{ -1, -1 });
    // no folders in folders
    CHECK(PageOrder::IntoFolder(list, { 0, -1 }, 2) == Place{ -1, -1 });

    CHECK(PageOrder::OutOfFolder(list, { 2, 0 }) == Place{ 3, -1 });
    CHECK(show(list) == "[F: a] c [G: b] d");
    // the last one out takes the folder with it
    CHECK(PageOrder::OutOfFolder(list, { 0, 0 }) == Place{ 0, -1 });
    CHECK(show(list) == "a c [G: b] d");
    CHECK(PageOrder::OutOfFolder(list, { 1, -1 }) == Place{ -1, -1 });
}

TEST_CASE("a new folder is made where the extension was, with a name not taken") {
    List list = make({ one("a"), one("b"), folderOf("F", { "c", "d" }) });

    CHECK(PageOrder::NewFolder(list, { 1, -1 }) == Place{ 1, 0 });
    CHECK(show(list) == "a [Folder: b] [F: c d]");
    CHECK(PageOrder::NewFolder(list, { 2, 0 }) == Place{ 3, 0 });
    CHECK(show(list) == "a [Folder: b] [F: d] [Folder 2: c]");
    CHECK(PageOrder::NewFolder(list, { 2, 0 }) == Place{ 2, 0 });
    CHECK(show(list) == "a [Folder: b] [Folder 3: d] [Folder 2: c]");
    CHECK(PageOrder::NewFolder(list, { 1, -1 }) == Place{ -1, -1 });
}

TEST_CASE("dragging puts the extension before the item it's dropped on, or in with it") {
    List list = make({ one("a"), one("b"), folderOf("F", { "c", "d" }), one("e") });

    CHECK(PageOrder::Move(list, { 0, -1 }, { 3, -1 }) == Place{ 2, -1 });
    CHECK(show(list) == "b [F: c d] a e");
    CHECK(PageOrder::Move(list, { 3, -1 }, { 1, 1 }) == Place{ 1, 1 });
    CHECK(show(list) == "b [F: c e d] a");
    CHECK(PageOrder::Move(list, { 1, 0 }, { 1, 3 }) == Place{ 1, 2 });
    CHECK(show(list) == "b [F: e d c] a");
    CHECK(PageOrder::Move(list, { 1, -1 }, { 0, -1 }) == Place{ 0, -1 });
    CHECK(show(list) == "[F: e d c] b a");
    CHECK(PageOrder::Move(list, { 0, -1 }, { 1, 0 }) == Place{ -1, -1 });
    // dropped where it already is
    CHECK(PageOrder::Move(list, { 1, -1 }, { 2, -1 }) == Place{ -1, -1 });
    CHECK(PageOrder::Move(list, { 0, 1 }, { 0, 1 }) == Place{ -1, -1 });
    CHECK(PageOrder::Move(list, { 0, 1 }, { 9, -1 }) == Place{ -1, -1 });

    CHECK(PageOrder::Onto(list, { 2, -1 }, { 1, -1 }) == Place{ 1, 1 });
    CHECK(show(list) == "[F: e d c] [Folder: b a]");
    CHECK(PageOrder::Onto(list, { 0, 2 }, { 1, -1 }) == Place{ 1, 2 });
    CHECK(PageOrder::Onto(list, { 1, 2 }, { 0, 0 }) == Place{ 0, 1 });
    CHECK(show(list) == "[F: e c d] [Folder: b a]");
    CHECK(PageOrder::Onto(list, { 1, -1 }, { 0, -1 }) == Place{ -1, -1 });
    CHECK(PageOrder::Onto(list, { 0, 0 }, { 0, 0 }) == Place{ -1, -1 });
}

TEST_CASE("rename and ungroup work on folders only") {
    List list = make({ one("a"), folderOf("F", { "b", "c" }), one("d") });

    CHECK(PageOrder::Rename(list, 1, "Maps"));
    CHECK_FALSE(PageOrder::Rename(list, 1, "Maps"));
    CHECK_FALSE(PageOrder::Rename(list, 1, ""));
    CHECK_FALSE(PageOrder::Rename(list, 0, "Maps"));
    CHECK(show(list) == "a [Maps: b c] d");

    List two = make({ folderOf("A", { "a" }), folderOf("B", { "b" }) });
    CHECK_FALSE(PageOrder::Rename(two, 1, "A"));
    CHECK_FALSE(PageOrder::Rename(two, 1, "  "));
    CHECK_FALSE(PageOrder::Rename(two, 1, "Maps##2"));
    CHECK(PageOrder::Rename(two, 1, "  Tools "));
    CHECK(show(two) == "[A: a] [Tools: b]");

    CHECK_FALSE(PageOrder::Ungroup(list, 0));
    CHECK(PageOrder::Ungroup(list, 1));
    CHECK(show(list) == "a b c d");
    CHECK(PageOrder::Flat(make({ one("a"), folderOf("F", { "b", "c" }), one("d") })) == std::vector<std::string>{ "a", "b", "c", "d" });
    CHECK(PageOrder::Find(make({ one("a"), folderOf("F", { "b", "c" }) }), "c") == Place{ 1, 1 });
    CHECK(PageOrder::Find(list, "x") == Place{ -1, -1 });
}

TEST_CASE("undo and redo step through the moves, a new move clears redo") {
    PageOrder::History history;
    List list = make({ one("a"), one("b"), one("c") });

    history.Did(list);
    PageOrder::Down(list, { 0, -1 });
    history.Did(list);
    PageOrder::NewFolder(list, { 2, -1 });
    CHECK(show(list) == "b a [Folder: c]");

    CHECK(history.Undo(list));
    CHECK(show(list) == "b a c");
    CHECK(history.Undo(list));
    CHECK(show(list) == "a b c");
    CHECK_FALSE(history.Undo(list));
    CHECK(history.Redo(list));
    CHECK(show(list) == "b a c");
    CHECK(history.CanRedo());

    history.Did(list);
    PageOrder::ToTop(list, { 2, -1 });
    CHECK_FALSE(history.CanRedo());
    CHECK_FALSE(history.Redo(list));
    CHECK(show(list) == "c b a");

    for (size_t i = 0; i < PageOrder::History::Limit + 5; i++) history.Did(list);
    size_t undone = 0;
    while (history.Undo(list)) undone++;
    CHECK(undone == PageOrder::History::Limit);
}
