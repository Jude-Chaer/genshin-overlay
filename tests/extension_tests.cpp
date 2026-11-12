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
#include <imgui.h>
#include <imgui_internal.h>
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

    // the socket's methods, as WebSocket.connect and so on, found through the
    // metatables an extension's getmetatable doesn't give
    lua->set("bound", bound);
    lua->set_function("realMetatable", [](sol::object value, sol::this_state state) -> sol::object {
        lua_State* L = state;
        value.push(L);
        if (!lua_getmetatable(L, -1)) {
            lua_pop(L, 1);
            return sol::lua_nil;
        }
        sol::object meta(L, -1);
        lua_pop(L, 2);
        return meta;
    });
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
            local meta = realMetatable(t)
            if type(meta) == "table" then
                look(meta)
                look(rawget(meta, "__index"))
            end
        end
        look(WebSocket)
        look(realMetatable(WebSocket.new()))
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

TEST_CASE("an extension that fails while loading leaves only its error behind") {
    fs::remove(Settings::file);
    Keybindings::keybinds.clear();
    LuaFunctions::intervals.clear();
    Extensions::registeredExtensions.clear();
    fs::path folder = DATA / "extensions" / "errors_after_register";

    CaptureLog log;
    Extensions::loadLuaExtension(folder / "main.lua", folder);

    CHECK(log.count("stops here on purpose") == 1);
    REQUIRE(Extensions::registeredExtensions.size() == 1);
    Extensions::Extension* ext = Extensions::registeredExtensions[0].get();
    CHECK(ext->startError == "stops here on purpose (main.lua:4)");
    CHECK(ext->luaState == nullptr);
    CHECK(ext->stopped);
    CHECK(Keybindings::keybinds.empty());
    CHECK(LuaFunctions::intervals.empty());
    CHECK(Extensions::currentExtension == nullptr);
    TestExtension::unloadAll();
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

TEST_CASE("what a finalizer asks for while the extension closes is refused") {
    Keybindings::keybinds.clear();
    LuaFunctions::intervals.clear();
    // An extension can't make a finalizer (see "no Lua runs where the guard
    // can't stop it"), the test makes one past that, in case another way
    // turns up. It runs while the Lua state closes; what it made would call
    // into the closed state.
    Extensions::Extension* ext = TestExtension::load("finalizer_unloads", "function Init() end");
    REQUIRE(ext != nullptr);
    sol::state& lua = *ext->luaState;
    sol::table meta = lua.create_table();
    meta["__gc"] = lua.load(R"(
        SetInterval(function() end, 10)
        RegisterKeybind(Keys.F10, function() end, KeybindFlags.None)
        DefineExtensionSetting("late", "Late", "int", 1)
    )").get<sol::protected_function>();
    sol::table kept = lua.create_table();
    kept[sol::metatable_key] = meta;
    // kept, so it runs at the close and not at an earlier collection
    lua["kept"] = kept;
    kept = sol::lua_nil;
    meta = sol::lua_nil;

    TestExtension::unloadAll();
    CHECK(LuaFunctions::intervals.empty());
    CHECK(Keybindings::keybinds.empty());
    CHECK(Extensions::currentExtension == nullptr);
    CHECK_NOTHROW(LuaFunctions::UpdateIntervals());
}

TEST_CASE("with no extension running every function refuses") {
    auto lua = Extensions::newLuaState(DATA / "extensions" / "registers_in_init");
    REQUIRE(Extensions::currentExtension == nullptr);
    LuaFunctions::intervals.clear();
    lua->script(R"(
        interval = SetInterval(function() end, 10)
        json = JSON_Encode({ 1 })
    )");
    CHECK(lua->script("return interval == nil and json == nil").get<bool>());
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
        R"(\\?\C:\Windows\win.ini)", R"(\\.\pipe\notes)", R"(/\no-such-server\share)",
        R"(\??\UNC\no-such-server\share\notes.txt)", R"(\??\C:\Windows\win.ini)" }) {
        CAPTURE(path);
        auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(pathFor(ext, path, Access::Read));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
    }
    CHECK(log.count("can't use the path") == 7);
}

TEST_CASE("pathFor refuses device names and hidden streams by their text") {
    using Extensions::Access;
    using Extensions::pathFor;
    fs::path root = fs::weakly_canonical(fs::temp_directory_path() / "genshin-overlay-tests" / "paths");
    fs::create_directories(root / "map" / "sub");
    Extensions::Extension ext;
    ext.name = "map";
    ext.folder = root / "map";
    ext.extensionPermissions = { Permission::InternalReadfile, Permission::InternalWritefile };
    CaptureLog log;

    // Windows opens each of these as a device, in any folder
    for (const char* path : { "CON", "nul", "nul.txt", "sub/COM1", "aux .log", "LPT9.dat", "conin$", "COM\xC2\xB9" }) {
        CAPTURE(path);
        CHECK_FALSE(pathFor(ext, path, Access::Read));
        CHECK_FALSE(pathFor(ext, path, Access::Write));
    }
    // a file's hidden stream (main.lua:code) is a file no hash sees
    CHECK_FALSE(pathFor(ext, "main.lua:code", Access::Write));
    CHECK_FALSE(pathFor(ext, "sub/x:y", Access::Read));
    // names that only start like one are files
    for (const char* path : { "console.txt", "COM10", "nulls.txt", "sub/auxiliary" }) {
        CAPTURE(path);
        CHECK(pathFor(ext, path, Access::Read) == root / "map" / Helpers::fromUtf8(path).make_preferred());
    }
    // a full path into its own folder is still its own
    CHECK(pathFor(ext, Helpers::utf8(root / "map" / "notes.txt"), Access::Read) == root / "map" / "notes.txt");
}

TEST_CASE("the file functions only open plain files") {
    Extensions::Extension* ext = TestExtension::load("plain_files", "function Init() end");
    REQUIRE(ext != nullptr);
    fs::create_directories(ext->folder / "sub");
    REQUIRE(TestExtension::run(ext, R"(
        readFolder = not pcall(ReadFile, "sub")
        writeFolder = not pcall(WriteFile, "sub", "x")
        appendFolder = not pcall(AppendFile, "sub", "x")
        textureFolder = LoadTexture("sub") == nil
        readDevice = ReadFile("NUL") == nil
        writeDevice = WriteFile("con", "x") == false
        newFile = WriteFile("new.txt", "made")
    )"));
    sol::state& lua = *ext->luaState;
    for (const char* name : { "readFolder", "writeFolder", "appendFolder", "textureFolder", "readDevice", "writeDevice", "newFile" }) {
        INFO(name);
        CHECK(lua[name].get_or(false) == true);
    }
    CHECK(Files::readText(ext->folder / "new.txt") == "made");
    CHECK(fs::is_directory(ext->folder / "sub"));
    TestExtension::unloadAll();
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

TEST_CASE("a main.lua that's wrong doesn't start, its page says why") {
    TestExtension::unloadAll();
    CHECK(TestExtension::load("broken", "this isn't lua") == nullptr);
    CHECK(TestExtension::load("stops", "\n\nerror('no')") == nullptr);
    REQUIRE(Extensions::registeredExtensions.size() == 2);
    CHECK(Extensions::registeredExtensions[0]->startError.find("(main.lua:1)") != std::string::npos);
    CHECK(Extensions::registeredExtensions[1]->startError == "no (main.lua:3)");
    CHECK(Extensions::registeredExtensions[1]->luaState == nullptr);
    TestExtension::unloadAll();
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
            and string.dump == nil and #package.searchers == 2 and package.searchpath == nil
        answer = require("helper").answer
        outside = not pcall(require, "..closed_other.helper")
        network = not pcall(require, "//no-such-server/share/helper")
            and not pcall(require, [[\\no-such-server\share\helper]])
            and not pcall(require, "C:/Windows/helper")
    )"));
    CHECK((*ext->luaState)["network"].get_or(false) == true);
    CHECK((*ext->luaState)["closed"].get_or(false) == true);
    // Lua files in its own folder are fine, others aren't
    CHECK((*ext->luaState)["answer"].get_or(0) == 42);
    CHECK((*ext->luaState)["outside"].get_or(false) == true);
    TestExtension::unloadAll();
}

TEST_CASE("require runs only the Lua files that were hashed, as they were then") {
    Extensions::Extension* ext = TestExtension::load("require_hashed", "function Init() end",
        { { "helper.lua", "return 1" }, { "changed.lua", "return 1" } });
    REQUIRE(ext != nullptr);
    // what the player allowed was for these files; new code would run under it
    std::ofstream(ext->folder / "changed.lua", std::ios::binary | std::ios::trunc) << "return 2";
    REQUIRE(TestExtension::run(ext, R"(
        helper = require("helper")
        sameCase = require("HELPER")
        WriteFile("written.lua", "return 3")
        written, whyWritten = pcall(require, "written")
        changed, whyChanged = pcall(require, "changed")
        stream = pcall(require, "helper.lua:code")
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["helper"].get_or(0) == 1);
    CHECK(lua["sameCase"].get_or(0) == 1);
    CHECK(lua["written"].get_or(true) == false);
    CHECK(lua["whyWritten"].get_or(std::string()).find("wasn't there when the extension loaded") != std::string::npos);
    CHECK(lua["changed"].get_or(true) == false);
    CHECK(lua["whyChanged"].get_or(std::string()).find("changed since the extension loaded") != std::string::npos);
    CHECK(lua["stream"].get_or(true) == false);
    TestExtension::unloadAll();
}

// precompiled Lua, which can get around Lua's own checks
static std::string bytecode(const std::string& code) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);
    return lua.script("return string.dump(function() " + code + " end)").get<std::string>();
}

TEST_CASE("precompiled Lua isn't loaded, not as main.lua and not by require") {
    CaptureLog log;
    CHECK(TestExtension::load("precompiled", bytecode("_G.ran = true")) == nullptr);
    CHECK(log.count("attempt to load a binary chunk") == 1);

    Extensions::Extension* ext = TestExtension::load("precompiled", "function Init() end",
        { { "helper.lua", bytecode("return 42") } });
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"( loaded, why = pcall(require, "helper") )"));
    CHECK((*ext->luaState)["loaded"].get_or(true) == false);
    CHECK((*ext->luaState)["why"].get_or(std::string()).find("binary chunk") != std::string::npos);
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

namespace {
    // one frame the way the overlay makes it, with no screen to show it on
    struct Frame {
        ImGuiContext* context = ImGui::CreateContext();
        Frame() {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            ImGui::NewFrame();
        }
        ~Frame() {
            ImGui::EndFrame();
            ImGui::DestroyContext(context);
        }
    };

    Extensions::Extension* drawing(const std::string& name) {
        Extensions::Extension* ext = TestExtension::load(name, "function Init() end");
        if (ext) ext->extensionPermissions.insert(Permission::DisplayOverlay);
        return ext;
    }
}

TEST_CASE("ImGui functions work only in their part of the frame") {
    Extensions::Extension* ext = drawing("phases");
    REQUIRE(ext != nullptr);
    Frame frame;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const char* everything = R"(
        ImGUI_Text("text") ImGUI_Separator() ImGUI_SameLine()
        checked = ImGUI_Checkbox("box", true)
        typed = ImGUI_InputText("type", "kept")
        DrawText("drawn", 10, 10)
    )";

    SECTION("outside the frame (Init, keys) nothing draws") {
        CaptureLog log;
        int drawn = list->VtxBuffer.Size;
        float cursor = ImGui::GetCursorPosY();
        REQUIRE(TestExtension::run(ext, everything));
        CHECK(list->VtxBuffer.Size == drawn);
        CHECK(ImGui::GetCursorPosY() == cursor);
        // what it gave in is what it gets back
        CHECK(TestExtension::run(ext, "assert(checked == true and typed == 'kept')"));
        CHECK(log.count("ImGUI_Text works only in Menu()") == 1);
        CHECK(log.count("DrawText works only in Update() and Menu()") == 1);
        // once per function
        REQUIRE(TestExtension::run(ext, everything));
        CHECK(log.count("ImGUI_Text works only in Menu()") == 1);
    }

    SECTION("in Update only drawing on the overlay works") {
        Extensions::InPhase update(Extensions::Phase::Update);
        int drawn = list->VtxBuffer.Size;
        float cursor = ImGui::GetCursorPosY();
        REQUIRE(TestExtension::run(ext, everything));
        CHECK(list->VtxBuffer.Size > drawn);
        CHECK(ImGui::GetCursorPosY() == cursor);
    }

    SECTION("in Menu both work") {
        ImGui::Begin("Genshin Overlay");
        float cursor = ImGui::GetCursorPosY();
        int drawn = list->VtxBuffer.Size;
        {
            Extensions::InPhase menu(Extensions::Phase::Menu);
            REQUIRE(TestExtension::run(ext, everything));
        }
        CHECK(ImGui::GetCursorPosY() > cursor);
        CHECK(list->VtxBuffer.Size > drawn);
        ImGui::End();
    }
    CHECK(Extensions::phase == Extensions::Phase::None);
    TestExtension::unloadAll();
}

TEST_CASE("an extension closes only its own clips") {
    Extensions::Extension* ext = drawing("clips");
    REQUIRE(ext != nullptr);
    Frame frame;
    Extensions::InPhase update(Extensions::Phase::Update);
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    // the overlay's own clip, under the extension's
    list->PushClipRect(ImVec2(100, 100), ImVec2(200, 200));
    const int base = list->_ClipRectStack.Size;
    const ImVec4 ours = list->_ClipRectStack.back();

    CaptureLog log;
    // more pops than pushes, then a push: the overlay's clips stay
    REQUIRE(TestExtension::run(ext, "PopClipRect() PopClipRect() PopClipRect() PushClipRect(0, 0, 5, 5)"));
    CHECK(list->_ClipRectStack.Size == base);
    CHECK(list->_ClipRectStack.back().x == ours.x);
    CHECK(list->_ClipRectStack.back().z == ours.z);
    CHECK(log.count("PopClipRect has no PushClipRect of its own left to close") == 1);

    // left open: closed after the callback
    REQUIRE(TestExtension::run(ext, "PushClipRect(0, 0, 5, 5) PushClipRect(1, 1, 4, 4)"));
    CHECK(list->_ClipRectStack.Size == base);

    // and when it stops with an error
    CHECK_FALSE(TestExtension::run(ext, "PushClipRect(0, 0, 5, 5) error('stops here')"));
    CHECK(list->_ClipRectStack.Size == base);

    // its own push and pop still work
    REQUIRE(TestExtension::run(ext, "PushClipRect(0, 0, 5, 5) PopClipRect()"));
    CHECK(list->_ClipRectStack.Size == base);
    CHECK(log.count("PopClipRect has no PushClipRect") == 1);
    list->PopClipRect();
    TestExtension::unloadAll();
}

TEST_CASE("what an extension draws stays under the menu") {
    Extensions::Extension* ext = drawing("under_menu");
    REQUIRE(ext != nullptr);
    Frame frame;
    ImGui::Begin("Genshin Overlay");
    ImDrawList* menu = ImGui::GetWindowDrawList();
    ImDrawList* back = ImGui::GetBackgroundDrawList();
    ImDrawList* front = ImGui::GetForegroundDrawList();
    int menuDrawn = menu->VtxBuffer.Size, frontDrawn = front->VtxBuffer.Size, backDrawn = back->VtxBuffer.Size;
    for (Extensions::Phase phase : { Extensions::Phase::Update, Extensions::Phase::Menu }) {
        Extensions::InPhase in(phase);
        REQUIRE(TestExtension::run(ext, R"(
            DrawText("drawn", 10, 10)
            DrawImage(1, 0, 0, 50, 50)
            PushClipRect(0, 0, 20, 20) DrawText("clipped", 5, 5) PopClipRect()
        )"));
    }
    CHECK(menu->VtxBuffer.Size == menuDrawn);
    CHECK(front->VtxBuffer.Size == frontDrawn);
    CHECK(back->VtxBuffer.Size > backDrawn);
    ImGui::End();
    TestExtension::unloadAll();
}

TEST_CASE("a call that never ends is stopped, and the extension unloaded at the next frame") {
    Keybindings::keybinds.clear();
    LuaFunctions::intervals.clear();
    Extensions::Extension* ext = TestExtension::load("loops", R"(
        metadata = { name = "Loops" }
        function Init() SetInterval(function() end, 1000) end
        function Update() while true do end end
    )");
    REQUIRE(ext != nullptr);
    Extensions::runForExtension(ext, ext->initializeFunction);
    REQUIRE(LuaFunctions::intervals.size() == 1);

    CaptureLog log;
    Extensions::frameUpdateExtensions();
    CHECK(ext->stopReason == "one call ran more than 50 million Lua steps");
    CHECK(log.count("[Loops] Update: ") == 1);

    // nothing of it runs until then, a key callback neither
    CHECK(!TestExtension::run(ext, "ran = true"));
    CHECK((*ext->luaState)["ran"] == sol::nil);

    Extensions::frameUpdateExtensions();
    CHECK(ext->stopped);
    CHECK(!ext->luaState);
    CHECK(ext->memory->used == 0);
    CHECK(LuaFunctions::intervals.empty());
    CHECK(log.count("[Loops] stopped: ") == 1);
    CHECK(log.count("[Loops] Update: ") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("a pcall can't catch the guard") {
    const char* loops[] = {
        "function Update() local f = function() end while true do pcall(f) end end",
        "function Update() while true do pcall(function() while true do end end) end end",
        "function Update() while true do xpcall(function() while true do end end, function() while true do end end) end end",
    };
    for (const char* loop : loops) {
        INFO(loop);
        Extensions::Extension* ext = TestExtension::load("pcall_loops", loop);
        REQUIRE(ext != nullptr);
        Extensions::frameUpdateExtensions();
        CHECK(!ext->stopReason.empty());
        Extensions::frameUpdateExtensions();
        CHECK(ext->stopped);
        TestExtension::unloadAll();
    }
}

TEST_CASE("the guard counts each call on its own, and not the time in overlay functions") {
    // about 20 million steps a call, 60 million in all
    Extensions::Extension* ext = TestExtension::load("busy", R"(
        function Update() local n = 0 for i = 1, 10000000 do n = n + i end _G.calls = (_G.calls or 0) + 1 end
    )");
    REQUIRE(ext != nullptr);
    for (int i = 0; i < 3; i++) Extensions::frameUpdateExtensions();
    CHECK(ext->stopReason.empty());
    CHECK((*ext->luaState)["calls"].get_or(0) == 3);

    // a slow Net_Get is time in the overlay, not Lua steps
    (*ext->luaState)["SlowHost"] = []() { std::this_thread::sleep_for(std::chrono::seconds(1)); };
    CHECK(TestExtension::run(ext, "SlowHost() slowDone = true"));
    CHECK((*ext->luaState)["slowDone"].get_or(false) == true);
    CHECK(ext->stopReason.empty());
    TestExtension::unloadAll();
}

TEST_CASE("a main.lua that never ends doesn't load") {
    CaptureLog log;
    CHECK(TestExtension::load("loops_at_load", "while true do end") == nullptr);
    CHECK(log.count("stopped: one call ran more than 500 million Lua steps") == 1);
    // shown as Stopped, not as an error
    REQUIRE(!Extensions::registeredExtensions.empty());
    Extensions::Extension* ext = Extensions::registeredExtensions.back().get();
    CHECK(!ext->stopReason.empty());
    CHECK(ext->startError.empty());
    CHECK_FALSE(ext->stoppedForMemory);
    CHECK(ext->luaState == nullptr);
    TestExtension::unloadAll();
}

TEST_CASE("main.lua and Init may take more steps than a frame's call") {
    // about 60 million steps each
    const char* work = "local n = 0 for i = 1, 30000000 do n = n + i end";
    Extensions::Extension* ext = TestExtension::load("starts_slowly", std::string(work) + R"(
        function Init() )" + work + R"( _G.started = true end
        function Update() )" + work + R"( end
    )");
    REQUIRE(ext != nullptr);
    Extensions::runInit(*ext);
    CHECK((*ext->luaState)["started"].get_or(false) == true);
    CHECK(ext->stopReason.empty());
    Extensions::frameUpdateExtensions();
    CHECK(ext->stopReason == "one call ran more than 50 million Lua steps");
    TestExtension::unloadAll();
}

TEST_CASE("an extension that holds too much is stopped, garbage doesn't count") {
    const size_t limit = 16 * 1024 * 1024;

    // makes 200 MB of strings it lets go of at once
    Extensions::Extension* ext = TestExtension::load("garbage", R"(
        function Update() for i = 1, 100000 do local s = string.rep("x", 1000) .. i end end
    )");
    REQUIRE(ext != nullptr);
    ext->memory->limit = limit;
    Extensions::frameUpdateExtensions();
    CHECK(ext->stopReason.empty());
    CHECK(ext->memory->used < limit);
    TestExtension::unloadAll();

    const char* holds[] = {
        "function Update() local t, i = {}, 0 while true do i = i + 1 t[i] = string.rep('x', 1000) .. i end end",
        "function Update() while true do pcall(string.rep, 'x', 100000000) end end",
    };
    for (const char* hold : holds) {
        INFO(hold);
        ext = TestExtension::load("holds", hold);
        REQUIRE(ext != nullptr);
        ext->memory->limit = limit;
        CaptureLog log;
        Extensions::frameUpdateExtensions();
        CHECK(ext->stopReason == "it held more than 16 MB");
        Extensions::frameUpdateExtensions();
        CHECK(ext->stopped);
        CHECK(ext->memory->used == 0);
        TestExtension::unloadAll();
    }
}

TEST_CASE("no Lua runs where the guard can't stop it") {
    // Lua runs no hooks in a finalizer: an extension can't make one
    Extensions::Extension* ext = TestExtension::load("finalizers", "function Init() end");
    REQUIRE(ext != nullptr);
    CHECK(!TestExtension::run(ext, "setmetatable({}, { __gc = function() end })"));
    CHECK(TestExtension::run(ext, "plain = setmetatable({}, { __index = { a = 1 } }).a"));
    CHECK((*ext->luaState)["plain"].get_or(0) == 1);
    // strings keep theirs, and a class's __metatable is the extension's own
    CHECK(TestExtension::run(ext, R"(
        stringIndex = getmetatable("").__index == string
        local shown = {}
        classSeen = getmetatable(setmetatable({}, { __metatable = shown })) == shown
    )"));
    CHECK((*ext->luaState)["stringIndex"].get_or(false));
    CHECK((*ext->luaState)["classSeen"].get_or(false));

    // nor through the metatable of the overlay's own userdata
    TestExtension::run(ext, R"(
        local ok = pcall(function()
            local meta = getmetatable(WebSocket.new())
            if type(meta) == "table" then meta.__gc = function() while true do end end end
        end)
        socket = WebSocket.new()
        socket = nil
    )");
    CaptureLog log;
    auto started = std::chrono::steady_clock::now();
    TestExtension::run(ext, "collectgarbage() collectgarbage()");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));

    // xpcall's handler runs with the guard on
    CHECK(TestExtension::run(ext, R"(
        ok, message = xpcall(function(a) error("broke " .. a, 0) end, function(e) return "handled: " .. e end, "here")
        good = { xpcall(function(a, b) return a + b, "two" end, print, 1, 2) }
    )"));
    CHECK((*ext->luaState)["ok"].get_or(true) == false);
    CHECK((*ext->luaState)["message"].get_or(std::string()) == "handled: broke here");
    CHECK((*ext->luaState)["good"][1].get_or(false) == true);
    CHECK((*ext->luaState)["good"][2].get_or(0) == 3);
    CHECK((*ext->luaState)["good"][3].get_or(std::string()) == "two");
    TestExtension::unloadAll();
}

TEST_CASE("an error's file and line go after it") {
    Extensions::Extension ext;
    ext.folderKey = "tools";
    CHECK(Extensions::errorLine(ext, "tools/main.lua:12: attempt to call a nil value") == "attempt to call a nil value (main.lua:12)");
    CHECK(Extensions::errorLine(ext, "lib/helper.lua:3: bad") == "bad (lib/helper.lua:3)");
    // one with no place stays as it is
    CHECK(Extensions::errorLine(ext, "not enough memory") == "not enough memory");
    CHECK(Extensions::errorLine(ext, "tools/main.lua: odd") == "tools/main.lua: odd");
    CHECK(Extensions::errorLine(ext, "tools/main.lua:2: no\nstack traceback:\n\t[C]: in function 'base.error'") == "no (main.lua:2)");
}

TEST_CASE("an Init that fails is a start that failed") {
    Keybindings::keybinds.clear();
    CaptureLog log;
    TestExtension::load("init_fails", R"(
        function Init()
            SetInterval(function() end, 1000)
            error("no data")
        end
    )");
    REQUIRE(!Extensions::registeredExtensions.empty());
    Extensions::Extension* ext = Extensions::registeredExtensions.back().get();
    REQUIRE(ext->luaState != nullptr);
    Extensions::runInit(*ext);
    CHECK(ext->startError == "no data (main.lua:4)");
    CHECK(ext->luaState == nullptr);
    CHECK(LuaFunctions::intervals.empty());
    TestExtension::unloadAll();
}

TEST_CASE("Start again loads the extension from its folder again") {
    TestExtension::unloadAll();
    CaptureLog log;
    TestExtension::load("starts_again", "error('not yet')");
    REQUIRE(Extensions::registeredExtensions.size() == 1);
    CHECK(Extensions::registeredExtensions[0]->startError == "not yet (main.lua:1)");

    // fixed, then [Start again]
    std::ofstream(TestExtension::folder("starts_again") / "main.lua", std::ios::binary)
        << "function Init() _G.started = (_G.started or 0) + 1 end";
    Extensions::registeredExtensions[0]->startAgain = true;
    Extensions::frameUpdateExtensions();
    REQUIRE(Extensions::registeredExtensions.size() == 1);
    Extensions::Extension* ext = Extensions::registeredExtensions[0].get();
    CHECK(ext->startError.empty());
    CHECK_FALSE(ext->startAgain);
    CHECK_FALSE(ext->stopped);
    REQUIRE(ext->luaState != nullptr);
    CHECK((*ext->luaState)["started"].get_or(0) == 1);

    // stopped by the guard, then started again
    std::ofstream(TestExtension::folder("starts_again") / "main.lua", std::ios::binary)
        << "function Update() while true do end end";
    ext->startAgain = true;
    Extensions::frameUpdateExtensions();
    ext = Extensions::registeredExtensions[0].get();
    CHECK(ext->stopReason == "one call ran more than 50 million Lua steps");
    Extensions::frameUpdateExtensions();
    CHECK(ext->luaState == nullptr);
    std::ofstream(TestExtension::folder("starts_again") / "main.lua", std::ios::binary) << "_G.fixed = true";
    ext->startAgain = true;
    Extensions::frameUpdateExtensions();
    ext = Extensions::registeredExtensions[0].get();
    CHECK(ext->stopReason.empty());
    REQUIRE(ext->luaState != nullptr);
    CHECK((*ext->luaState)["fixed"].get_or(false) == true);
    TestExtension::unloadAll();
}

TEST_CASE("an error that repeats every frame is logged once") {
    CaptureLog log;
    Extensions::Extension* ext = TestExtension::load("fails_each_frame", R"(
        function Update()
            _G.frames = (_G.frames or 0) + 1
            if _G.frames == 4 then error("other") end
            error("same")
        end
    )");
    REQUIRE(ext != nullptr);
    for (int i = 0; i < 5; i++) Extensions::frameUpdateExtensions();
    CHECK((*ext->luaState)["frames"].get_or(0) == 5);
    CHECK(log.count(": same") == 2);
    CHECK(log.count(": other") == 1);
    TestExtension::unloadAll();

    // two functions failing in turn, each logged once
    ext = TestExtension::load("two_fail", R"(
        function Update() error("in update") end
        function Menu() error("in menu") end
    )");
    REQUIRE(ext != nullptr);
    for (int i = 0; i < 3; i++) {
        ext->updateFunction();
        ext->menuFunction();
    }
    CHECK(log.count(": in update") == 1);
    CHECK(log.count(": in menu") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("an extension has so many intervals, textures and settings at most") {
    CaptureLog log;
    Extensions::Extension* ext = TestExtension::load("many_things", R"(
        _G.intervals = 0
        for i = 1, 65 do
            if SetInterval(function() end, 1000) ~= 0 then _G.intervals = _G.intervals + 1 end
        end
        for i = 1, 129 do DefineExtensionSetting("s" .. i, "S" .. i, "bool", false) end
        DefineExtensionSetting("long", "Long", "string", string.rep("x", 257))
    )");
    REQUIRE(ext != nullptr);
    CHECK((*ext->luaState)["intervals"].get_or(0) == (int)Extensions::MaxIntervals);
    CHECK(ext->settings.size() == Extensions::MaxSettings);
    CHECK(log.count("has 64 intervals, no more are set") == 1);
    CHECK(log.count("has 128 settings, no more are made") == 1);

    // past the cap nothing is read
    ext->textures.assign(Extensions::MaxTextures, 0);
    CHECK(TestExtension::run(ext, "_G.texture = LoadTexture('icon.png')"));
    CHECK((*ext->luaState)["texture"] == sol::lua_nil);
    CHECK(log.count("has 256 textures, no more are loaded") == 1);
    // not real ones, nothing to free
    ext->textures.clear();
    TestExtension::unloadAll();

    // and a long text default
    ext = TestExtension::load("long_default", R"(
        DefineExtensionSetting("ok", "Ok", "string", string.rep("x", 256))
        DefineExtensionSetting("long", "Long", "string", string.rep("x", 257))
    )");
    REQUIRE(ext != nullptr);
    CHECK(ext->settings.count("ok") == 1);
    CHECK(ext->settings.count("long") == 0);
    TestExtension::unloadAll();
}

TEST_CASE("an extension has so many keys at most") {
    CaptureLog log;
    // 33: F1 to F11, each alone, with Ctrl and with Alt
    Extensions::Extension* ext = TestExtension::load("many_keys", R"(
        _G.keys = 0
        local mods = { KeybindModifiers.None, KeybindModifiers.Ctrl, KeybindModifiers.Alt }
        for _, mod in ipairs(mods) do
            for _, key in ipairs({ Keys.F1, Keys.F2, Keys.F3, Keys.F4, Keys.F5, Keys.F6, Keys.F7, Keys.F8, Keys.F9, Keys.F10, Keys.F11 }) do
                if RegisterKeybind(key, function() end, KeybindFlags.None, mod) ~= "" then _G.keys = _G.keys + 1 end
            end
        end
    )");
    REQUIRE(ext != nullptr);
    CHECK((*ext->luaState)["keys"].get_or(0) == (int)Extensions::MaxKeys);
    CHECK(ext->keybinds.size() == Extensions::MaxKeys);
    CHECK(log.count("has 32 keys, no more are bound") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("print gives so many lines a minute at most") {
    CaptureLog log;
    std::ostringstream out;
    std::streambuf* before = std::cout.rdbuf(out.rdbuf());
    Extensions::Extension* ext = TestExtension::load("prints", R"(
        print("a", 1, nil, true)
        for i = 1, 300 do print(i) end
    )");
    std::cout.rdbuf(before);
    REQUIRE(ext != nullptr);
    std::string printed = out.str();
    CHECK(printed.find("a\t1\tnil\ttrue\n") != std::string::npos);
    CHECK(printed.find("\n199\n") != std::string::npos);
    CHECK(printed.find("\n200\n") == std::string::npos);
    CHECK(log.count("more than 200 lines this minute, the rest are left out") == 1);
    TestExtension::unloadAll();
}

TEST_CASE("only the settings a player changed are saved") {
    std::filesystem::remove(Settings::file);
    Extensions::Extension* ext = TestExtension::load("saved_changes", R"(
        DefineExtensionSetting("label", "Label", "string", "hi")
        DefineExtensionSetting("count", "Count", "int", 5)
    )");
    REQUIRE(ext != nullptr);
    Extensions::saveSettings();
    CHECK(Settings::LoadExtensions().count("saved_changes") == 0);

    ext->settings["count"].value = 7;
    Extensions::saveSettings();
    nlohmann::json saved = Settings::LoadExtensions()["saved_changes"];
    CHECK(saved["count"] == 7);
    CHECK_FALSE(saved.contains("label"));

    // back at its default it's taken out
    ext->settings["count"].value = 5;
    Extensions::saveSettings();
    CHECK(Settings::LoadExtensions().count("saved_changes") == 0);
    TestExtension::unloadAll();
}
