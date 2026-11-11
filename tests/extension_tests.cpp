#include <chrono>
#include <windows.h>
#include <GLFW/glfw3.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include "controls.hpp"
#include "extensions.hpp"
#include "files.hpp"
#include "helpers.hpp"
#include "keybindings.hpp"
#include "lua_functions.hpp"
#include "settings.hpp"
#include "test_extension.hpp"

// What an extension can do and what it leaves behind: every function checks its
// permission, grants are kept per folder, and nothing it registered outlives it.

namespace fs = std::filesystem;
using Permission = Extensions::ExtensionPermissionTypes;
using Catch::Approx;

namespace {
    const fs::path DATA = fs::path(__FILE__).parent_path() / "data";

    // what's written to std::cerr while it's alive
    struct CaptureLog {
        std::ostringstream text;
        std::streambuf* previous = std::cerr.rdbuf(text.rdbuf());
        ~CaptureLog() { std::cerr.rdbuf(previous); }
        int count(const std::string& part) const {
            std::string all = text.str();
            int n = 0;
            for (size_t at = all.find(part); at != std::string::npos; at = all.find(part, at + 1)) n++;
            return n;
        }
    };

    // runs as ext until it goes out of scope
    struct RunAs {
        Extensions::Extension* previous = Extensions::currentExtension;
        RunAs(Extensions::Extension& ext) { Extensions::currentExtension = &ext; }
        ~RunAs() { Extensions::currentExtension = previous; }
    };

    std::set<std::string> globalNames(sol::state& lua) {
        std::set<std::string> names;
        for (auto& [key, value] : lua.globals()) {
            if (key.is<std::string>()) names.insert(key.as<std::string>());
        }
        return names;
    }

    void writeSettings(const std::string& text) {
        std::ofstream(Settings::file, std::ios::binary | std::ios::trunc) << text;
    }

    nlohmann::json readSettings() {
        return nlohmann::json::parse(Files::readText(Settings::file));
    }
}

TEST_CASE("every function an extension gets goes through the permission check") {
    auto lua = Extensions::newLuaState(DATA / "extensions" / "registers_in_init");

    // what the Lua libraries bring on their own
    sol::state plain;
    plain.open_libraries(sol::lib::base, sol::lib::package, sol::lib::math, sol::lib::string, sol::lib::table);
    std::set<std::string> fromLibraries = globalNames(plain);

    sol::table bound = lua->registry()[LuaFunctions::BOUND_TABLE];
    int globalsChecked = 0;
    for (const std::string& name : globalNames(*lua)) {
        if (fromLibraries.count(name)) continue;
        sol::object value = (*lua)[name];
        if (value.get_type() == sol::type::function) {
            INFO(name);
            CHECK(value == bound[name].get<sol::object>());
            globalsChecked++;
        }
        else if (value.get_type() == sol::type::table && name != "WebSocket") {
            // KeybindFlags, Keys and the like hold numbers only
            for (auto& [key, inner] : value.as<sol::table>()) {
                INFO(name);
                CHECK(inner.get_type() != sol::type::function);
            }
        }
    }

    // the socket's methods, as WebSocket.connect and so on
    lua->set("bound", bound);
    lua->script(R"(
        methods = {}
        local seen = {}
        local function look(t)
            if type(t) ~= "table" or seen[t] then return end
            seen[t] = true
            for k, v in next, t do
                if type(k) == "string" and type(v) == "function" and k:sub(1, 2) ~= "__" and k ~= "new" then
                    methods[k] = v
                end
            end
            local meta = getmetatable(t)
            if type(meta) == "table" then
                look(meta)
                look(rawget(meta, "__index"))
            end
        end
        look(WebSocket)
        look(getmetatable(WebSocket.new()))
    )");
    int methodsChecked = 0;
    sol::table methods = (*lua)["methods"];
    for (auto& [key, value] : methods) {
        std::string name = "WebSocket." + key.as<std::string>();
        INFO(name);
        CHECK(value == bound[name].get<sol::object>());
        methodsChecked++;
    }

    // everything in the list was found, nothing was missed
    int globals = 0, socketMethods = 0;
    for (const auto& function : LuaFunctions::boundFunctions()) {
        (function.name.rfind("WebSocket.", 0) == 0 ? socketMethods : globals)++;
    }
    CHECK(globalsChecked == globals);
    CHECK(methodsChecked == socketMethods);
    CHECK(socketMethods == 6);

    // a few by what they need
    auto needs = [](const std::string& name) {
        for (const auto& function : LuaFunctions::boundFunctions()) {
            if (function.name == name) return function.permission;
        }
        FAIL("not bound: " << name);
        return Permission::None;
    };
    CHECK(needs("Net_Get") == Permission::NetworkAccess);
    CHECK(needs("WebSocket.connect") == Permission::NetworkAccess);
    CHECK(needs("WebSocket.close") == Permission::None);
    CHECK(needs("RegisterKeybind") == Permission::Keybinds);
    CHECK(needs("DrawText") == Permission::DisplayOverlay);
    CHECK(needs("GetItemTypes") == Permission::DisplayRead);
    CHECK(needs("ReadFile") == Permission::InternalReadfile);
    CHECK(needs("WriteFile") == Permission::InternalWritefile);
    CHECK(needs("GetSetting") == Permission::None);
}

TEST_CASE("a call without its permission gives nil or false, and is logged once") {
    auto lua = Extensions::newLuaState(DATA / "extensions" / "registers_in_init");
    Extensions::Extension ext;
    ext.name = "nothing allowed";
    ext.folder = DATA / "extensions" / "registers_in_init";
    RunAs runAs(ext);
    CaptureLog log;

    lua->script(R"(
        got = Net_Get("http://127.0.0.1:9/")
        again = Net_Get("http://127.0.0.1:9/")
        exists = FileExists("main.lua")
        key = RegisterKeybind(Keys.F9, function() end, KeybindFlags.None)
        DrawText("hi", 0, 0)
        local socket = WebSocket.new()
        connected = socket:isConnected()
        socket:close()
        done = true
    )");
    CHECK(lua->script("return got == nil and again == nil").get<bool>());
    CHECK((*lua)["exists"].get<bool>() == false);
    CHECK(lua->script("return key == nil").get<bool>());
    CHECK((*lua)["connected"].get<bool>() == false);
    CHECK((*lua)["done"].get<bool>());
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F9) == nullptr);

    CHECK(log.count("[nothing allowed] Net_Get needs Network Access, which it isn't allowed") == 1);
    CHECK(log.count("WebSocket.isConnected needs Network Access") == 1);
    CHECK(log.count("RegisterKeybind needs Keybinds") == 1);
    CHECK(log.count("DrawText needs Display Overlay") == 1);
    CHECK(log.count("FileExists needs Internal File Reading") == 1);
    CHECK(log.count("close") == 0);
}

TEST_CASE("an extension can only delete its own keybinds") {
    Keybindings::keybinds.clear();
    auto lua = Extensions::newLuaState(DATA / "extensions" / "registers_in_init");
    Extensions::Extension first, second;
    first.name = "first";
    second.name = "second";
    first.extensionPermissions = second.extensionPermissions = Extensions::defaultPermissions;

    std::string id;
    {
        RunAs runAs(first);
        id = lua->script("return RegisterKeybind(Keys.F9, function() end, KeybindFlags.None)").get<std::string>();
    }
    REQUIRE_FALSE(id.empty());
    CHECK(first.keybinds == std::vector<std::string>{ id });
    {
        RunAs runAs(second);
        sol::function deleteKeybind = (*lua)["DeleteKeybind"];
        CHECK(deleteKeybind(id).get<bool>() == false);
    }
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F9) != nullptr);

    // and it goes when its extension does
    Extensions::unload(first);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F9) == nullptr);
}

TEST_CASE("an extension that fails while loading leaves nothing behind") {
    fs::remove(Settings::file);
    Keybindings::keybinds.clear();
    LuaFunctions::intervals.clear();
    Extensions::registeredExtensions.clear();
    fs::path folder = DATA / "extensions" / "errors_after_register";

    CaptureLog log;
    Extensions::loadLuaExtension(folder / "main.lua", folder);

    CHECK(log.count("stops here on purpose") == 1);
    CHECK(Extensions::registeredExtensions.empty());
    CHECK(Keybindings::keybinds.empty());
    CHECK(LuaFunctions::intervals.empty());
    CHECK(Extensions::currentExtension == nullptr);
}

TEST_CASE("an extension that shuts down leaves nothing behind") {
    fs::remove(Settings::file);
    Keybindings::keybinds.clear();
    LuaFunctions::intervals.clear();
    Extensions::registeredExtensions.clear();
    fs::path folder = DATA / "extensions" / "registers_in_init";

    Extensions::loadLuaExtension(folder / "main.lua", folder);
    REQUIRE(Extensions::registeredExtensions.size() == 1);
    Extensions::Extension* ext = Extensions::registeredExtensions[0].get();
    CHECK(ext->name == "registers in init");
    Extensions::runForExtension(ext, ext->initializeFunction);

    // what Init made is there
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F8) != nullptr);
    REQUIRE(LuaFunctions::intervals.size() == 1);
    CHECK(LuaFunctions::intervals[0].owner == ext);

    Extensions::destroyExtensions();
    CHECK(Extensions::registeredExtensions.empty());
    CHECK(Keybindings::keybinds.empty());
    CHECK(LuaFunctions::intervals.empty());
}

TEST_CASE("an old by-hash permission list is moved to the extension's folder") {
    // a real settings.json from before, its one list is the example extension's
    fs::copy_file(DATA / "settings" / "old_permissions.json", Settings::file, fs::copy_options::overwrite_existing);
    const std::string hash = readSettings()["extensionPermissions"].begin().key();

    CaptureLog log;
    Extensions::Extension ext;
    ext.name = ext.folderKey = "example";
    ext.extensionHash = hash;
    Extensions::loadExtensionPermissions(&ext);

    std::vector<std::string> names = { "display_overlay", "display_read", "keybinds", "internal_write", "network", "internal_read" };
    nlohmann::json saved = readSettings();
    CHECK(saved["extensionGrants"]["example"]["hash"] == hash);
    CHECK(saved["extensionGrants"]["example"]["granted"] == names);
    CHECK_FALSE(saved.contains("extensionPermissions"));
    // nothing else in the file was touched
    CHECK(saved["extensions"]["map"]["opacity"] == 0.5);
    CHECK(saved["zoomFromKnob"] == true);

    CHECK(ext.extensionPermissions == std::unordered_set<Permission>{
        Permission::DisplayOverlay, Permission::DisplayRead, Permission::Keybinds,
        Permission::InternalWritefile, Permission::NetworkAccess, Permission::InternalReadfile });
    CHECK(ext.grantsBeforeChange.empty());

    // the next start finds it by folder
    Extensions::loadExtensionPermissions(&ext);
    CHECK(ext.extensionPermissions.count(Permission::NetworkAccess) == 1);
    CHECK(readSettings() == saved);
}

TEST_CASE("saved grants come back by name, with ones this overlay doesn't know kept") {
    // a newer overlay saved "game_traffic", which this one hasn't heard of
    writeSettings(R"({ "extensionGrants": { "markers": { "hash": "h1", "granted": ["keybinds", "game_traffic", "network"] } } })");
    Extensions::Extension ext;
    ext.name = ext.folderKey = "markers";
    ext.extensionHash = "h1";
    Extensions::loadExtensionPermissions(&ext);

    CHECK(ext.extensionPermissions == std::unordered_set<Permission>{ Permission::Keybinds, Permission::NetworkAccess });
    CHECK(ext.unknownGrants == std::vector<std::string>{ "game_traffic" });

    // the player takes Network Access away
    ext.extensionPermissions.erase(Permission::NetworkAccess);
    Extensions::saveExtensionPermissions(ext);
    CHECK(readSettings()["extensionGrants"]["markers"]["granted"] == std::vector<std::string>{ "keybinds", "game_traffic" });

    Extensions::loadExtensionPermissions(&ext);
    CHECK(ext.extensionPermissions == std::unordered_set<Permission>{ Permission::Keybinds });
    CHECK(ext.unknownGrants == std::vector<std::string>{ "game_traffic" });
}

TEST_CASE("saved grants for files outside the folder are dropped") {
    writeSettings(R"({ "extensionGrants": { "markers": { "hash": "h1", "granted": ["external_write", "network", "external_read"] } } })");
    Extensions::Extension ext;
    ext.name = ext.folderKey = "markers";
    ext.extensionHash = "h1";
    Extensions::loadExtensionPermissions(&ext);
    CHECK(ext.extensionPermissions == std::unordered_set<Permission>{ Permission::NetworkAccess });
    CHECK(ext.unknownGrants.empty());

    // nor asked for again after a change
    ext.extensionHash = "h2";
    Extensions::loadExtensionPermissions(&ext);
    CHECK(ext.grantsBeforeChange == std::vector<std::string>{ "network" });
}

TEST_CASE("an extension whose files changed runs with the defaults it still had") {
    // the player took file writing away, and gave it the internet
    writeSettings(R"({ "extensionGrants": { "markers": { "hash": "h1", "granted": ["keybinds", "network",
        "display_overlay", "display_read", "internal_read"] } } })");
    Extensions::Extension ext;
    ext.name = ext.folderKey = "markers";
    ext.extensionHash = "h2";
    Extensions::loadExtensionPermissions(&ext);

    CHECK(ext.extensionPermissions == std::unordered_set<Permission>{
        Permission::Keybinds, Permission::DisplayOverlay, Permission::DisplayRead, Permission::InternalReadfile });
    CHECK(ext.grantsBeforeChange == std::vector<std::string>{ "keybinds", "network",
        "display_overlay", "display_read", "internal_read" });
    CHECK(ext.hashBeforeChange == "h1");
    // what was allowed is still saved, for asking to allow it again
    CHECK(readSettings()["extensionGrants"]["markers"]["hash"] == "h1");
}

TEST_CASE("a new extension gets the defaults, without the internet") {
    fs::remove(Settings::file);
    Extensions::Extension ext;
    ext.name = ext.folderKey = "new one";
    ext.extensionHash = "h1";
    Extensions::loadExtensionPermissions(&ext);

    CHECK(ext.extensionPermissions == Extensions::defaultPermissions);
    CHECK(ext.extensionPermissions.count(Permission::NetworkAccess) == 0);
    CHECK(ext.grantsBeforeChange.empty());
}

TEST_CASE("pathFor keeps an extension in its folder") {
    using Extensions::Access;
    using Extensions::pathFor;
    fs::path root = fs::weakly_canonical(fs::temp_directory_path() / "genshin-overlay-tests" / "paths");
    fs::remove_all(root);
    fs::create_directories(root / "map" / "sub");
    fs::create_directories(root / "map2");

    Extensions::Extension ext;
    ext.name = "map";
    ext.folder = root / "map";
    ext.extensionPermissions = { Permission::InternalReadfile };
    CaptureLog log;

    CHECK(pathFor(ext, "notes.txt", Access::Read) == root / "map" / "notes.txt");
    CHECK(pathFor(ext, "sub/../notes.txt", Access::Read) == root / "map" / "notes.txt");
    CHECK(pathFor(ext, "../map/notes.txt", Access::Read) == root / "map" / "notes.txt");
    CHECK(pathFor(ext, "", Access::Read) == root / "map");
    CHECK(pathFor(ext, Helpers::utf8(root / "map" / "notes.txt"), Access::Read) == root / "map" / "notes.txt");
    // a folder whose name starts the same isn't its own
    CHECK_FALSE(pathFor(ext, "../map2/notes.txt", Access::Read));
    CHECK_FALSE(pathFor(ext, "..", Access::Read));
    CHECK_FALSE(pathFor(ext, "sub/../../map2/notes.txt", Access::Read));
    CHECK_FALSE(pathFor(ext, Helpers::utf8(root / "map2" / "notes.txt"), Access::Read));
    // reading isn't writing
    CHECK_FALSE(pathFor(ext, "notes.txt", Access::Write));
    CHECK(log.count("is outside its folder") == 1);
    CHECK(log.count("needs Internal File Writing") == 1);

    // with every file permission it has, outside is still outside
    ext.extensionPermissions.insert(Permission::InternalWritefile);
    CHECK_FALSE(pathFor(ext, "../map2/notes.txt", Access::Write));
    CHECK_FALSE(pathFor(ext, "../genshin-overlay.exe", Access::Write));

    // not a path at all
    CHECK_FALSE(pathFor(ext, "\xff", Access::Read));
    CHECK(log.count("can't use the path") == 1);
}

TEST_CASE("pathFor never looks up a network or device path") {
    using Extensions::Access;
    using Extensions::pathFor;
    Extensions::Extension ext;
    ext.name = "map";
    ext.folder = fs::temp_directory_path() / "genshin-overlay-tests";
    ext.extensionPermissions = { Permission::InternalReadfile };
    CaptureLog log;

    // looking one of these up would already send the player's login to the server
    for (const char* path : { R"(\\no-such-server\share\notes.txt)", "//no-such-server/share/notes.txt",
        R"(\\?\C:\Windows\win.ini)", R"(\\.\pipe\notes)", R"(/\no-such-server\share)" }) {
        CAPTURE(path);
        auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(pathFor(ext, path, Access::Read));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
    }
    CHECK(log.count("can't use the path") == 5);
}

TEST_CASE("pathFor takes folder and file names in any language") {
    using Extensions::Access;
    fs::path root = fs::weakly_canonical(fs::temp_directory_path() / "genshin-overlay-tests" / "paths");
    fs::path folder = root / Helpers::fromUtf8("地图");
    fs::create_directories(folder);
    std::ofstream(folder / Helpers::fromUtf8("笔记.txt")) << "hi";

    Extensions::Extension ext;
    ext.name = "地图";
    ext.folder = folder;
    ext.extensionPermissions = { Permission::InternalReadfile };

    auto file = Extensions::pathFor(ext, "笔记.txt", Access::Read);
    REQUIRE(file);
    CHECK(*file == folder / Helpers::fromUtf8("笔记.txt"));
    CHECK(Files::readText(*file) == "hi");

    // and through Lua, the way an extension asks
    auto lua = Extensions::newLuaState(folder);
    RunAs runAs(ext);
    CHECK(lua->script(R"(return ReadFile("笔记.txt"))").get<std::string>() == "hi");
    CHECK(lua->script(R"(return ListDirectory("")[1])").get<std::string>() == "笔记.txt");
}

TEST_CASE("an extension loads with its metadata and its functions") {
    std::filesystem::remove(Settings::file);
    Extensions::Extension* ext = TestExtension::load("loads", R"(
        metadata = { name = "Test", description = "For the tests", author = "Someone", version = "1.2.3" }
        function Init() _G.started = true end
        function Update() _G.updates = (_G.updates or 0) + 1 end
    )");
    REQUIRE(ext != nullptr);
    CHECK(ext->name == "Test");
    CHECK(ext->description == "For the tests");
    CHECK(ext->author == "Someone");
    CHECK(ext->version == "1.2.3");
    CHECK(ext->folder == TestExtension::folder("loads"));
    CHECK(ext->extensionPermissions == Extensions::defaultPermissions);

    // only the functions main.lua has
    CHECK(ext->initializeFunction);
    CHECK(ext->updateFunction);
    CHECK(!ext->menuFunction);
    CHECK(!ext->registerSettings);
    CHECK(!ext->shutdownFunction);

    Extensions::runForExtension(ext, ext->initializeFunction);
    CHECK((*ext->luaState)["started"].get_or(false) == true);
    Extensions::frameUpdateExtensions();
    Extensions::frameUpdateExtensions();
    CHECK((*ext->luaState)["updates"].get_or(0) == 2);
    TestExtension::unloadAll();
}

TEST_CASE("an extension without metadata is named after its folder") {
    Extensions::Extension* ext = TestExtension::load("no_metadata", "function Init() end");
    REQUIRE(ext != nullptr);
    CHECK(ext->name == "no_metadata");
    TestExtension::unloadAll();
}

TEST_CASE("an extension's name is shown whole, an empty one is its folder's") {
    // ImGui would show "Map" for the first and hide the rest
    Extensions::Extension* ext = TestExtension::load("hidden_name", R"( metadata = { name = "Map##Settings" } )");
    REQUIRE(ext != nullptr);
    CHECK(ext->name == "Map# #Settings");
    ext = TestExtension::load("empty_name", R"( metadata = { name = "" } )");
    REQUIRE(ext != nullptr);
    CHECK(ext->name == "empty_name");
    // a folder's name too, with no name given or an empty one; the folder stays the key
    ext = TestExtension::load("Tools##Hidden", "");
    REQUIRE(ext != nullptr);
    CHECK(ext->name == "Tools# #Hidden");
    CHECK(ext->folderKey == "Tools##Hidden");
    ext = TestExtension::load("Maps##Hidden", R"( metadata = { name = "" } )");
    REQUIRE(ext != nullptr);
    CHECK(ext->name == "Maps# #Hidden");
    TestExtension::unloadAll();
}

TEST_CASE("a main.lua that's wrong isn't loaded") {
    CHECK(TestExtension::load("broken", "this isn't lua") == nullptr);
    CHECK(TestExtension::load("stops", "error('no')") == nullptr);
    CHECK(Extensions::registeredExtensions.empty());
}

TEST_CASE("an error in an extension's function doesn't get out") {
    Extensions::Extension* ext = TestExtension::load("throws", R"(
        function Update() error("every frame") end
    )");
    REQUIRE(ext != nullptr);
    CHECK_NOTHROW(Extensions::frameUpdateExtensions());
    TestExtension::unloadAll();
}

TEST_CASE("an error in a key or interval callback names its extension") {
    Keybindings::keybinds.clear();
    Extensions::Extension* ext = TestExtension::load("callback_errors", R"(
        metadata = { name = "Breaks" }
        function Init()
            SetInterval(function() error("interval broke") end, 1)
            RegisterKeybind(Keys.F7, function() error("key broke") end, KeybindFlags.None)
        end
    )");
    REQUIRE(ext != nullptr);
    Extensions::runForExtension(ext, ext->initializeFunction);

    CaptureLog log;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    LuaFunctions::UpdateIntervals();
    Keybindings::Keybind* f7 = Keybindings::DoesKeybindExist(GLFW_KEY_F7);
    REQUIRE(f7 != nullptr);
    f7->Press();
    Keybindings::ProcessKeybindings();
    CHECK(log.count("[Breaks] interval: ") == 1);
    CHECK(log.count("interval broke") == 1);
    CHECK(log.count("[Breaks] key: ") == 1);
    CHECK(log.count("key broke") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("the hash follows the Lua files and nothing else") {
    std::string main = "function Init() end";
    Extensions::Extension* ext = TestExtension::load("hashed", main, { { "helper.lua", "return 1" } });
    REQUIRE(ext != nullptr);
    std::string hash = ext->extensionHash;
    CHECK(hash.size() == 128);

    ext = TestExtension::load("hashed", main, { { "helper.lua", "return 1" }, { "notes.txt", "not code" } });
    REQUIRE(ext != nullptr);
    CHECK(ext->extensionHash == hash);

    ext = TestExtension::load("hashed", main, { { "helper.lua", "return 2" } });
    REQUIRE(ext != nullptr);
    CHECK(ext->extensionHash != hash);

    ext = TestExtension::load("hashed", main, { { "helper.lua", "return 1" }, { "more.lua", "return 1" } });
    REQUIRE(ext != nullptr);
    CHECK(ext->extensionHash != hash);

    // require("more") opens more.LUA too, so it counts
    ext = TestExtension::load("hashed", main, { { "helper.lua", "return 1" }, { "more.LUA", "return 1" } });
    REQUIRE(ext != nullptr);
    CHECK(ext->extensionHash != hash);
    TestExtension::unloadAll();
}

TEST_CASE("a Lua file named in any language is hashed and loads") {
    // outside the Windows code page here, it loads like any other
    std::string main = "function Init() end";
    Extensions::Extension* ext = TestExtension::load("names", main, { { "地图.lua", "return { answer = 7 }" } });
    REQUIRE(ext != nullptr);
    std::string hash = ext->extensionHash;
    REQUIRE(TestExtension::run(ext, R"( answer = require("地图").answer )"));
    CHECK((*ext->luaState)["answer"].get_or(0) == 7);

    ext = TestExtension::load("names", main, { { "地图.lua", "return { answer = 8 }" } });
    REQUIRE(ext != nullptr);
    CHECK(ext->extensionHash != hash);
    TestExtension::unloadAll();
}

TEST_CASE("an extension can't run code from text or a DLL, or reach os and io") {
    Extensions::Extension* ext = TestExtension::load("closed", "function Init() end",
        { { "helper.lua", "return { answer = 42 }" } });
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"(
        closed = load == nil and loadfile == nil and dofile == nil and loadstring == nil
            and package.loadlib == nil and os == nil and io == nil and debug == nil
        answer = require("helper").answer
        network = not pcall(require, "//no-such-server/share/helper")
            and not pcall(require, [[\\no-such-server\share\helper]])
            and not pcall(require, "C:/Windows/helper")
    )"));
    CHECK((*ext->luaState)["network"].get_or(false) == true);
    CHECK((*ext->luaState)["closed"].get_or(false) == true);
    // Lua files in its own folder are fine
    CHECK((*ext->luaState)["answer"].get_or(0) == 42);
    TestExtension::unloadAll();
}

static const char* WITH_SETTINGS = R"(
    function RegisterSettings()
        DefineExtensionSetting("on", "On", "bool", true)
        DefineExtensionSetting("count", "Count", "int", 5)
        DefineExtensionSetting("size", "Size", "float", 1.5, 0.5, 4.0)
        DefineExtensionSetting("label", "Label", "string", "hi")
        -- the default isn't an int
        DefineExtensionSetting("wrong", "Wrong", "int", "five")
        -- there already is a count
        DefineExtensionSetting("count", "Count again", "int", 9)
    end
)";

TEST_CASE("an extension's settings start at their defaults") {
    std::filesystem::remove(Settings::file);
    Extensions::Extension* ext = TestExtension::load("settings", WITH_SETTINGS);
    REQUIRE(ext != nullptr);
    Extensions::configureSettings();

    CHECK(ext->settingsOrder == std::vector<std::string>{ "on", "count", "size", "label" });
    REQUIRE(ext->settings.count("size") == 1);
    CHECK(ext->settings["size"].min == 0.5f);
    CHECK(ext->settings["size"].max == 4.0f);
    CHECK(!ext->settings["count"].min);
    CHECK(ext->settings["count"].label == "Count");

    REQUIRE(TestExtension::run(ext, R"(
        on, count, size, label = GetSetting("on"), GetSetting("count"), GetSetting("size"), GetSetting("label")
        missing = GetSetting("wrong") == nil and GetSetting("never defined") == nil
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["on"].get_or(false) == true);
    CHECK(lua["count"].get_or(0) == 5);
    CHECK(lua["size"].get_or(0.0) == Approx(1.5));
    CHECK(lua["label"].get_or(std::string()) == "hi");
    CHECK(lua["missing"].get_or(false) == true);
    TestExtension::unloadAll();
}

TEST_CASE("saved extension settings come back, one of the wrong type is left out") {
    std::filesystem::remove(Settings::file);
    Settings::SaveExtensions({ { "settings", { { "on", false }, { "count", 8 }, { "label", 3 }, { "gone", 1 } } } });
    Extensions::Extension* ext = TestExtension::load("settings", WITH_SETTINGS);
    REQUIRE(ext != nullptr);
    Extensions::configureSettings();

    REQUIRE(TestExtension::run(ext, R"(
        on, count, size, label = GetSetting("on"), GetSetting("count"), GetSetting("size"), GetSetting("label")
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["on"].get_or(true) == false);
    CHECK(lua["count"].get_or(0) == 8);
    // not in the file
    CHECK(lua["size"].get_or(0.0) == Approx(1.5));
    // a number was saved for it
    CHECK(lua["label"].get_or(std::string()) == "hi");
    TestExtension::unloadAll();
}
