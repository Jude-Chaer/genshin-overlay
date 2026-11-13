#include <windows.h>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <sol/sol.hpp>
#include "item_catalog.hpp"
#include "item_files.hpp"
#include "item_icons.hpp"
#include "extensions.hpp"
#include "lua_functions.hpp"

// What extensions see of the map items, on the test pack (answers in
// tests/data/items/answers.md, as in item_catalog_tests.cpp).

namespace fs = std::filesystem;

namespace {
    const fs::path FOLDER = fs::path(__FILE__).parent_path() / "data" / "items" / "folder";

    std::shared_ptr<ItemCatalog::Catalog> loadFolder() {
        std::string raw;
        if (!ItemFiles::pack(FOLDER, raw)) return nullptr;
        return ItemCatalog::load(raw);
    }

    // a Lua state with every function an extension gets, run as an extension with the
    // default permissions, and the test pack published and taken for the frame
    struct Lua {
        sol::state lua;
        Extensions::Extension ext;
        Extensions::Extension* previous = Extensions::currentExtension;
        Lua() {
            ext.name = "item tests";
            ext.extensionPermissions = Extensions::defaultPermissions;
            Extensions::currentExtension = &ext;
            lua.open_libraries(sol::lib::base, sol::lib::table);
            LuaFunctions::registerLuaFunctions(lua);
            auto catalog = loadFolder();
            REQUIRE(catalog);
            ItemCatalog::publish(catalog);
            LuaFunctions::Items::beginFrame();
        }
        ~Lua() {
            Extensions::currentExtension = previous;
        }
        template <typename T> T run(const std::string& code) {
            auto result = lua.safe_script("return " + code, sol::script_pass_on_error);
            REQUIRE(result.valid());
            return result.get<T>();
        }
    };

    // stands in for GL: every texture gets the next number, freed ones are written down
    struct FakeTextures {
        GLuint next = 100;
        int made = 0;
        std::vector<GLuint> freed;
        std::function<GLuint(std::string_view)> realMake = ItemIcons::make;
        std::function<void(GLuint)> realRelease = ItemIcons::release;
        FakeTextures() {
            ItemIcons::clear();
            ItemIcons::make = [this](std::string_view png) { made++; return png.empty() ? 0 : next++; };
            ItemIcons::release = [this](GLuint texture) { freed.push_back(texture); };
        }
        ~FakeTextures() {
            ItemIcons::clear();
            ItemIcons::make = realMake;
            ItemIcons::release = realRelease;
        }
    };
}

TEST_CASE("extensions get the type list from the catalog in memory") {
    Lua lua;
    CHECK(lua.run<uint64_t>("GetItemsVersion()") == ItemCatalog::version());
    CHECK(lua.run<int>("#GetItemTypes()") == 4);

    lua.lua.script("groups = GetItemTypes()");
    CHECK(lua.run<int>("groups[2].id") == 186);
    CHECK(lua.run<std::string>("groups[2].name") == "Puzzle Chest");
    CHECK(lua.run<int>("#groups[2].types") == 2);
    CHECK(lua.run<int>("groups[2].types[1].id") == 509);
    CHECK(lua.run<std::string>("groups[2].types[1].name") == "Fontaine Shrine of Depths");
    CHECK(lua.run<int>("groups[2].types[1].counts[2]") == 8);
    CHECK(lua.run<int>("groups[2].types[1].counts[34]") == 2);
    CHECK(lua.run<bool>("groups[2].types[1].counts[9] == nil"));
    // a type with no points has no counts
    CHECK(lua.run<int>("groups[1].types[1].id") == 589);
    CHECK(lua.run<bool>("next(groups[1].types[1].counts) == nil"));
}

TEST_CASE("extensions get a type's points with every field") {
    Lua lua;
    lua.lua.script("points = GetItemPositions(231)");
    REQUIRE(lua.run<int>("#points") == 6);
    CHECK(lua.run<int>("points[1].id") == 47783);
    CHECK(lua.run<int>("points[1].map") == 2);
    CHECK(lua.run<double>("points[1].x") == 3008.75);
    CHECK(lua.run<double>("points[1].y") == 6754.75);
    CHECK(lua.run<int>("points[1].area") == 3);
    CHECK(lua.run<int>("points[1].group") == 84);
    CHECK(lua.run<int>("points[1].floor") == 9);
    CHECK(lua.run<int>("points[6].id") == 33158);
    CHECK(lua.run<int>("points[6].map") == 9);
    CHECK(lua.run<int>("points[6].group") == 0);

    // in the tree with no points: an empty list, not nil
    CHECK(lua.run<bool>("type(GetItemPositions(589)) == 'table' and #GetItemPositions(589) == 0"));
}

TEST_CASE("extensions get the regions with their boxes") {
    Lua lua;
    CHECK(lua.run<int>("#GetItemRegions()") == 14);
    lua.lua.script(R"(
        for _, region in ipairs(GetItemRegions()) do
            if region.id == 1 then mondstadt = region end
        end
    )");
    CHECK(lua.run<std::string>("mondstadt.name") == "Mondstadt");
    CHECK(lua.run<int>("mondstadt.map") == 2);
    CHECK(lua.run<double>("mondstadt.box.left") == -1031);
    CHECK(lua.run<double>("mondstadt.box.top") == -688);
    CHECK(lua.run<double>("mondstadt.box.right") == 1195);
    CHECK(lua.run<double>("mondstadt.box.bottom") == 1069);
}

TEST_CASE("unknown ids give nil to extensions, never an error") {
    FakeTextures textures;
    Lua lua;
    CHECK(lua.run<bool>("GetItemPositions(999999) == nil"));
    CHECK(lua.run<bool>("GetItemIcon(999999) == nil"));
    // a type that has no icon
    CHECK(lua.run<bool>("GetItemIcon(189) == nil"));
}

TEST_CASE("with no catalog yet the lists are empty and there are no icons") {
    sol::state lua;
    CHECK(LuaFunctions::Items::types(nullptr, lua).size() == 0);
    CHECK(LuaFunctions::Items::regions(nullptr, lua).size() == 0);
    CHECK(LuaFunctions::Items::positions(nullptr, 231, lua) == sol::lua_nil);
    CHECK(ItemIcons::get(nullptr, 231) == 0);
}

TEST_CASE("the download state reads as words") {
    sol::state lua;
    ItemFiles::Status status;
    status.state = ItemFiles::State::Old;
    status.reason = ItemFiles::Reason::NoNetwork;
    status.built = "2026-10-17";
    sol::table old = LuaFunctions::Items::state(status, lua);
    CHECK(old.get<std::string>("state") == "old");
    CHECK(old.get<std::string>("reason") == "no_network");
    CHECK(old.get<std::string>("built") == "2026-10-17");

    status = {};
    status.state = ItemFiles::State::Downloading;
    status.percent = 40;
    status.started = true;
    sol::table downloading = LuaFunctions::Items::state(status, lua);
    CHECK(downloading.get<std::string>("state") == "downloading");
    CHECK(downloading.get<std::string>("reason") == "none");
    CHECK(downloading.get<int>("percent") == 40);

    CHECK_FALSE(downloading.get<bool>("checking"));
    status.checking = true;
    CHECK(LuaFunctions::Items::state(status, lua).get<bool>("checking"));

    // nothing started in the tests: none, and still waiting on the first check
    Lua withFunctions;
    CHECK(withFunctions.run<std::string>("GetItemsState().state") == "none");
    CHECK(withFunctions.run<bool>("GetItemsState().checking"));
}

TEST_CASE("an icon is made once and freed at the first frame of a new catalog") {
    FakeTextures textures;
    Lua lua;
    GLuint first = lua.run<GLuint>("GetItemIcon(231)");
    CHECK(first != 0);
    CHECK(lua.run<GLuint>("GetItemIcon(231)") == first);
    CHECK(lua.run<GLuint>("GetItemIcon(239)") != first);
    CHECK(textures.made == 2);
    CHECK(ItemIcons::count() == 2);
    CHECK(ItemIcons::owns(first));
    CHECK(textures.freed.empty());

    // a new catalog mid frame changes nothing until the next frame starts
    uint64_t version = lua.run<uint64_t>("GetItemsVersion()");
    ItemCatalog::publish(loadFolder());
    CHECK(lua.run<uint64_t>("GetItemsVersion()") == version);
    CHECK(lua.run<GLuint>("GetItemIcon(231)") == first);
    CHECK(textures.freed.empty());

    // the next frame frees the old icons, even with nobody asking for one
    LuaFunctions::Items::beginFrame();
    CHECK(lua.run<uint64_t>("GetItemsVersion()") == version + 1);
    CHECK(textures.freed.size() == 2);
    CHECK(ItemIcons::count() == 0);
    CHECK(!ItemIcons::owns(first));
    GLuint again = lua.run<GLuint>("GetItemIcon(231)");
    CHECK(again != first);
    CHECK(textures.made == 3);
    CHECK(ItemIcons::count() == 1);
}
