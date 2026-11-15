#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <set>
#include <string>
#include <sol/sol.hpp>

// The Markers extension's saved sets (extensions/markers/selection.lua): a set
// is on or off only by its own switch, what's on the map is the sets that are
// on plus the types picked by hand. Type ids are HoYoLAB's: 17 Common Chest,
// 44 Exquisite Chest, 18 Seelie, 70 Elemental Monument, 35 Cecilia.

namespace fs = std::filesystem;

namespace {
    const fs::path MARKERS = fs::path(__FILE__).parent_path().parent_path() / "extensions" / "markers";

    struct Markers {
        sol::state lua;
        Markers() {
            lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::package, sol::lib::math);
            lua["package"]["path"] = (MARKERS / "?.lua").string();
            // the item files and the overlay's calls, only what selection.lua uses
            lua.script(R"(
                package.preload.catalog = function()
                    local types = {}
                    for _, id in ipairs({ 17, 44, 18, 70, 35 }) do types[#types + 1] = { id = id } end
                    return { types = types, typeOf = {}, regionAt = function() return nil end, on = function() return nil end }
                end
                package.preload.collected = function() return { has = function() return false end } end
                FileExists = function() return false end
                WriteFile = function() end
                JSON_Encode = function() return "" end
                GetSetting = function() return false end
                selection = require("selection")
                selection.load()
                selection.ready()
                selection.clear()
                function newSet(name, ids)
                    local types = {}
                    for _, id in ipairs(ids) do types[id] = true end
                    selection.sets[#selection.sets + 1] = { name = name, types = types, on = false }
                end
            )");
        }
        void run(const std::string& code) { lua.script(code); }
        std::set<int> onMap() {
            std::set<int> out;
            sol::table on = lua["selection"]["on"];
            for (auto& [key, value] : on) out.insert(key.as<int>());
            return out;
        }
        bool setOn(int index) { return lua["selection"]["sets"][index]["on"]; }
    };
}

TEST_CASE("sets that share a type stay on and off by themselves") {
    Markers m;
    m.run("newSet('Chests and puzzles', { 17, 44, 18 }) newSet('Puzzles', { 18, 70 })");
    m.run("selection.setOn(1, true) selection.setOn(2, true)");
    CHECK(m.onMap() == std::set<int>{ 17, 44, 18, 70 });
    // off: Seelie stays, the other set still has it
    m.run("selection.setOn(1, false)");
    CHECK(m.onMap() == std::set<int>{ 18, 70 });
    CHECK_FALSE(m.setOn(1));
    CHECK(m.setOn(2));
    m.run("selection.setOn(2, false)");
    CHECK(m.onMap().empty());
}

TEST_CASE("picking a set's types by hand doesn't turn the set on") {
    Markers m;
    m.run("newSet('Chests', { 17, 44 }) selection.pick({ 17, 44 }, true)");
    CHECK(m.onMap() == std::set<int>{ 17, 44 });
    CHECK_FALSE(m.setOn(1));
}

TEST_CASE("a type picked by hand stays when its set goes off") {
    Markers m;
    m.run("newSet('Chests', { 17, 44 }) selection.pick({ 17 }, true) selection.setOn(1, true) selection.setOn(1, false)");
    CHECK(m.onMap() == std::set<int>{ 17 });
}

TEST_CASE("a type taken off by hand stays off while its set is on, and comes back with the set") {
    Markers m;
    m.run("newSet('Chests', { 17, 44 }) selection.setOn(1, true) selection.pick({ 44 }, false)");
    CHECK(m.onMap() == std::set<int>{ 17 });
    CHECK(m.setOn(1));
    m.run("selection.setOn(1, false) selection.setOn(1, true)");
    CHECK(m.onMap() == std::set<int>{ 17, 44 });
}

TEST_CASE("all sets off leaves only what was picked by hand") {
    Markers m;
    m.run("newSet('Chests', { 17, 44 }) newSet('Puzzles', { 18, 70 }) selection.pick({ 35 }, true)");
    m.run("selection.setOn(1, true) selection.setOn(2, true) selection.allSetsOff()");
    CHECK(m.onMap() == std::set<int>{ 35 });
    CHECK_FALSE(m.setOn(1));
    CHECK_FALSE(m.setOn(2));
}

TEST_CASE("a saved set takes what's on the map, and turning it off takes it away") {
    Markers m;
    m.run("selection.pick({ 17, 35 }, true) selection.saveSet('Set 1')");
    CHECK(m.setOn(1));
    CHECK(m.onMap() == std::set<int>{ 17, 35 });
    m.run("selection.setOn(1, false)");
    CHECK(m.onMap().empty());
}

TEST_CASE("deleting a set that's on takes its types off the map") {
    Markers m;
    m.run("newSet('Chests', { 17, 44 }) selection.pick({ 35 }, true) selection.setOn(1, true) selection.deleteSet(1)");
    CHECK(m.onMap() == std::set<int>{ 35 });
    CHECK(m.lua["selection"]["sets"].get<sol::table>().size() == 0);
}

TEST_CASE("a set's icon is one of its own types, and is kept") {
    Markers m;
    m.run("newSet('Chests', { 17, 44 }) selection.setIcon(1, 44) selection.setIcon(1, 18)");
    CHECK(m.lua["selection"]["sets"][1]["icon"].get<int>() == 44);
}
