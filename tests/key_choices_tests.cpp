#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "asks.hpp"
#include "extensions.hpp"
#include "files.hpp"
#include "key_choices.hpp"
#include "keybindings.hpp"
#include "settings.hpp"
#include "test_extension.hpp"

namespace fs = std::filesystem;
using Permission = Extensions::ExtensionPermissionTypes;
using Keybindings::KeybindModifiers_Alt;
using Keybindings::KeybindModifiers_Ctrl;

namespace {
    struct Fresh {
        Fresh() {
            fs::remove(Settings::file);
            Asks::clear();
            Keybindings::keybinds.clear();
        }
        ~Fresh() {
            TestExtension::unloadAll();
            Asks::clear();
        }
    };

    // loaded with Keybinds allowed, then registers its key
    Extensions::Extension* withKey(const std::string& folder, const std::string& registers) {
        Extensions::Extension* ext = TestExtension::load(folder, "function Init() end");
        if (!ext) return nullptr;
        Extensions::allow(*ext, Permission::Keybinds);
        if (!TestExtension::run(ext, "id = " + registers)) return nullptr;
        return ext;
    }

    const Extensions::Extension::HeldKey& only(Extensions::Extension* ext) {
        REQUIRE(ext->keys.size() == 1);
        return ext->keys[0];
    }

    std::string id(Extensions::Extension* ext) {
        return (*ext->luaState)["id"].get<std::string>();
    }

    bool bound(int key, int modifiers) {
        return Keybindings::DoesKeybindExist(key, modifiers) != nullptr;
    }

    nlohmann::json picked(const std::string& folder) {
        if (!fs::exists(Settings::file)) return nullptr;
        nlohmann::json all = nlohmann::json::parse(Files::readText(Settings::file));
        return all["extensionGrants"][folder]["picked"];
    }
}

TEST_CASE("a new extension with a key another one has starts without it") {
    Fresh fresh;
    Extensions::Extension* first = withKey("keys_first", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt, 'Open map')");
    REQUIRE(first != nullptr);
    CHECK(only(first).label == "Open map");
    CHECK(only(first).name == "Alt+M");
    first->allowedKeys = { "Alt+M" };
    Extensions::bindAllowedKeys(*first);
    REQUIRE(bound(GLFW_KEY_M, KeybindModifiers_Alt));
    Asks::clear();

    Extensions::Extension* second = withKey("keys_second", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(second != nullptr);
    // it gets its id, the key just isn't set
    CHECK_FALSE(id(second).empty());
    CHECK(only(second).key == 0);
    CHECK(only(second).name.empty());
    CHECK(only(second).authorName == "Alt+M");
    // nothing to ask for
    CHECK(Asks::queue().empty());
    CHECK(picked("keys_second") == nlohmann::json{ { "Alt+M", "" } });

    std::optional<KeyChoices::Holder> holder = KeyChoices::holderOf(GLFW_KEY_M, KeybindModifiers_Alt);
    REQUIRE(holder);
    CHECK(holder->ext == first);
    CHECK(KeyChoices::describe(*holder) == first->name + " (Open map)");

    // the next start loads them the other way round, the one without stays without
    TestExtension::unloadAll();
    second = withKey("keys_second", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    first = withKey("keys_first", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt, 'Open map')");
    REQUIRE(second != nullptr);
    REQUIRE(first != nullptr);
    CHECK(only(second).key == 0);
    CHECK(only(first).name == "Alt+M");
}

TEST_CASE("the player picks a key, it's allowed, saved and comes back at the next start") {
    Fresh fresh;
    Extensions::Extension* ext = withKey("keys_pick", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(ext != nullptr);
    // the author's key waits to be asked, the picked one doesn't
    CHECK_FALSE(bound(GLFW_KEY_M, KeybindModifiers_Alt));

    KeyChoices::Result result = KeyChoices::pick(*ext, id(ext), GLFW_KEY_N, KeybindModifiers_Ctrl);
    CHECK(result.done());
    CHECK(only(ext).name == "Ctrl+N");
    CHECK(bound(GLFW_KEY_N, KeybindModifiers_Ctrl));
    CHECK(ext->allowedKeys == std::vector<std::string>{ "Ctrl+N" });
    CHECK(Asks::queue().empty());
    CHECK(picked("keys_pick") == nlohmann::json{ { "Alt+M", "Ctrl+N" } });

    TestExtension::unloadAll();
    CHECK_FALSE(bound(GLFW_KEY_N, KeybindModifiers_Ctrl));
    ext = withKey("keys_pick", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(ext != nullptr);
    CHECK(only(ext).name == "Ctrl+N");
    CHECK(bound(GLFW_KEY_N, KeybindModifiers_Ctrl));

    // Reset is the author's key again, which the player puts there, so it's allowed too
    CHECK(KeyChoices::reset(*ext, id(ext)).done());
    CHECK(only(ext).name == "Alt+M");
    CHECK(bound(GLFW_KEY_M, KeybindModifiers_Alt));
    CHECK_FALSE(bound(GLFW_KEY_N, KeybindModifiers_Ctrl));
    CHECK(ext->allowedKeys == std::vector<std::string>{ "Alt+M" });
    CHECK(picked("keys_pick").is_null());

    KeyChoices::unset(*ext, id(ext));
    CHECK(only(ext).key == 0);
    CHECK_FALSE(bound(GLFW_KEY_M, KeybindModifiers_Alt));
    CHECK(picked("keys_pick") == nlohmann::json{ { "Alt+M", "" } });
}

TEST_CASE("a picked key that's taken names who has it, and can be taken from them") {
    Fresh fresh;
    Extensions::Extension* first = withKey("keys_has", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    Extensions::Extension* second = withKey("keys_wants", "RegisterKeybind(Keys.J, function() end, KeybindModifiers.Alt)");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    first->allowedKeys = { "Alt+M" };
    Extensions::bindAllowedKeys(*first);
    REQUIRE(bound(GLFW_KEY_M, KeybindModifiers_Alt));

    KeyChoices::Result result = KeyChoices::pick(*second, id(second), GLFW_KEY_M, KeybindModifiers_Alt);
    CHECK_FALSE(result.done());
    CHECK(result.whyNot.empty());
    REQUIRE(result.takenBy);
    CHECK(result.takenBy->ext == first);
    CHECK(only(second).name == "Alt+J");
    CHECK(only(first).name == "Alt+M");

    result = KeyChoices::give(*second, id(second), GLFW_KEY_M, KeybindModifiers_Alt);
    CHECK(result.done());
    CHECK(only(second).name == "Alt+M");
    CHECK(only(first).key == 0);
    CHECK(picked("keys_has") == nlohmann::json{ { "Alt+M", "" } });
    CHECK(first->allowedKeys.empty());
    // the one bound now runs the second's function
    REQUIRE(bound(GLFW_KEY_M, KeybindModifiers_Alt));
    CHECK(Keybindings::keybinds.size() == 1);

    // its own key again is no change
    CHECK(KeyChoices::pick(*second, id(second), GLFW_KEY_M, KeybindModifiers_Alt).done());
}

TEST_CASE("picking a key turns Keybinds on") {
    Fresh fresh;
    Extensions::Extension* first = withKey("keys_on_first", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(first != nullptr);
    first->allowedKeys = { "Alt+M" };
    Extensions::bindAllowedKeys(*first);
    Asks::clear();

    // started without its key and without Keybinds, so it was never asked
    Extensions::Extension* second = TestExtension::load("keys_on_second", "function Init() end");
    REQUIRE(second != nullptr);
    REQUIRE(TestExtension::run(second, "id = RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)"));
    REQUIRE(only(second).key == 0);
    CHECK(Asks::queue().empty());
    CHECK_FALSE(second->extensionPermissions.count(Permission::Keybinds));

    CHECK(KeyChoices::give(*second, id(second), GLFW_KEY_M, KeybindModifiers_Alt).done());
    CHECK(second->extensionPermissions.count(Permission::Keybinds));
    CHECK(only(second).bound.size() > 0);
    CHECK(Asks::queue().empty());
    nlohmann::json all = nlohmann::json::parse(Files::readText(Settings::file));
    CHECK(all["extensionGrants"]["keys_on_second"]["granted"].dump().find("keybinds") != std::string::npos);
}

TEST_CASE("a picked key needs Alt or Ctrl and can't be one Windows keeps") {
    Fresh fresh;
    Extensions::Extension* ext = withKey("keys_rule", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(ext != nullptr);
    CHECK(KeyChoices::pick(*ext, id(ext), GLFW_KEY_TAB, KeybindModifiers_Alt).whyNot == "Alt+Tab is kept for Windows");
    CHECK(KeyChoices::pick(*ext, id(ext), GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Alt).whyNot == "Alt+` is kept for the overlay's menu");
    CHECK_FALSE(KeyChoices::pick(*ext, id(ext), GLFW_KEY_N, 0).whyNot.empty());
    CHECK(only(ext).name == "Alt+M");
    CHECK(picked("keys_rule").is_null());
}

TEST_CASE("picking the key it already shows turns Keybinds on") {
    Fresh fresh;
    Extensions::Extension* ext = TestExtension::load("keys_same", "function Init() end");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, "id = RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)"));
    REQUIRE(only(ext).name == "Alt+M");
    REQUIRE_FALSE(bound(GLFW_KEY_M, KeybindModifiers_Alt));

    CHECK(KeyChoices::pick(*ext, id(ext), GLFW_KEY_M, KeybindModifiers_Alt).done());
    CHECK(ext->extensionPermissions.count(Permission::Keybinds));
    CHECK(bound(GLFW_KEY_M, KeybindModifiers_Alt));
}

TEST_CASE("a picked key another one has at the next start stays saved") {
    Fresh fresh;
    Extensions::Extension* mine = withKey("keys_mine", "RegisterKeybind(Keys.J, function() end, KeybindModifiers.Alt)");
    REQUIRE(mine != nullptr);
    REQUIRE(KeyChoices::pick(*mine, id(mine), GLFW_KEY_M, KeybindModifiers_Alt).done());
    TestExtension::unloadAll();
    Keybindings::keybinds.clear();

    // another one with Alt+M as its own key starts first
    Extensions::Extension* other = withKey("keys_other", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(other != nullptr);
    other->allowedKeys = { "Alt+M" };
    Extensions::bindAllowedKeys(*other);
    mine = withKey("keys_mine", "RegisterKeybind(Keys.J, function() end, KeybindModifiers.Alt)");
    REQUIRE(mine != nullptr);
    CHECK(only(mine).key == 0);
    CHECK(picked("keys_mine") == nlohmann::json{ { "Alt+J", "Alt+M" } });
}

TEST_CASE("a clash on the first start saves only the key, the next start is still a first one") {
    Fresh fresh;
    Extensions::Extension* first = withKey("keys_early_first", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(first != nullptr);
    first->allowedKeys = { "Alt+M" };
    Extensions::bindAllowedKeys(*first);
    Extensions::Extension* second = TestExtension::load("keys_early_second", "function Init() end");
    REQUIRE(second != nullptr);
    REQUIRE(second->firstStart);
    REQUIRE(TestExtension::run(second, "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)"));
    CHECK(picked("keys_early_second") == nlohmann::json{ { "Alt+M", "" } });
    TestExtension::unloadAll();
    second = TestExtension::load("keys_early_second", "function Init() end");
    REQUIRE(second != nullptr);
    CHECK(second->firstStart);
    CHECK(second->pickedKeys == std::map<std::string, std::string>{ { "Alt+M", "" } });
}

TEST_CASE("a saved key that's no longer allowed to be picked starts without one") {
    Fresh fresh;
    Extensions::Extension* ext = withKey("keys_old", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(ext != nullptr);
    ext->pickedKeys["Alt+M"] = "Alt+F4";
    Extensions::saveExtensionPermissions(*ext);
    TestExtension::unloadAll();
    ext = withKey("keys_old", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(ext != nullptr);
    CHECK(only(ext).key == 0);
    CHECK_FALSE(bound(GLFW_KEY_F4, KeybindModifiers_Alt));
}

TEST_CASE("the menu key can be another one, and holds it like an extension does") {
    Fresh fresh;
    int toggles = 0;
    KeyChoices::startMenuKey([&] { toggles++; });
    CHECK(KeyChoices::menuKeyName() == "Alt+`");
    REQUIRE(bound(GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Alt));

    CHECK(KeyChoices::pickMenuKey(GLFW_KEY_TAB, KeybindModifiers_Alt).whyNot == "Alt+Tab is kept for Windows");
    CHECK(KeyChoices::pickMenuKey(GLFW_KEY_M, KeybindModifiers_Alt).done());
    CHECK(KeyChoices::menuKeyName() == "Alt+M");
    CHECK(Settings::menuKey == "Alt+M");
    CHECK_FALSE(bound(GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Alt));
    Keybindings::DoesKeybindExist(GLFW_KEY_M, KeybindModifiers_Alt)->Press();
    Keybindings::ProcessKeybindings();
    CHECK(toggles == 1);

    // an extension with Alt+M starts without it, and can't take it from the menu
    Extensions::Extension* ext = withKey("keys_menu", "RegisterKeybind(Keys.M, function() end, KeybindModifiers.Alt)");
    REQUIRE(ext != nullptr);
    CHECK(only(ext).key == 0);
    KeyChoices::Result result = KeyChoices::give(*ext, id(ext), GLFW_KEY_M, KeybindModifiers_Alt);
    REQUIRE(result.takenBy);
    CHECK(result.takenBy->menu());
    CHECK(KeyChoices::describe(*result.takenBy) == "the menu");
    CHECK(KeyChoices::menuKeyName() == "Alt+M");
    // Alt+` stays the menu's even while the menu has another key
    CHECK(KeyChoices::pick(*ext, id(ext), GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Alt).whyNot == "Alt+` is kept for the overlay's menu");

    // the menu takes a key from an extension only when told to
    CHECK(KeyChoices::pick(*ext, id(ext), GLFW_KEY_J, KeybindModifiers_Ctrl).done());
    result = KeyChoices::pickMenuKey(GLFW_KEY_J, KeybindModifiers_Ctrl);
    REQUIRE(result.takenBy);
    CHECK(result.takenBy->ext == ext);
    CHECK(KeyChoices::pickMenuKey(GLFW_KEY_J, KeybindModifiers_Ctrl, true).done());
    CHECK(only(ext).key == 0);
    CHECK(KeyChoices::menuKeyName() == "Ctrl+J");

    CHECK(KeyChoices::resetMenuKey().done());
    CHECK(KeyChoices::menuKeyName() == "Alt+`");
    CHECK(Settings::menuKey == "Alt+`");
}

TEST_CASE("a saved menu key that can't be used is Alt+` again") {
    Fresh fresh;
    Settings::menuKey = "Alt+F4";
    KeyChoices::startMenuKey([] {});
    CHECK(KeyChoices::menuKeyName() == "Alt+`");
    Settings::menuKey = "nonsense";
    KeyChoices::startMenuKey([] {});
    CHECK(KeyChoices::menuKeyName() == "Alt+`");
    Settings::menuKey = "Ctrl+Shift+O";
    KeyChoices::startMenuKey([] {});
    CHECK(KeyChoices::menuKeyName() == "Ctrl+Shift+O");
    CHECK(KeyChoices::resetMenuKey().done());
}
