#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "asks.hpp"
#include "extensions.hpp"
#include "files.hpp"
#include "keybindings.hpp"
#include "settings.hpp"
#include "test_extension.hpp"

// The question window's queue, what each answer saves, and what PermissionState
// tells an extension.

namespace fs = std::filesystem;
using Permission = Extensions::ExtensionPermissionTypes;
using Asks::Answer;
using Asks::Kind;

namespace {
    struct CaptureLog {
        std::ostringstream text;
        std::streambuf* previous = std::cerr.rdbuf(text.rdbuf());
        ~CaptureLog() { std::cerr.rdbuf(previous); }
        bool has(const std::string& part) const { return text.str().find(part) != std::string::npos; }
    };

    nlohmann::json savedFor(const std::string& folder) {
        nlohmann::json all = nlohmann::json::parse(Files::readText(Settings::file));
        return all["extensionGrants"][folder];
    }

    // an extension that isn't loaded from a folder, with the grants saved for it
    struct Bare {
        Extensions::Extension ext;
        Bare(const std::string& folder, const std::string& hash = "h1") {
            ext.name = ext.folderKey = folder;
            ext.author = "someone";
            ext.extensionHash = hash;
            Extensions::loadExtensionPermissions(&ext);
        }
        ~Bare() { Asks::forget(ext); }
    };

    struct Fresh {
        Fresh() {
            fs::remove(Settings::file);
            Asks::clear();
        }
        ~Fresh() { Asks::clear(); }
    };
}

TEST_CASE("one question per extension and kind, a second ask joins it") {
    Fresh fresh;
    Bare a("a"), b("b");
    Asks::askPermission(a.ext, Permission::NetworkAccess);
    Asks::askPermission(b.ext, Permission::NetworkAccess);
    Asks::askPermission(a.ext, Permission::MouseAnywhere);
    Asks::askPermission(a.ext, Permission::NetworkAccess);

    REQUIRE(Asks::queue().size() == 2);
    CHECK(Asks::queue()[0].ext == &a.ext);
    CHECK(Asks::queue()[0].permissions == std::vector<Permission>{ Permission::NetworkAccess, Permission::MouseAnywhere });
    CHECK(Asks::queue()[1].ext == &b.ext);

    // nothing for what it has already
    Asks::askPermission(b.ext, Permission::DisplayOverlay);
    CHECK(Asks::queue()[1].permissions.size() == 1);
}

TEST_CASE("a question waits until the big map or the menu is open") {
    Fresh fresh;
    Bare a("a");
    Asks::askPermission(a.ext, Permission::NetworkAccess);
    CHECK(Asks::showing(false, false) == nullptr);
    CHECK(Asks::showing(true, false) == &Asks::queue().front());
    CHECK(Asks::showing(false, true) == &Asks::queue().front());
}

TEST_CASE("Don't allow holds until the next start, Never ask again until Let it ask") {
    Fresh fresh;
    {
        Bare a("a");
        Asks::askPermission(a.ext, Permission::NetworkAccess);
        Asks::answer(Answer::DontAllow);
        CHECK(Extensions::permissionState(a.ext, Permission::NetworkAccess) == "refused");
        Asks::askPermission(a.ext, Permission::NetworkAccess);
        CHECK(Asks::queue().empty());
    }
    {
        // the next start
        Bare a("a");
        CHECK(Extensions::permissionState(a.ext, Permission::NetworkAccess) == "not_asked");
        Asks::askPermission(a.ext, Permission::NetworkAccess);
        Asks::answer(Answer::Never);
        CHECK(savedFor("a")["never"] == std::vector<std::string>{ "network" });
    }
    {
        Bare a("a");
        CHECK(Extensions::permissionState(a.ext, Permission::NetworkAccess) == "never");
        Asks::askPermission(a.ext, Permission::NetworkAccess);
        CHECK(Asks::queue().empty());

        Extensions::letAsk(a.ext, "network");
        Extensions::saveExtensionPermissions(a.ext);
        CHECK(Extensions::permissionState(a.ext, Permission::NetworkAccess) == "not_asked");
        Asks::askPermission(a.ext, Permission::NetworkAccess);
        CHECK(Asks::queue().size() == 1);
        Asks::answer(Answer::Allow);
        CHECK(Extensions::permissionState(a.ext, Permission::NetworkAccess) == "allowed");
    }
    Bare a("a");
    CHECK(a.ext.extensionPermissions.count(Permission::NetworkAccess) == 1);
    CHECK_FALSE(savedFor("a").contains("never"));
}

TEST_CASE("a permission switched off on the tab is never asked for again") {
    Fresh fresh;
    {
        Bare a("a");
        Extensions::switchPermission(a.ext, Permission::DisplayOverlay, false);
        Extensions::saveExtensionPermissions(a.ext);
    }
    Bare a("a");
    CHECK(Extensions::permissionState(a.ext, Permission::DisplayOverlay) == "off");
    Asks::askPermission(a.ext, Permission::DisplayOverlay);
    CHECK(Asks::queue().empty());

    Extensions::switchPermission(a.ext, Permission::DisplayOverlay, true);
    CHECK(Extensions::permissionState(a.ext, Permission::DisplayOverlay) == "allowed");
    CHECK(a.ext.switchedOff.empty());
}

TEST_CASE("PermissionState gives its five answers to Lua, nil for an unknown name") {
    Fresh fresh;
    Extensions::Extension* ext = TestExtension::load("states", "function Init() end");
    REQUIRE(ext != nullptr);
    Extensions::refuse(*ext, Permission::NetworkAccess, false);
    Extensions::refuse(*ext, Permission::MouseAnywhere, true);
    Extensions::switchPermission(*ext, Permission::DisplayRead, false);

    CaptureLog log;
    REQUIRE(TestExtension::run(ext, R"(
        allowed = PermissionState("display_overlay")
        refused = PermissionState("network")
        never = PermissionState("mouse_anywhere")
        off = PermissionState("display_read")
        unknown = PermissionState("everything") == nil
        AskForPermission("everything")
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["allowed"].get<std::string>() == "allowed");
    CHECK(lua["refused"].get<std::string>() == "refused");
    CHECK(lua["never"].get<std::string>() == "never");
    CHECK(lua["off"].get<std::string>() == "off");
    CHECK(lua["unknown"].get<bool>() == true);
    CHECK(log.has("knows no permission named everything"));
    CHECK(Asks::queue().empty());

    Extensions::letAsk(*ext, "mouse_anywhere");
    ext->refused.clear();
    REQUIRE(TestExtension::run(ext, R"(
        notAsked = PermissionState("mouse_anywhere")
        AskForPermission("mouse_anywhere")
    )"));
    CHECK(lua["notAsked"].get<std::string>() == "not_asked");
    CHECK(Asks::queue().size() == 1);
    TestExtension::unloadAll();
    // its question went with it
    CHECK(Asks::queue().empty());
}

TEST_CASE("a changed extension is asked to allow the same again, and for what's new after it") {
    Fresh fresh;
    {
        Bare a("a");
        Extensions::allow(a.ext, Permission::NetworkAccess);
        Extensions::saveExtensionPermissions(a.ext);
    }
    {
        Bare a("a", "h2");
        CHECK(a.ext.extensionPermissions.count(Permission::NetworkAccess) == 0);
        Asks::askPermission(a.ext, Permission::MouseAnywhere);
        Asks::askChanged(a.ext);
        // what it had is in "Allow the same" only
        Asks::askPermission(a.ext, Permission::NetworkAccess);
        REQUIRE(Asks::queue().size() == 2);
        CHECK(Asks::queue()[0].kind == Kind::Changed);
        CHECK(Asks::queue()[0].permissions.empty());
        CHECK(Asks::queue()[1].kind == Kind::Permissions);
        CHECK(Asks::queue()[1].permissions == std::vector<Permission>{ Permission::MouseAnywhere });

        // Not now: asked again at the next start
        Asks::answer(Answer::NotNow);
        CHECK(savedFor("a")["hash"] == "h1");
        Asks::answer(Answer::DontAllow);
        CHECK(Extensions::permissionState(a.ext, Permission::MouseAnywhere) == "refused");
    }
    Bare a("a", "h2");
    Asks::askChanged(a.ext);
    Asks::askPermission(a.ext, Permission::MouseAnywhere);
    REQUIRE(Asks::queue().size() == 2);
    // allowing the same doesn't allow the new one
    Asks::answer(Answer::Allow);
    CHECK(a.ext.extensionPermissions.count(Permission::NetworkAccess) == 1);
    CHECK(a.ext.extensionPermissions.count(Permission::MouseAnywhere) == 0);
    CHECK(savedFor("a")["hash"] == "h2");
    CHECK(a.ext.grantsBeforeChange.empty());
    Asks::answer(Answer::Allow);
    CHECK(a.ext.extensionPermissions.count(Permission::MouseAnywhere) == 1);
}

TEST_CASE("what's allowed while the same again waits is kept") {
    Fresh fresh;
    {
        Bare a("a");
        Extensions::allow(a.ext, Permission::NetworkAccess);
        Extensions::saveExtensionPermissions(a.ext);
    }
    {
        Bare a("a", "h2");
        Asks::askChanged(a.ext);
        Asks::askPermission(a.ext, Permission::MouseAnywhere);
        Asks::answer(Answer::NotNow);
        Asks::answer(Answer::Allow);
        CHECK(savedFor("a")["hash"] == "h1");
    }
    // at the next start it's part of the same again
    Bare a("a", "h2");
    CHECK(a.ext.extensionPermissions.count(Permission::MouseAnywhere) == 0);
    Asks::askChanged(a.ext);
    Asks::answer(Answer::Allow);
    CHECK(a.ext.extensionPermissions.count(Permission::NetworkAccess) == 1);
    CHECK(a.ext.extensionPermissions.count(Permission::MouseAnywhere) == 1);
}

TEST_CASE("a changed extension that only had the defaults isn't asked") {
    Fresh fresh;
    {
        Bare a("a");
        Extensions::saveExtensionPermissions(a.ext);
    }
    Bare a("a", "h2");
    Asks::askChanged(a.ext);
    CHECK(Asks::queue().empty());
    CHECK(savedFor("a")["hash"] == "h2");
}

TEST_CASE("an overlay setting is asked like a permission and turned on by the answer") {
    Fresh fresh;
    bool on = false;
    Asks::addSetting({ "Test setting", "Do the test thing", "It only tests.", [&]() { return on; }, [&]() { on = true; } });
    Bare a("a"), b("b");
    Asks::askSetting(a.ext, "Test setting");
    Asks::askSetting(a.ext, "Test setting");
    Asks::askSetting(a.ext, "Nothing like it");
    REQUIRE(Asks::queue().size() == 1);
    Asks::answer(Answer::Never);
    CHECK(Asks::settingState(a.ext, "Test setting") == "never");
    CHECK(savedFor("a")["never"] == std::vector<std::string>{ "Test setting" });

    Asks::askSetting(b.ext, "Test setting");
    Asks::answer(Answer::Allow);
    CHECK(on);
    CHECK(Asks::settingState(b.ext, "Test setting") == "allowed");
    // nothing asked once it's on
    Asks::askSetting(a.ext, "Test setting");
    CHECK(Asks::queue().empty());
}

TEST_CASE("an extension switched off stops now and isn't run at the next start, switched on it starts now") {
    Fresh fresh;
    // Update is only found when main.lua runs, which a switched-off extension skips
    const std::string code = "function Update() end";
    Extensions::Extension* ext = TestExtension::load("switch", code);
    REQUIRE(ext != nullptr);
    CHECK(ext->updateFunction);
    Asks::askPermission(*ext, Permission::NetworkAccess);

    Extensions::setRunning(*ext, false);
    CHECK(ext->stopped);
    CHECK_FALSE(ext->updateFunction);
    CHECK(Asks::queue().empty());
    CHECK(savedFor("switch")["enabled"] == false);
    TestExtension::unloadAll();

    ext = TestExtension::load("switch", code);
    REQUIRE(ext != nullptr);
    CHECK(ext->stopped);
    CHECK_FALSE(ext->updateFunction);

    // on and off again before the next frame, it stays off
    Extensions::setRunning(*ext, true);
    Extensions::setRunning(*ext, false);
    TestExtension::turn();
    REQUIRE(Extensions::registeredExtensions.size() == 1);
    CHECK(Extensions::registeredExtensions.back().get() == ext);
    CHECK(ext->stopped);

    // switched on, it runs from the next frame
    Extensions::setRunning(*ext, true);
    TestExtension::turn();
    REQUIRE(Extensions::registeredExtensions.size() == 1);
    ext = Extensions::registeredExtensions.back().get();
    CHECK_FALSE(ext->stopped);
    CHECK(ext->updateFunction);
    CHECK_FALSE(savedFor("switch").contains("enabled"));
    TestExtension::unloadAll();
}

TEST_CASE("an extension folder added while the overlay runs starts, once its files stop changing") {
    Fresh fresh;
    fs::path root = TestExtension::folder("added") / "extensions";
    fs::remove_all(root);
    fs::create_directories(root / "late");
    std::ofstream(root / "late" / "main.lua", std::ios::binary)
        << "function RegisterSettings() DefineExtensionSetting('size', 'Size', 'int', 3) end\n"
           "function Init() error('ran with ' .. GetSetting('size')) end";
    // a folder without main.lua isn't one
    fs::create_directories(root / "notes");

    // just seen: it may still be copying, even with the old times a copy keeps
    fs::last_write_time(root / "late" / "main.lua", fs::file_time_type::clock::now() - std::chrono::hours(5));
    CHECK(Extensions::findNewExtensions(root, std::chrono::seconds(60)) == 0);
    CHECK(Extensions::registeredExtensions.empty());

    CHECK(Extensions::findNewExtensions(root, std::chrono::seconds(0)) == 1);
    REQUIRE(Extensions::registeredExtensions.size() == 1);
    Extensions::Extension* ext = Extensions::registeredExtensions.back().get();
    CHECK(ext->folderKey == "late");
    // Init ran, its setting already there
    CHECK(ext->startError.find("ran with 3") == 0);

    // found once
    CHECK(Extensions::findNewExtensions(root, std::chrono::seconds(0)) == 0);
    CHECK(Extensions::registeredExtensions.size() == 1);
    TestExtension::unloadAll();
    fs::remove_all(root);
}

TEST_CASE("forgetting one kind of question keeps the others") {
    Fresh fresh;
    bool on = false;
    Asks::addSetting({ "Kind setting", "Do it", "It tests.", [&]() { return on; }, [&]() { on = true; } });
    Bare a("a");
    Asks::askPermission(a.ext, Permission::NetworkAccess);
    Asks::askSetting(a.ext, "Kind setting");
    REQUIRE(Asks::queue().size() == 2);
    Asks::forget(a.ext, Kind::Permissions);
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].kind == Kind::Setting);
}

TEST_CASE("a key waits until the player allows it, and a new key asks again") {
    Fresh fresh;
    Keybindings::keybinds.clear();
    Extensions::Extension* ext = TestExtension::load("keys", "function Init() end");
    REQUIRE(ext != nullptr);
    CHECK(Extensions::permissionState(*ext, Permission::Keybinds) == "not_asked");
    REQUIRE(TestExtension::run(ext, "f5 = RegisterKeybind(Keys.F5, function() end, KeybindModifiers.Ctrl)"));
    CHECK((*ext->luaState)["f5"] != sol::nil);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5, Keybindings::KeybindModifiers_Ctrl) == nullptr);
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].permissions == std::vector<Permission>{ Permission::Keybinds });
    CHECK(Asks::queue()[0].keys == std::vector<std::string>{ "Ctrl+F5" });

    Asks::answer(Answer::Allow);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5, Keybindings::KeybindModifiers_Ctrl) != nullptr);
    CHECK(savedFor("keys")["keys"] == std::vector<std::string>{ "Ctrl+F5" });

    // only the new one is asked for
    REQUIRE(TestExtension::run(ext, "ins = RegisterKeybind(Keys.Insert, function() end, KeybindModifiers.Alt)"));
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].permissions.empty());
    CHECK(Asks::queue()[0].keys == std::vector<std::string>{ "Alt+Insert" });
    // a waiting key can be deleted, and leaves the question
    REQUIRE(TestExtension::run(ext, "deleted = DeleteKeybind(ins)"));
    CHECK((*ext->luaState)["deleted"].get<bool>());
    CHECK(ext->keys.size() == 1);
    CHECK(Asks::queue().empty());

    // switched off on the tab, the key is let go; switched on, it's back
    Extensions::switchPermission(*ext, Permission::Keybinds, false);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5, Keybindings::KeybindModifiers_Ctrl) == nullptr);
    Extensions::switchPermission(*ext, Permission::Keybinds, true);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5, Keybindings::KeybindModifiers_Ctrl) != nullptr);
    TestExtension::unloadAll();
    CHECK(Keybindings::keybinds.empty());
}

// without admin, only keys with Alt or Ctrl reach the overlay while the game is in front
TEST_CASE("a key needs Alt or Ctrl") {
    Fresh fresh;
    Keybindings::keybinds.clear();
    Extensions::Extension* ext = TestExtension::load("keys_plain", "function Init() end");
    REQUIRE(ext != nullptr);
    CaptureLog log;
    REQUIRE(TestExtension::run(ext, R"(
        plain = RegisterKeybind(Keys.J, function() end)
        shift = RegisterKeybind(Keys.J, function() end, KeybindModifiers.Shift)
        f6 = RegisterKeybind(Keys.F6, function() end)
        alt = RegisterKeybind(Keys.J, function() end, KeybindModifiers.Alt)
        ctrl = RegisterKeybind(Keys.F6, function() end, KeybindModifiers.Ctrl + KeybindModifiers.Shift)
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["plain"].get<std::string>().empty());
    CHECK(lua["shift"].get<std::string>().empty());
    CHECK(lua["f6"].get<std::string>().empty());
    CHECK(log.has("J needs Alt or Ctrl"));
    CHECK(log.has("Shift+J needs Alt or Ctrl"));
    CHECK(log.has("F6 needs Alt or Ctrl"));
    CHECK_FALSE(lua["alt"].get<std::string>().empty());
    CHECK_FALSE(lua["ctrl"].get<std::string>().empty());
    // only the two that can work are asked for
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].keys == std::vector<std::string>{ "Alt+J", "Ctrl+Shift+F6" });
    TestExtension::unloadAll();
}

TEST_CASE("an extension can't take a key Windows or the menu keeps") {
    Fresh fresh;
    Keybindings::keybinds.clear();
    Extensions::Extension* ext = TestExtension::load("keys_kept", "function Init() end");
    REQUIRE(ext != nullptr);
    CaptureLog log;
    REQUIRE(TestExtension::run(ext, R"(
        menu = RegisterKeybind(Keys.Grave, function() end, KeybindModifiers.Alt)
        space = RegisterKeybind(Keys.Space, function() end, KeybindModifiers.Alt)
        ctrlSpace = RegisterKeybind(Keys.Space, function() end, KeybindModifiers.Ctrl)
    )"));
    sol::state& lua = *ext->luaState;
    CHECK(lua["menu"].get<std::string>().empty());
    CHECK(lua["space"].get<std::string>().empty());
    CHECK(log.has("Alt+` is kept for the overlay's menu"));
    CHECK(log.has("Alt+Space is kept for Windows"));
    CHECK_FALSE(lua["ctrlSpace"].get<std::string>().empty());
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].keys == std::vector<std::string>{ "Ctrl+Space" });
    TestExtension::unloadAll();
}

TEST_CASE("Keybinds switched on on the tab asks again for keys said no to") {
    Fresh fresh;
    Keybindings::keybinds.clear();
    Extensions::Extension* ext = TestExtension::load("keys_no", "function Init() end");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, "RegisterKeybind(Keys.F5, function() end, KeybindModifiers.Alt)"));
    Asks::answer(Answer::DontAllow);
    CHECK(Asks::queue().empty());

    Extensions::switchPermission(*ext, Permission::Keybinds, true);
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].keys == std::vector<std::string>{ "Alt+F5" });
    Asks::answer(Answer::Allow);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5, Keybindings::KeybindModifiers_Alt) != nullptr);
    TestExtension::unloadAll();
}

TEST_CASE("keys of a changed extension wait for the same again") {
    Fresh fresh;
    Keybindings::keybinds.clear();
    {
        Bare a("a");
        Extensions::allow(a.ext, Permission::Keybinds);
        a.ext.allowedKeys = { "F5" };
        Extensions::saveExtensionPermissions(a.ext);
    }
    Bare a("a", "h2");
    Asks::askChanged(a.ext);
    // F5 was allowed before, F6 is new: neither is asked while the same again waits
    Asks::askKeys(a.ext, "F6");
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].kind == Kind::Changed);

    Asks::answer(Answer::Allow);
    CHECK(a.ext.extensionPermissions.count(Permission::Keybinds) == 1);
}

TEST_CASE("after the same again, the allowed keys are bound and only the new one is asked") {
    Fresh fresh;
    Keybindings::keybinds.clear();
    Extensions::Extension* ext = TestExtension::load("same_keys",
        "function Init() RegisterKeybind(Keys.F5, function() end, KeybindModifiers.Alt) end");
    REQUIRE(ext != nullptr);
    Extensions::runForExtension(ext, ext->initializeFunction);
    Asks::answer(Answer::Allow);
    TestExtension::unloadAll();

    // its files changed, and it takes one more key
    ext = TestExtension::load("same_keys", R"(
        function Init()
            RegisterKeybind(Keys.F5, function() end, KeybindModifiers.Alt)
            RegisterKeybind(Keys.F6, function() end, KeybindModifiers.Alt)
        end
    )");
    REQUIRE(ext != nullptr);
    Extensions::runForExtension(ext, ext->initializeFunction);
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].kind == Kind::Changed);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5, Keybindings::KeybindModifiers_Alt) == nullptr);

    Asks::answer(Answer::Allow);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5, Keybindings::KeybindModifiers_Alt) != nullptr);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F6, Keybindings::KeybindModifiers_Alt) == nullptr);
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].kind == Kind::Permissions);
    CHECK(Asks::queue()[0].keys == std::vector<std::string>{ "Alt+F6" });

    Asks::answer(Answer::Allow);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F6, Keybindings::KeybindModifiers_Alt) != nullptr);
    TestExtension::unloadAll();
}

TEST_CASE("an allowed key is bound at the next start without asking") {
    Fresh fresh;
    Keybindings::keybinds.clear();
    const std::string code = "function Init() RegisterKeybind(Keys.F6, function() end, KeybindModifiers.Alt) end";
    Extensions::Extension* ext = TestExtension::load("keys_again", code);
    REQUIRE(ext != nullptr);
    Extensions::runForExtension(ext, ext->initializeFunction);
    Asks::answer(Answer::Allow);
    TestExtension::unloadAll();

    ext = TestExtension::load("keys_again", code);
    REQUIRE(ext != nullptr);
    Extensions::runForExtension(ext, ext->initializeFunction);
    CHECK(Asks::queue().empty());
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F6, Keybindings::KeybindModifiers_Alt) != nullptr);
    TestExtension::unloadAll();
}

TEST_CASE("what main.lua lists is asked for at the first start, with the author's reasons") {
    Fresh fresh;
    const std::string listed = R"(
        metadata = { name = "Lists", permissions = {
            network = { reason = "Syncs your marks." },
            mouse_anywhere = {},
            display_overlay = { reason = "Draws the marks." },
            everything = { reason = "?" },
            internal_read = "no table",
        } }
    )";
    CaptureLog log;
    Extensions::Extension* ext = TestExtension::load("lists", listed);
    REQUIRE(ext != nullptr);
    CHECK(ext->reasons.at(Permission::NetworkAccess) == "Syncs your marks.");
    CHECK(ext->reasons.at(Permission::MouseAnywhere).empty());
    CHECK(ext->reasons.at(Permission::DisplayOverlay) == "Draws the marks.");
    CHECK(log.has("metadata.permissions: knows no permission named everything"));
    CHECK(log.has("metadata.permissions.internal_read should be a table"));
    // one question, only what it doesn't have yet
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].permissions == std::vector<Permission>{ Permission::NetworkAccess, Permission::MouseAnywhere });
    Asks::answer(Answer::DontAllow);
    TestExtension::unloadAll();

    // answered, it isn't asked at the next start
    ext = TestExtension::load("lists", listed);
    REQUIRE(ext != nullptr);
    CHECK(Asks::queue().empty());
    TestExtension::unloadAll();
}

TEST_CASE("an update that lists a new permission asks for it after the same again") {
    Fresh fresh;
    Extensions::Extension* ext = TestExtension::load("updates", "metadata = { permissions = { network = {} } }");
    REQUIRE(ext != nullptr);
    Asks::answer(Answer::Allow);
    TestExtension::unloadAll();

    ext = TestExtension::load("updates", "metadata = { permissions = { network = {}, mouse_anywhere = {} } }");
    REQUIRE(ext != nullptr);
    REQUIRE(Asks::queue().size() == 2);
    CHECK(Asks::queue()[0].kind == Kind::Changed);
    CHECK(Asks::queue()[1].permissions == std::vector<Permission>{ Permission::MouseAnywhere });
    Asks::answer(Answer::Allow);
    CHECK(ext->extensionPermissions.count(Permission::NetworkAccess) == 1);
    CHECK(ext->extensionPermissions.count(Permission::MouseAnywhere) == 0);
    Asks::answer(Answer::Allow);
    CHECK(ext->extensionPermissions.count(Permission::MouseAnywhere) == 1);
    TestExtension::unloadAll();
}

TEST_CASE("an author's reason is one line of at most 200 letters, \"...\" where it's cut") {
    Fresh fresh;
    std::string longer;
    for (int i = 0; i < 300; i++) longer += "\xC3\xA9";
    Extensions::Extension* ext = TestExtension::load("reasons",
        "metadata = { permissions = { network = { reason = \"one\\ntwo##three\" }, mouse_anywhere = { reason = \"" + longer + "\" } } }");
    REQUIRE(ext != nullptr);
    CHECK(ext->reasons.at(Permission::NetworkAccess) == "one two# #three");
    CHECK(ext->reasons.at(Permission::MouseAnywhere) == longer.substr(0, 400) + "...");
    TestExtension::unloadAll();
}

TEST_CASE("network with sites reaches only them, and the question says which") {
    Fresh fresh;
    CaptureLog log;
    Extensions::Extension* ext = TestExtension::load("sites", R"(
        metadata = { name = "Sites", permissions = { network = { sites = { "HoYoLAB.com", "not a site", "a..b", 5 } } } }
    )");
    REQUIRE(ext != nullptr);
    CHECK(ext->sitesOnly);
    CHECK(ext->sites == std::vector<std::string>{ "hoyolab.com" });
    CHECK(log.has("metadata.permissions.network.sites: not a site isn't a site name"));
    CHECK(log.has("metadata.permissions.network.sites: a..b isn't a site name"));
    const Extensions::PermissionInfo* network = Extensions::findPermission(Permission::NetworkAccess);
    CHECK(Extensions::reasonLine(*ext, *network) == "It can talk to: hoyolab.com");

    Asks::answer(Answer::Allow);
    REQUIRE(TestExtension::run(ext, R"(got = Net_Get("http://example.com/") == nil)"));
    CHECK((*ext->luaState)["got"].get<bool>());
    CHECK(log.has("can't reach example.com: its main.lua lists only hoyolab.com"));
    TestExtension::unloadAll();

    // without sites it may talk to any
    ext = TestExtension::load("any_site", "metadata = { permissions = { network = {} } }");
    REQUIRE(ext != nullptr);
    CHECK_FALSE(ext->sitesOnly);
    CHECK(Extensions::reasonLine(*ext, *network) == network->reason);
    TestExtension::unloadAll();
}

TEST_CASE("main.lua reaches no site before its sites are read") {
    Fresh fresh;
    const std::string mainLua = R"(
        Net_Get("http://example.com/")
        metadata = { permissions = { network = { sites = { "hoyolab.com" } } } }
    )";
    Extensions::Extension* ext = TestExtension::load("early", mainLua);
    REQUIRE(ext != nullptr);
    Asks::answer(Answer::Allow);
    TestExtension::unloadAll();

    // allowed now: the call at the top of main.lua runs before the list is read
    CaptureLog log;
    ext = TestExtension::load("early", mainLua);
    REQUIRE(ext != nullptr);
    CHECK(ext->extensionPermissions.count(Permission::NetworkAccess) == 1);
    CHECK(log.has("can't reach example.com while main.lua is still loading"));
    TestExtension::unloadAll();
}

TEST_CASE("sites it can't read let it reach none") {
    Fresh fresh;
    CaptureLog log;
    Extensions::Extension* ext = TestExtension::load("bad_sites", R"(
        metadata = { permissions = { network = { sites = "hoyolab.com" } } }
    )");
    REQUIRE(ext != nullptr);
    CHECK(ext->sitesOnly);
    CHECK(ext->sites.empty());
    CHECK(log.has("metadata.permissions.network.sites should be a list"));
    TestExtension::unloadAll();
}

TEST_CASE("notices and dots are on without asking and can be switched off") {
    Fresh fresh;
    {
        Bare a("a");
        CHECK(Extensions::permissionState(a.ext, Permission::Notices) == "allowed");
        CHECK(Extensions::permissionState(a.ext, Permission::Dots) == "allowed");
        Extensions::switchPermission(a.ext, Permission::Dots, false);
        Extensions::saveExtensionPermissions(a.ext);
    }
    Bare a("a");
    CHECK(Extensions::permissionState(a.ext, Permission::Notices) == "allowed");
    CHECK(Extensions::permissionState(a.ext, Permission::Dots) == "off");
    CHECK(Extensions::findPermission("notices")->type == Permission::Notices);
}

TEST_CASE("the permissions an extension's code calls for are found, comments left out") {
    // fills the list of functions
    auto lua = Extensions::newLuaState(TestExtension::folder("calls"));
    using Extensions::callsFor;
    CHECK(callsFor("ClickArea(1, 2, 3, 4)") == std::unordered_set<Permission>{ Permission::TakeClicks });
    CHECK(callsFor("local socket = WebSocket.new()") == std::unordered_set<Permission>{ Permission::NetworkAccess });
    CHECK(callsFor("RegisterKeybind(Keys.F5, f, KeybindModifiers.Alt)") == std::unordered_set<Permission>{ Permission::Keybinds });
    CHECK(callsFor("AddTrigger('MapView', f)") == std::unordered_set<Permission>{ Permission::MapInfo });
    CHECK(callsFor("AskForPermission([[mouse_anywhere]])") == std::unordered_set<Permission>{ Permission::MouseAnywhere });
    CHECK(callsFor("-- ClickArea(1, 2, 3, 4)\n--[==[ Net_Get(x) ]==]\nlocal ClickAreas = 1").empty());
    CHECK(callsFor("print(\"ClickArea \\\" Net_Get\")").empty());
    CHECK(callsFor("x = 1 -- ClickArea(1, 2, 3, 4)\nNet_Get(x)") == std::unordered_set<Permission>{ Permission::NetworkAccess });
}

TEST_CASE("a permission refused while main.lua's top level runs is asked for once it has loaded") {
    Fresh fresh;
    Extensions::Extension* ext = TestExtension::load("top_level", "Net_Get('http://127.0.0.1:9/')");
    REQUIRE(ext != nullptr);
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].ext == ext);
    CHECK(Asks::queue()[0].kind == Kind::Permissions);
    CHECK(Asks::queue()[0].permissions == std::vector<Permission>{ Permission::NetworkAccess });
    CHECK(ext->askWhenLoaded.empty());
    TestExtension::unloadAll();
}

TEST_CASE("the Permissions tab lists what any Lua file calls for") {
    Fresh fresh;
    Extensions::Extension* ext = TestExtension::load("calls", "require('draw')",
        { { "draw.lua", "function draw() ClickArea(0, 0, 10, 10) end\nfunction connect() return WebSocket.new() end" } });
    REQUIRE(ext != nullptr);
    CHECK(ext->uses == std::unordered_set<Permission>{ Permission::TakeClicks, Permission::NetworkAccess });
    // found in the files, not asked for until it's called
    CHECK(Asks::queue().empty());
    TestExtension::unloadAll();
}

TEST_CASE("a call its files didn't show is asked for when it's refused, and listed from then on") {
    Fresh fresh;
    // the name is put together, the scan can't see it
    const std::string get = "_G['Net' .. '_Get']('http://127.0.0.1:9/')";
    Extensions::Extension* ext = TestExtension::load("hidden", "");
    REQUIRE(ext != nullptr);
    CHECK(ext->uses.empty());
    REQUIRE(TestExtension::run(ext, get));
    CHECK(ext->uses == std::unordered_set<Permission>{ Permission::NetworkAccess });
    REQUIRE(Asks::queue().size() == 1);
    CHECK(Asks::queue()[0].permissions == std::vector<Permission>{ Permission::NetworkAccess });
    Asks::answer(Answer::DontAllow);
    // refused this start: not asked again
    REQUIRE(TestExtension::run(ext, get));
    CHECK(Asks::queue().empty());
    TestExtension::unloadAll();

    ext = TestExtension::load("hidden", "");
    REQUIRE(ext != nullptr);
    CHECK(ext->uses == std::unordered_set<Permission>{ Permission::NetworkAccess });
    TestExtension::unloadAll();
}
