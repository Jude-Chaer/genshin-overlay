#include <windows.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "extensions.hpp"
#include "keybindings.hpp"
#include "lua_functions.hpp"
#include "map_tiles.hpp"
#include "settings.hpp"
#include "test_extension.hpp"

using Extensions::ExtensionPermissionTypes;

// What Lua functions an extension calls do. The ones that draw or need the
// game aren't here, there's no window in the tests.

static Extensions::Extension* load(const std::string& name) {
    std::filesystem::remove(Settings::file);
    return TestExtension::load(name, "function Init() end");
}

static std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static void wait(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

TEST_CASE("JSON_Decode makes Lua tables") {
    Extensions::Extension* ext = load("json");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"(
        local chest = JSON_Decode('{"name":"chest","at":[1.5,2,3],"open":true,"inside":{"count":4},"owner":null}')
        ok = chest.name == "chest" and #chest.at == 3 and chest.at[1] == 1.5 and chest.at[3] == 3
            and chest.open == true and chest.inside.count == 4 and chest.owner == nil
        bad = JSON_Decode("{ this isn't json") == nil
    )"));
    CHECK((*ext->luaState)["ok"].get_or(false) == true);
    CHECK((*ext->luaState)["bad"].get_or(false) == true);
    TestExtension::unloadAll();
}

TEST_CASE("what JSON_Encode writes, JSON_Decode reads back the same") {
    Extensions::Extension* ext = load("json");
    REQUIRE(ext != nullptr);
    // with a quote and a line break in the text
    const char* lua = R"(
        local text = JSON_Encode({ list = { 1, 2, 3 }, name = "a \"b\"\nc", flag = false, deep = { x = 0.25 } })
        local back = JSON_Decode(text)
        ok = type(text) == "string" and #back.list == 3 and back.list[2] == 2 and back.name == "a \"b\"\nc"
            and back.flag == false and back.deep.x == 0.25
        -- a function can't be written as JSON
        bad = JSON_Encode({ run = print }) == nil
    )";
    REQUIRE(TestExtension::run(ext, lua));
    CHECK((*ext->luaState)["ok"].get_or(false) == true);
    CHECK((*ext->luaState)["bad"].get_or(false) == true);
    TestExtension::unloadAll();
}

TEST_CASE("JSON too deep or a table inside itself gives nil, not a crash") {
    Extensions::Extension* ext = load("json");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"(
        local me = {}
        me.me = me
        loop = JSON_Encode(me) == nil
        local list = { 1 }
        list[2] = list
        listLoop = JSON_Encode(list) == nil

        -- the same table twice side by side isn't a loop
        local shared = { x = 1 }
        twice = JSON_Encode({ a = shared, b = shared }) ~= nil

        local deep = {}
        local at = deep
        for i = 1, 200 do at.next = {}; at = at.next end
        tooDeep = JSON_Encode(deep) == nil
        local fine = {}
        at = fine
        for i = 1, 100 do at.next = {}; at = at.next end
        deepEnough = JSON_Encode(fine) ~= nil

        -- a server reply nested far down
        decodeDeep = JSON_Decode(string.rep("[", 100000) .. string.rep("]", 100000)) == nil
        decodeFine = JSON_Decode(string.rep("[", 100) .. string.rep("]", 100)) ~= nil
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["loop"].get_or(false) == true);
    CHECK(lua["listLoop"].get_or(false) == true);
    CHECK(lua["twice"].get_or(false) == true);
    CHECK(lua["tooDeep"].get_or(false) == true);
    CHECK(lua["deepEnough"].get_or(false) == true);
    CHECK(lua["decodeDeep"].get_or(false) == true);
    CHECK(lua["decodeFine"].get_or(false) == true);
    TestExtension::unloadAll();
}

TEST_CASE("an extension reads and writes files in its own folder") {
    Extensions::Extension* ext = load("files");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"(
        WriteFile("notes.txt", "one")
        AppendFile("notes.txt", " two")
        text = ReadFile("notes.txt")
        there = FileExists("notes.txt") and not FileExists("nothing.txt")
        names = {}
        for _, name in ipairs(ListDirectory(".")) do names[name] = true end
        listed = names["main.lua"] and names["notes.txt"]
        missing = not pcall(ReadFile, "nothing.txt")
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["text"].get_or(std::string()) == "one two");
    CHECK(lua["there"].get_or(false) == true);
    CHECK(lua["listed"].get_or(false) == true);
    CHECK(lua["missing"].get_or(false) == true);
    CHECK(read(ext->folder / "notes.txt") == "one two");
    // what it wrote is counted
    CHECK(ext->bytesToDiskWritten == 7);
    TestExtension::unloadAll();
}

TEST_CASE("files outside its folder are refused") {
    Extensions::Extension* ext = load("files");
    REQUIRE(ext != nullptr);
    // a folder next to it whose name starts the same
    std::filesystem::path other = TestExtension::folder("files_other");
    std::filesystem::create_directories(other);
    std::ofstream(other / "secret.txt", std::ios::binary) << "secret";
    std::filesystem::remove(other / "new.txt");

    // refused: nil or false, and the script goes on
    REQUIRE(TestExtension::run(ext, R"(
        refused = ReadFile("../files_other/secret.txt") == nil
            and FileExists("../files_other/secret.txt") == false
            and ListDirectory("..") == nil
            and WriteFile("../files_other/new.txt", "x") == false
            and AppendFile("../files_other/secret.txt", "x") == false
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["refused"].get_or(false) == true);
    CHECK(!std::filesystem::exists(other / "new.txt"));
    CHECK(read(other / "secret.txt") == "secret");
    TestExtension::unloadAll();
}

TEST_CASE("without the file permissions nothing is read or written") {
    Extensions::Extension* ext = load("files");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"( WriteFile("notes.txt", "one") )"));

    ext->extensionPermissions.erase(ExtensionPermissionTypes::InternalReadfile);
    ext->extensionPermissions.erase(ExtensionPermissionTypes::InternalWritefile);
    REQUIRE(TestExtension::run(ext, R"(
        refused = ReadFile("notes.txt") == nil
            and FileExists("notes.txt") == false
            and ListDirectory(".") == nil
            and WriteFile("notes.txt", "two") == false
            and AppendFile("notes.txt", "two") == false
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["refused"].get_or(false) == true);
    CHECK(read(ext->folder / "notes.txt") == "one");
    TestExtension::unloadAll();
}

TEST_CASE("an extension can't write more than the write limit") {
    Extensions::Extension* ext = load("files");
    REQUIRE(ext != nullptr);
    std::ofstream(ext->folder / "kept.txt", std::ios::binary) << "kept";
    ext->bytesToDiskWritten = Extensions::ExtensionWriteLimit - 3;
    REQUIRE(TestExtension::run(ext, R"(
        tooMuch = pcall(WriteFile, "big.txt", "four")
        fits = pcall(AppendFile, "small.txt", "abc")
        full = pcall(AppendFile, "small.txt", "d")
        overKept = pcall(WriteFile, "kept.txt", "too long")
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["tooMuch"].get_or(true) == false);
    CHECK(lua["fits"].get_or(false) == true);
    CHECK(lua["full"].get_or(true) == false);
    CHECK(read(ext->folder / "small.txt") == "abc");
    // refused before the file was opened, so it isn't emptied
    CHECK(lua["overKept"].get_or(true) == false);
    CHECK(read(ext->folder / "kept.txt") == "kept");
    CHECK(!std::filesystem::exists(ext->folder / "big.txt"));
    CHECK(ext->bytesToDiskWritten == Extensions::ExtensionWriteLimit);
    TestExtension::unloadAll();
}

TEST_CASE("without the network permission nothing is sent") {
    Extensions::Extension* ext = load("network");
    REQUIRE(ext != nullptr);
    ext->extensionPermissions.erase(ExtensionPermissionTypes::NetworkAccess);
    // nothing listens there, and it must not even be tried
    REQUIRE(TestExtension::run(ext, R"(
        local socket = WebSocket.new()
        refused = Net_Get("http://127.0.0.1:9/") == nil
            and Net_Post("http://127.0.0.1:9/", "data") == nil
            and socket:connect("ws://127.0.0.1:9/") == false
            and socket:send("data") == false
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["refused"].get_or(false) == true);
    TestExtension::unloadAll();
}

TEST_CASE("with the network permission this PC and the home network are still refused") {
    Extensions::Extension* ext = load("internet_only");
    REQUIRE(ext != nullptr);
    ext->extensionPermissions.insert(ExtensionPermissionTypes::NetworkAccess);
    std::ostringstream log;
    std::streambuf* before = std::cerr.rdbuf(log.rdbuf());
    // nothing listens on port 9, and it must not even be tried
    bool ran = TestExtension::run(ext, R"(
        local socket = WebSocket.new()
        refused = Net_Get("http://127.0.0.1:9/") == nil
            and Net_Get("http://192.168.1.1/") == nil
            and Net_Get("http://127.0.0.1:9/again") == nil
            and Net_Post("http://localhost:9/", "data") == nil
        -- a socket says so a frame later
        socket:connect("ws://[::1]:9/", { onClose = function(reason) _G.socketClosed = reason end })
    )");
    for (int i = 0; i < 200 && (*ext->luaState)["socketClosed"] == sol::lua_nil; i++) {
        TestExtension::turn();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cerr.rdbuf(before);
    REQUIRE(ran);
    CHECK((*ext->luaState)["refused"].get_or(false) == true);
    CHECK((*ext->luaState)["socketClosed"].get_or(std::string()) == "refused");
    std::string text = log.str();
    INFO(text);
    const std::string why = ": extensions reach only the internet, not this PC or the home network";
    CHECK(text.find("[internet_only] Net_Get can't reach 127.0.0.1 (127.0.0.1)" + why) != std::string::npos);
    CHECK(text.find("[internet_only] Net_Get can't reach 192.168.1.1 (192.168.1.1)" + why) != std::string::npos);
    CHECK(text.find("[internet_only] Net_Post can't reach localhost") != std::string::npos);
    CHECK(text.find("[internet_only] WebSocket.connect can't reach [::1]") != std::string::npos);
    // one line per place
    CHECK(text.find("127.0.0.1 (127.0.0.1)") == text.rfind("127.0.0.1 (127.0.0.1)"));
    TestExtension::unloadAll();
}

TEST_CASE("SetInterval runs a function again and again until it's cleared") {
    Extensions::Extension* ext = load("intervals");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"(
        DefineExtensionSetting("count", "Count", "int", 5)
        ticks = 0
        -- GetSetting only works when the function runs as this extension
        id = SetInterval(function() ticks = ticks + 1; seen = GetSetting("count") end, 1)
        none = SetInterval(function() end, 0)
    )"));
    sol::state& lua = *ext->luaState;
    int id = lua["id"].get_or(0);
    CHECK(id != 0);
    CHECK(lua["none"].get_or(-1) == 0);

    LuaFunctions::UpdateIntervals();
    CHECK(lua["ticks"].get_or(-1) == 0);
    wait(20);
    LuaFunctions::UpdateIntervals();
    CHECK(lua["ticks"].get_or(-1) == 1);
    CHECK(lua["seen"].get_or(0) == 5);
    wait(20);
    LuaFunctions::UpdateIntervals();
    CHECK(lua["ticks"].get_or(-1) == 2);

    // another extension can't clear it
    Extensions::Extension* stranger = TestExtension::load("intervals_other", "function Init() end");
    REQUIRE(stranger != nullptr);
    REQUIRE(TestExtension::run(stranger, "ClearInterval(" + std::to_string(id) + ")"));
    wait(20);
    LuaFunctions::UpdateIntervals();
    CHECK(lua["ticks"].get_or(-1) == 3);

    REQUIRE(TestExtension::run(ext, "ClearInterval(id)"));
    wait(20);
    LuaFunctions::UpdateIntervals();
    CHECK(lua["ticks"].get_or(-1) == 3);
    TestExtension::unloadAll();
}

TEST_CASE("a key bound from Lua runs its function as that extension") {
    Keybindings::keybinds.clear();
    Extensions::Extension* ext = load("keys");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"(
        DefineExtensionSetting("count", "Count", "int", 5)
        f5 = RegisterKeybind(Keys.F5, function() pressed = GetSetting("count") end, KeybindFlags.None)
        RegisterKeybind(Keys.G, function() end, KeybindFlags.ProcessWhileHidden, KeybindModifiers.Ctrl + KeybindModifiers.Shift)
    )"));

    Keybindings::Keybind* f5 = Keybindings::DoesKeybindExist(GLFW_KEY_F5);
    REQUIRE(f5 != nullptr);
    CHECK(!f5->HasFlag(Keybindings::KeybindFlags_ProcessWhileHidden));
    CHECK(f5->GetModifiers() == Keybindings::KeybindModifiers_None);
    f5->Press();
    Keybindings::ProcessKeybindings();
    CHECK((*ext->luaState)["pressed"].get_or(0) == 5);

    // looked up with its modifiers, plain G is another key
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_G) == nullptr);
    Keybindings::Keybind* g = Keybindings::DoesKeybindExist(GLFW_KEY_G,
        (Keybindings::KeybindModifiers)(Keybindings::KeybindModifiers_Ctrl | Keybindings::KeybindModifiers_Shift));
    REQUIRE(g != nullptr);
    CHECK(g->HasFlag(Keybindings::KeybindFlags_ProcessWhileHidden));
    CHECK(g->GetModifiers() == (Keybindings::KeybindModifiers_Ctrl | Keybindings::KeybindModifiers_Shift));

    REQUIRE(TestExtension::run(ext, "DeleteKeybind(f5)"));
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5) == nullptr);
    TestExtension::unloadAll();
}

TEST_CASE("every key in Keys can be bound") {
    Extensions::Extension* ext = load("keys");
    REQUIRE(ext != nullptr);
    int count = 0;
    {
        // let go of before its Lua state closes
        sol::table keys = (*ext->luaState)["Keys"];
        for (const auto& [name, key] : keys) {
            INFO("Keys." << name.as<std::string>());
            CHECK(Keybindings::GlfwKeyToVirtualKey(key.as<int>()) != 0);
            count++;
        }
    }
    // A to Z, Num0 to Num9, F1 to F12 and six more
    CHECK(count == 54);
    TestExtension::unloadAll();
}

TEST_CASE("GetMapTile only takes tiles on the map's grid at the sizes drawn") {
    MapTiles::Grid grid;
    grid.cols = 10;
    grid.rows = 8;
    CHECK(MapTiles::IsTile(grid, 0, 0, 256));
    CHECK(MapTiles::IsTile(grid, 9, 7, 32));
    for (int size : { 64, 128 }) CHECK(MapTiles::IsTile(grid, 4, 4, size));
    // off the grid: each would be a download and a cache file
    CHECK_FALSE(MapTiles::IsTile(grid, 10, 0, 256));
    CHECK_FALSE(MapTiles::IsTile(grid, 0, 8, 256));
    CHECK_FALSE(MapTiles::IsTile(grid, -1, 0, 256));
    CHECK_FALSE(MapTiles::IsTile(grid, 0, -1, 256));
    // a size nobody draws, the biggest a resize to gigabytes
    for (int size : { 0, -32, 100, 512, 1 << 20 }) CHECK_FALSE(MapTiles::IsTile(grid, 0, 0, size));
}

TEST_CASE("GetMousePosition sees the cursor only over the game, unless it has Mouse anywhere") {
    using LuaFunctions::mouseFor;
    // the game on the second screen, the overlay over it
    RECT game = { 1920, 0, 3840, 1080 };
    POINT overlay = { 1920, 0 };

    CHECK(mouseFor({ 2000, 500 }, game, overlay, false) == std::make_pair(80, 500));
    CHECK(mouseFor({ 1920, 0 }, game, overlay, false) == std::make_pair(0, 0));
    // on the desktop next to it, past its last pixel, or with no game at all
    CHECK_FALSE(mouseFor({ 100, 500 }, game, overlay, false));
    CHECK_FALSE(mouseFor({ 3840, 500 }, game, overlay, false));
    CHECK_FALSE(mouseFor({ 2000, 500 }, std::nullopt, overlay, false));

    CHECK(mouseFor({ 100, 500 }, game, overlay, true) == std::make_pair(-1820, 500));
    CHECK(mouseFor({ 100, 500 }, std::nullopt, overlay, true) == std::make_pair(-1820, 500));
}
