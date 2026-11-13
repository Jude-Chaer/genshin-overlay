#include <string>
#include <catch2/catch_test_macros.hpp>
#include "extensions.hpp"
#include "triggers.hpp"
#include "test_extension.hpp"

using Permission = Extensions::ExtensionPermissionTypes;

// loaded and its Init run, as at the overlay's start
static Extensions::Extension* start(const std::string& folder, const std::string& mainLua) {
    Extensions::Extension* ext = TestExtension::load(folder, mainLua);
    if (ext) Extensions::runInit(*ext);
    return ext;
}

// main.lua's globals are its own, the tests read _G
// An extension that sets a trigger for each word, "Name" or "Name:only", counts
// its runs in runs[word] and keeps what it was given in given[word] and changed[word].
static Extensions::Extension* counting(const std::string& folder, const std::string& words) {
    Extensions::Extension* ext = start(folder, R"(
        _G.runs = {}
        _G.given = {}
        _G.changed = {}
        _G.ids = {}
        local runs, given, changed, ids = _G.runs, _G.given, _G.changed, _G.ids
        function Init()
            for word in string.gmatch(")" + words + R"(", "%S+") do
                local name, only = word:match("^([^:]+):?(.*)$")
                runs[word] = 0
                ids[word] = AddTrigger(name, function(value, what)
                    runs[word] = runs[word] + 1
                    given[word] = value
                    changed[word] = what
                end, only ~= "" and only or nil)
            end
        end
    )");
    return ext;
}

static int runs(Extensions::Extension* ext, const std::string& word) {
    return (*ext->luaState)["runs"][word].get_or(-1);
}

static std::string changed(Extensions::Extension* ext, const std::string& word) {
    return (*ext->luaState)["changed"][word].get_or(std::string());
}

static sol::object given(Extensions::Extension* ext, const std::string& word) {
    return (*ext->luaState)["given"][word];
}

static Triggers::State screens(bool map, bool menu) {
    Triggers::State state;
    state.screens.set(Screens::Screen::Map, map);
    state.screens.set(Screens::Screen::Menu, menu);
    return state;
}

static Triggers::State mapAt(double lat, double lng, int group = 0, int floor = 0) {
    Triggers::State state = screens(true, false);
    state.view.visible = true;
    state.view.result.mapId = 2;
    state.view.result.groupId = group;
    state.view.result.floorId = floor;
    state.view.result.lat = lat;
    state.view.result.lng = lng;
    state.view.result.unitsPerPixel = 1.5;
    state.view.gameRect = { 0, 0, 1920, 1080 };
    return state;
}

static Triggers::State playerAt(bool found, double lat) {
    Triggers::State state;
    state.player.found = found;
    state.player.state = found ? MiniMap::State::Found : MiniMap::State::Near;
    state.player.mapId = 2;
    state.player.lat = lat;
    return state;
}

TEST_CASE("Screens runs once for each screen that opens or closes, not at the start", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_screens", "Screens Screens:map");
    REQUIRE(ext != nullptr);

    // the map already open when it was set: that's the start, nothing runs
    Triggers::check(screens(true, false));
    Triggers::check(screens(true, false));
    CHECK(runs(ext, "Screens") == 0);

    Triggers::check(screens(false, false));
    CHECK(runs(ext, "Screens") == 1);
    CHECK(changed(ext, "Screens") == "map");
    CHECK(given(ext, "Screens").as<sol::table>()["map"].get<bool>() == false);

    // the menu opening with the map is two changes
    Triggers::check(screens(true, true));
    CHECK(runs(ext, "Screens") == 3);
    {
        // let go of before its Lua state closes
        sol::table up = given(ext, "Screens").as<sol::table>();
        CHECK(up["map"].get<bool>());
        CHECK(up["menu"].get<bool>());
    }

    // only the map
    CHECK(runs(ext, "Screens:map") == 2);
    Triggers::check(screens(true, false));
    CHECK(runs(ext, "Screens") == 4);
    CHECK(changed(ext, "Screens") == "menu");
    CHECK(runs(ext, "Screens:map") == 2);
    TestExtension::unloadAll();
}

TEST_CASE("Screens needs no permission", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_screens_free", "Screens");
    REQUIRE(ext != nullptr);
    ext->extensionPermissions.clear();
    Triggers::check(screens(false, false));
    Triggers::check(screens(true, false));
    CHECK(runs(ext, "Screens") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("MapView gives GetMapView's table, a new floor over a move", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_view", "MapView MapView:floor");
    REQUIRE(ext != nullptr);
    Extensions::allow(*ext, Permission::MapInfo);

    Triggers::check(mapAt(10, 20));
    Triggers::check(mapAt(10, 20));
    CHECK(runs(ext, "MapView") == 0);

    Triggers::check(mapAt(11, 20));
    Triggers::check(mapAt(11, 20));
    CHECK(runs(ext, "MapView") == 1);
    CHECK(changed(ext, "MapView") == "view");
    {
        // let go of before its Lua state closes
        sol::table view = given(ext, "MapView").as<sol::table>();
        CHECK(view["lat"].get<double>() == 11.0);
        CHECK(view["mapId"].get<int>() == 2);
    }

    // nil while it moves is a change too, GetMapView gives nil then
    Triggers::State moving = mapAt(11, 20);
    moving.view.visible = false;
    Triggers::check(moving);
    CHECK(runs(ext, "MapView") == 2);
    CHECK(given(ext, "MapView") == sol::lua_nil);
    // and not a new floor
    CHECK(runs(ext, "MapView:floor") == 0);

    Triggers::check(mapAt(11, 20, 7, 2));
    CHECK(runs(ext, "MapView") == 3);
    CHECK(changed(ext, "MapView") == "floor");
    CHECK(runs(ext, "MapView:floor") == 1);
    CHECK(given(ext, "MapView:floor").as<sol::table>()["groupId"].get<int>() == 7);
    TestExtension::unloadAll();
}

TEST_CASE("Player runs on found, lost and moved, found or lost over moved", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_player", "Player Player:lost");
    REQUIRE(ext != nullptr);
    Extensions::allow(*ext, Permission::MapInfo);
    CHECK(Triggers::wantsPlayer());

    Triggers::check(playerAt(false, 0));
    Triggers::check(playerAt(true, 5));
    CHECK(runs(ext, "Player") == 1);
    CHECK(changed(ext, "Player") == "found");
    CHECK(given(ext, "Player").as<sol::table>()["lat"].get<double>() == 5.0);

    Triggers::check(playerAt(true, 5));
    Triggers::check(playerAt(true, 6));
    CHECK(runs(ext, "Player") == 2);
    CHECK(changed(ext, "Player") == "moved");

    // lost: where it was last seen, that's no move
    Triggers::check(playerAt(false, 6));
    Triggers::check(playerAt(false, 6));
    CHECK(runs(ext, "Player") == 3);
    CHECK(changed(ext, "Player") == "lost");
    CHECK(runs(ext, "Player:lost") == 1);
    CHECK(given(ext, "Player:lost").as<sol::table>()["state"].get<std::string>() == "near");
    TestExtension::unloadAll();
}

TEST_CASE("Items runs on a new list and while the list loads", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_items", "Items Items:list");
    REQUIRE(ext != nullptr);
    Extensions::allow(*ext, Permission::MapInfo);

    Triggers::State state;
    Triggers::check(state);
    state.items.state = ItemFiles::State::Downloading;
    state.items.percent = 40;
    Triggers::check(state);
    CHECK(runs(ext, "Items") == 1);
    CHECK(changed(ext, "Items") == "loading");
    CHECK(given(ext, "Items").as<sol::table>()["percent"].get<int>() == 40);
    CHECK(runs(ext, "Items:list") == 0);

    state.items.state = ItemFiles::State::Ready;
    state.itemsVersion = 3;
    Triggers::check(state);
    CHECK(runs(ext, "Items") == 2);
    CHECK(changed(ext, "Items") == "list");
    CHECK(given(ext, "Items").as<sol::table>()["version"].get<int>() == 3);
    CHECK(runs(ext, "Items:list") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("only still runs when a bigger change comes in the same turn", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_only", "MapView:view Items:loading Player:moved");
    REQUIRE(ext != nullptr);
    Extensions::allow(*ext, Permission::MapInfo);

    // a new floor moves the view too
    Triggers::check(mapAt(10, 20));
    Triggers::check(mapAt(10, 20, 7, 2));
    CHECK(runs(ext, "MapView:view") == 1);
    CHECK(changed(ext, "MapView:view") == "view");

    // the download ending: a new list and a new state at once
    Triggers::State items;
    items.items.state = ItemFiles::State::Downloading;
    Triggers::check(items);
    CHECK(runs(ext, "Items:loading") == 1);
    items.items.state = ItemFiles::State::Ready;
    items.itemsVersion = 1;
    Triggers::check(items);
    CHECK(runs(ext, "Items:loading") == 2);

    // found somewhere new
    Triggers::check(playerAt(true, 5));
    Triggers::check(playerAt(false, 5));
    Triggers::check(playerAt(true, 9));
    CHECK(runs(ext, "Player:moved") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("MapView runs when the floor picker comes or goes on a still map", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_picker", "MapView");
    REQUIRE(ext != nullptr);
    Extensions::allow(*ext, Permission::MapInfo);

    Triggers::State still = mapAt(10, 20);
    Triggers::check(still);
    still.picker = { 1700, 300, 1800, 600 };
    Triggers::check(still);
    CHECK(runs(ext, "MapView") == 1);
    CHECK(changed(ext, "MapView") == "view");
    TestExtension::unloadAll();
}

TEST_CASE("map, player and item triggers need Map info, and nothing from before runs once it's allowed", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_read", "MapView Player Items Screens");
    REQUIRE(ext != nullptr);
    Extensions::switchPermission(*ext, Permission::MapInfo, false);
    CHECK_FALSE(Triggers::wantsPlayer());

    Triggers::State closed;
    Triggers::check(closed);
    Triggers::State open = mapAt(1, 1);
    open.player.found = true;
    open.itemsVersion = 3;
    Triggers::check(open);
    CHECK(runs(ext, "MapView") == 0);
    CHECK(runs(ext, "Player") == 0);
    CHECK(runs(ext, "Items") == 0);
    // which screen is up isn't something Map info hides
    CHECK(runs(ext, "Screens") == 1);

    // allowed while the map is open: it starts from there
    Extensions::switchPermission(*ext, Permission::MapInfo, true);
    CHECK(Triggers::wantsPlayer());
    Triggers::check(open);
    CHECK(runs(ext, "MapView") == 0);
    Triggers::check(closed);
    Triggers::check(open);
    CHECK(runs(ext, "MapView") == 2);
    CHECK(runs(ext, "Player") == 2);
    TestExtension::unloadAll();
}

TEST_CASE("Page goes by the extension's own page", "[triggers]") {
    Extensions::Extension* first = counting("triggers_page_a", "Page Page:hidden");
    Extensions::Extension* second = counting("triggers_page_b", "Page");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    int firstPage = (int)Extensions::registeredExtensions.size() - 2;

    Triggers::State menu = screens(false, true);
    Triggers::check(menu);
    menu.pageShown = firstPage;
    Triggers::check(menu);
    CHECK(runs(first, "Page") == 1);
    CHECK(changed(first, "Page") == "shown");
    CHECK(given(first, "Page").as<bool>());
    CHECK(runs(second, "Page") == 0);
    CHECK(runs(first, "Page:hidden") == 0);

    menu.pageShown = firstPage + 1;
    Triggers::check(menu);
    CHECK(runs(first, "Page") == 2);
    CHECK(changed(first, "Page") == "hidden");
    CHECK(runs(first, "Page:hidden") == 1);
    CHECK(runs(second, "Page") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("Setting and Permission give the new value and what changed, only the extension's own", "[triggers]") {
    Extensions::Extension* ext = start("triggers_own", R"(
        DefineExtensionSetting("size", "Size", "int", 5)
        DefineExtensionSetting("name", "Name", "string", "a")
        _G.settings = {}
        _G.permissions = {}
        _G.sizes = 0
        local settings, permissions = _G.settings, _G.permissions
        function Init()
            AddTrigger("Setting", function(value, id) settings[#settings + 1] = id .. "=" .. tostring(value) end)
            AddTrigger("Setting", function() _G.sizes = _G.sizes + 1 end, "size")
            AddTrigger("Permission", function(state, name) permissions[#permissions + 1] = name .. "=" .. state end)
        end
    )");
    Extensions::Extension* other = TestExtension::load("triggers_other", R"(
        DefineExtensionSetting("size", "Size", "int", 5)
    )");
    REQUIRE(ext != nullptr);
    REQUIRE(other != nullptr);
    REQUIRE(ext->settings.count("size") == 1);
    REQUIRE(other->settings.count("size") == 1);
    sol::state& lua = *ext->luaState;

    Triggers::State state;
    Triggers::check(state);
    other->settings["size"].value = 9;
    Extensions::switchPermission(*other, Permission::MapInfo, false);
    Triggers::check(state);
    CHECK(lua["settings"].get<sol::table>().size() == 0);
    CHECK(lua["permissions"].get<sol::table>().size() == 0);

    ext->settings["size"].value = 6;
    Extensions::switchPermission(*ext, Permission::MapInfo, false);
    Triggers::check(state);
    Triggers::check(state);
    CHECK(lua["settings"].get<sol::table>().size() == 1);
    CHECK(lua["settings"][1].get_or(std::string()) == "size=6");
    CHECK(lua["sizes"].get_or(0) == 1);
    CHECK(lua["permissions"].get<sol::table>().size() == 1);
    CHECK(lua["permissions"][1].get_or(std::string()) == "map_info=off");

    // another setting doesn't run the one for size
    ext->settings["name"].value = std::string("b");
    Triggers::check(state);
    CHECK(lua["settings"].get<sol::table>().size() == 2);
    CHECK(lua["settings"][2].get_or(std::string()) == "name=b");
    CHECK(lua["sizes"].get_or(0) == 1);
    TestExtension::unloadAll();
}

TEST_CASE("AddTrigger refuses unknown names, changes it can't have and too many, RemoveTrigger takes only its own", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_ids", "Screens");
    Extensions::Extension* stranger = TestExtension::load("triggers_stranger", "function Init() end");
    REQUIRE(ext != nullptr);
    REQUIRE(stranger != nullptr);
    sol::state& lua = *ext->luaState;
    int id = lua["ids"]["Screens"].get_or(0);
    CHECK(id != 0);

    REQUIRE(TestExtension::run(ext, R"(
        unknown = AddTrigger('MapOpened', function() end)
        badChange = AddTrigger('Screens', function() end, 'inventory')
        anySetting = AddTrigger('Setting', function() end, 'not_defined_yet')
    )"));
    CHECK(lua["unknown"].get_or(-1) == 0);
    CHECK(lua["badChange"].get_or(-1) == 0);
    CHECK(lua["anySetting"].get_or(0) != 0);

    REQUIRE(TestExtension::run(stranger, "RemoveTrigger(" + std::to_string(id) + ")"));
    Triggers::check(screens(false, false));
    Triggers::check(screens(false, true));
    CHECK(runs(ext, "Screens") == 1);

    REQUIRE(TestExtension::run(ext, "RemoveTrigger(ids.Screens)"));
    Triggers::check(screens(false, false));
    Triggers::check(screens(false, true));
    CHECK(runs(ext, "Screens") == 1);

    REQUIRE(TestExtension::run(stranger, R"(
        added = 0
        for i = 1, 100 do
            if AddTrigger("Screens", function() end) ~= 0 then added = added + 1 end
        end
    )"));
    CHECK((*stranger->luaState)["added"].get_or(0) == (int)Triggers::MaxTriggers);
    CHECK(Triggers::count(stranger) == Triggers::MaxTriggers);

    // they go with the extension
    TestExtension::unloadAll();
    CHECK_FALSE(Triggers::any());
}

TEST_CASE("a trigger that removes itself or adds another while they run is fine", "[triggers]") {
    Extensions::Extension* ext = start("triggers_change", R"(
        _G.runs = 0
        function Init()
            local id
            id = AddTrigger("Screens", function()
                _G.runs = _G.runs + 1
                RemoveTrigger(id)
                AddTrigger("Screens", function() _G.runs = _G.runs + 10 end)
            end)
        end
    )");
    REQUIRE(ext != nullptr);
    Triggers::State closed, open = screens(false, true);
    Triggers::check(closed);
    Triggers::check(open);
    CHECK((*ext->luaState)["runs"].get_or(0) == 1);
    // the new one starts from the open menu
    Triggers::check(open);
    Triggers::check(closed);
    Triggers::check(open);
    CHECK((*ext->luaState)["runs"].get_or(0) == 21);
    TestExtension::unloadAll();
}

TEST_CASE("every trigger name says what it can change, or takes any id", "[triggers]") {
    for (const std::string& name : Triggers::names()) {
        std::vector<std::string> changes = Triggers::changes(name);
        if (name == "Setting" || name == "Permission") CHECK(changes.empty());
        else CHECK_FALSE(changes.empty());
    }
}

TEST_CASE("Look runs when the player picks another look, light and dark or size, or Windows' accent changes, with that look's colours", "[triggers]") {
    Extensions::Extension* ext = counting("triggers_look", "Look Look:dark");
    REQUIRE(ext != nullptr);
    Triggers::State state;
    Triggers::check(state);
    Triggers::check(state);
    CHECK(runs(ext, "Look") == 0);

    state.look = Controls::Look::Game;
    Triggers::check(state);
    CHECK(runs(ext, "Look") == 1);
    CHECK(changed(ext, "Look") == "look");
    CHECK(runs(ext, "Look:dark") == 0);
    {
        // let go of before its Lua state closes
        sol::table theme = given(ext, "Look");
        CHECK(theme["look"].get<std::string>() == "Game");
        CHECK(theme["dark"].get<bool>());
        CHECK(theme["page"].get<uint32_t>() == Controls::GetTheme(Controls::Look::Game, true).page);
    }

    state.dark = false;
    Triggers::check(state);
    CHECK(runs(ext, "Look") == 2);
    CHECK(changed(ext, "Look") == "dark");
    CHECK(runs(ext, "Look:dark") == 1);
    CHECK_FALSE(given(ext, "Look:dark").as<sol::table>()["dark"].get<bool>());

    // Windows' accent changed, which Windows 11 and Material You follow
    state.accent = 0xC44E00FF;
    Triggers::check(state);
    CHECK(runs(ext, "Look") == 3);
    CHECK(changed(ext, "Look") == "accent");
    CHECK(runs(ext, "Look:dark") == 1);

    // the player picked another size, or Windows' scale changed
    state.font = 17.5f;
    Triggers::check(state);
    CHECK(runs(ext, "Look") == 4);
    CHECK(changed(ext, "Look") == "size");
    CHECK(runs(ext, "Look:dark") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("GetTheme gives the player's look with every colour", "[triggers]") {
    Extensions::Extension* ext = start("theme_player", R"(
        function Init()
            local theme = GetTheme()
            _G.look, _G.dark, _G.page = theme.look, theme.dark, theme.page
        end
    )");
    REQUIRE(ext != nullptr);
    sol::state& lua = *ext->luaState;
    Controls::Theme theme = Controls::GetTheme();
    CHECK(lua["look"].get<std::string>() == Controls::LookName(theme.look));
    CHECK(lua["dark"].get<bool>() == theme.dark);
    CHECK(lua["page"].get<uint32_t>() == theme.page);
    TestExtension::unloadAll();
}
