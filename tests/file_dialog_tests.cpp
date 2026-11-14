#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include "extensions.hpp"
#include "file_dialogs.hpp"
#include "files.hpp"
#include "lua_functions.hpp"
#include "test_extension.hpp"

// The open and save windows: only the player starts one, only the extension
// that opened it gets the answer. The window itself is replaced by a picker
// that gives a file in the temp folder.

namespace fs = std::filesystem;

namespace {
    fs::path tempFile(const std::string& name) {
        fs::path folder = fs::temp_directory_path() / "genshin-overlay-tests" / "dialogs";
        fs::create_directories(folder);
        return folder / name;
    }

    // the picker gives path, or nothing for a closed window
    struct PickFile {
        FileDialogs::Picker previous = FileDialogs::picker;
        PickFile(const fs::path& path) {
            FileDialogs::picker = [path](HWND, bool, const std::wstring&, const std::optional<FileDialogs::Filter>&) { return path; };
        }
        ~PickFile() { FileDialogs::picker = previous; }
    };

    struct CaptureLog {
        std::ostringstream text;
        std::streambuf* previous = std::cerr.rdbuf(text.rdbuf());
        ~CaptureLog() { std::cerr.rdbuf(previous); }
        bool has(const std::string& part) const { return text.str().find(part) != std::string::npos; }
    };

    // a function of the extension's key, the way a key press runs it
    void pressKey(Extensions::Extension* ext) {
        REQUIRE(!ext->keys.empty());
        ext->keys.back().callback();
    }
}

TEST_CASE("a file window opens only from the function of the extension's button or key") {
    fs::path file = tempFile("only_player.json");
    Files::writeFile(file, "{}");
    PickFile pick(file);
    Extensions::Extension* ext = TestExtension::load("dialog_player", R"(
        RegisterKeybind(Keys.F7, function() _G.opened = OpenFileDialog(nil, function(r) _G.answer = r end) end, KeybindModifiers.Ctrl)
    )");
    REQUIRE(ext != nullptr);

    {
        CaptureLog log;
        REQUIRE(TestExtension::run(ext, "_G.early = OpenFileDialog()"));
        CHECK_FALSE((*ext->luaState)["early"].valid());
        CHECK(log.has("opens only from the function of its button or key"));
    }
    REQUIRE(TestExtension::run(ext, "_G.later = SaveFileDialog(\"a.json\", \"x\")"));
    CHECK_FALSE((*ext->luaState)["later"].valid());

    pressKey(ext);
    CHECK((*ext->luaState)["opened"].get_type() == sol::type::number);
    CHECK(ext->playerAction == 0);
    for (int i = 0; i < 200 && !(*ext->luaState)["answer"].valid(); i++) {
        LuaFunctions::UpdateFileDialogs();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        // let go of before its Lua state closes
        sol::table answer = (*ext->luaState)["answer"];
        CHECK(answer["name"].get<std::string>() == "only_player.json");
        CHECK(answer["data"].get<std::string>() == "{}");
    }
    TestExtension::unloadAll();
}

TEST_CASE("only the extension that opened a file window gets its answer") {
    fs::path file = tempFile("opener.txt");
    Files::writeFile(file, "mine");
    PickFile pick(file);
    Extensions::Extension* a = TestExtension::load("dialog_a", R"(
        RegisterKeybind(Keys.F9, function() _G.id = OpenFileDialog({ "Text", "*.txt" }) end, KeybindModifiers.Ctrl)
    )");
    Extensions::Extension* b = TestExtension::load("dialog_b", "");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    pressKey(a);
    int id = (*a->luaState)["id"].get_or(0);
    REQUIRE(id != 0);
    REQUIRE(TestExtension::run(b, "_G.id = " + std::to_string(id)));

    {
        // let go of before its Lua state closes
        sol::object got;
        for (int i = 0; i < 200; i++) {
            REQUIRE(TestExtension::run(b, "_G.theirs = FileDialogResult(id)"));
            CHECK_FALSE((*b->luaState)["theirs"].valid());
            REQUIRE(TestExtension::run(a, "_G.mine = FileDialogResult(id)"));
            got = (*a->luaState)["mine"];
            if (got.get_type() == sol::type::table) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(got.get_type() == sol::type::table);
        CHECK(got.as<sol::table>()["data"].get<std::string>() == "mine");
    }
    // once only
    REQUIRE(TestExtension::run(a, "_G.again = FileDialogResult(id)"));
    CHECK_FALSE((*a->luaState)["again"].valid());
    TestExtension::unloadAll();
}

TEST_CASE("a save writes the picked file whole, an open says why it couldn't") {
    fs::path saved = tempFile("saved.json");
    fs::remove(saved);
    Extensions::Extension* ext = TestExtension::load("dialog_save", R"(
        RegisterKeybind(Keys.F10, function()
            SaveFileDialog("marks.json", '{"a":1}', { "JSON", "*.json" }, function(r) _G.answer = r end)
        end, KeybindModifiers.Ctrl)
        RegisterKeybind(Keys.F11, function()
            OpenFileDialog(nil, function(r) _G.answer = r end)
        end, KeybindModifiers.Ctrl)
    )");
    REQUIRE(ext != nullptr);
    auto press = [&](int key) {
        (*ext->luaState)["answer"] = sol::lua_nil;
        ext->keys[key].callback();
        sol::table got;
        for (int i = 0; i < 200; i++) {
            LuaFunctions::UpdateFileDialogs();
            sol::object value = (*ext->luaState)["answer"];
            if (value.get_type() == sol::type::table) return value.as<sol::table>();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return got;
    };

    {
        PickFile pick(saved);
        sol::table result = press(0);
        REQUIRE(result.valid());
        CHECK(result["name"].get<std::string>() == "saved.json");
        CHECK_FALSE(result["data"].valid());
        CHECK(Files::readText(saved) == "{\"a\":1}");
    }
    {
        PickFile pick({});
        sol::table result = press(1);
        REQUIRE(result.valid());
        CHECK(result["error"].get<std::string>() == "cancelled");
    }
    {
        fs::path big = tempFile("big.bin");
        { std::ofstream(big, std::ios::binary); }
        fs::resize_file(big, FileDialogs::MaxSize + 1);
        PickFile pick(big);
        sol::table result = press(1);
        REQUIRE(result.valid());
        CHECK(result["error"].get<std::string>() == "too_big");
        CHECK_FALSE(result["data"].valid());
        fs::remove(big);
    }
    {
        PickFile pick(tempFile("missing") / "nothing.json");
        sol::table result = press(1);
        REQUIRE(result.valid());
        CHECK(result["error"].get<std::string>() == "failed");
    }
    TestExtension::unloadAll();
}

TEST_CASE("one file window at a time, and a filter has to be one") {
    // shared with the picker's thread, which can outlive the test
    auto release = std::make_shared<std::atomic<bool>>(false);
    FileDialogs::Picker previous = FileDialogs::picker;
    fs::path file = tempFile("slow.txt");
    Files::writeFile(file, "x");
    FileDialogs::picker = [release, file](HWND, bool, const std::wstring&, const std::optional<FileDialogs::Filter>&) {
        while (!*release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return file;
    };
    Extensions::Extension* ext = TestExtension::load("dialog_one", R"(
        RegisterKeybind(Keys.F12, function()
            _G.first = OpenFileDialog()
            _G.second = OpenFileDialog()
            _G.bad = OpenFileDialog({ "JSON", "json" })
        end, KeybindModifiers.Ctrl)
    )");
    REQUIRE(ext != nullptr);
    ext->keys[0].callback();
    CHECK((*ext->luaState)["first"].get_type() == sol::type::number);
    CHECK_FALSE((*ext->luaState)["second"].valid());
    CHECK_FALSE((*ext->luaState)["bad"].valid());
    *release = true;

    CHECK(FileDialogs::filterFrom("JSON", "*.json"));
    CHECK(FileDialogs::filterFrom("Pictures", "*.png;*.jpg"));
    CHECK_FALSE(FileDialogs::filterFrom("", "*.json"));
    CHECK_FALSE(FileDialogs::filterFrom("JSON", "*.*"));
    CHECK_FALSE(FileDialogs::filterFrom("JSON", "*.json|*.exe"));
    CHECK_FALSE(FileDialogs::filterFrom(std::string(65, 'a'), "*.json"));

    // its window still open when it unloads: the answer goes nowhere
    TestExtension::unloadAll();
    for (int i = 0; i < 400 && FileDialogs::open(); i++) {
        LuaFunctions::UpdateFileDialogs();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(FileDialogs::open());
    LuaFunctions::UpdateFileDialogs();
    FileDialogs::picker = previous;
}

TEST_CASE("a file name that isn't UTF-8 still opens the save window") {
    FileDialogs::Picker previous = FileDialogs::picker;
    std::wstring seen;
    FileDialogs::picker = [&seen](HWND, bool, const std::wstring& name, const std::optional<FileDialogs::Filter>&) {
        seen = name;
        return fs::path();
    };
    auto answer = FileDialogs::start(nullptr, true, "folder/\xff.json", "data", std::nullopt);
    REQUIRE(answer != nullptr);
    for (int i = 0; i < 400 && !answer->done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(answer->done);
    CHECK(answer->error == "cancelled");
    CHECK(seen == L"\xFFFD" L".json");
    CHECK_FALSE(FileDialogs::open());
    LuaFunctions::UpdateFileDialogs();
    FileDialogs::picker = previous;
}
