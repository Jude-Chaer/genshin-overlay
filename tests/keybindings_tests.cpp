#include <windows.h>
#include <GLFW/glfw3.h>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <thread>
#include "keybindings.hpp"

using Keybindings::GlfwKeyToVirtualKey;

// There's no overlay window in the tests, so no key is really taken from
// Windows. A press is faked the way the window proc does it.
static void press(int glfwKey, Keybindings::KeybindModifiers modifiers = Keybindings::KeybindModifiers_None) {
    Keybindings::Keybind* keybind = Keybindings::DoesKeybindExist(glfwKey, modifiers);
    REQUIRE(keybind != nullptr);
    keybind->Press();
}

TEST_CASE("GLFW keys turn into the Windows ones") {
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_A) == 'A');
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_Z) == 'Z');
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_0) == '0');
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_9) == '9');
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_F1) == VK_F1);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_F12) == VK_F12);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_F24) == VK_F24);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_KP_0) == VK_NUMPAD0);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_KP_9) == VK_NUMPAD9);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_GRAVE_ACCENT) == VK_OEM_3);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_SPACE) == VK_SPACE);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_ESCAPE) == VK_ESCAPE);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_ENTER) == VK_RETURN);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_PAGE_UP) == VK_PRIOR);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_PAGE_DOWN) == VK_NEXT);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_LEFT) == VK_LEFT);
    // keys that can't be bound
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_LEFT_SHIFT) == 0);
    CHECK(GlfwKeyToVirtualKey(GLFW_KEY_UNKNOWN) == 0);
}

TEST_CASE("every pickable key comes back from its Windows key") {
    for (int key : Keybindings::PickableKeys()) {
        INFO(key);
        CHECK(Keybindings::PickableKeyFor(Keybindings::GlfwKeyToVirtualKey(key)) == key);
    }
    // modifiers and nothing aren't picked
    CHECK(Keybindings::PickableKeyFor(VK_MENU) == 0);
    CHECK(Keybindings::PickableKeyFor(0) == 0);
}

TEST_CASE("a pressed key runs its function once") {
    Keybindings::keybinds.clear();
    int runs = 0;
    Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { runs++; }, Keybindings::KeybindFlags_None);

    Keybindings::ProcessKeybindings();
    CHECK(runs == 0);

    press(GLFW_KEY_F5);
    Keybindings::ProcessKeybindings();
    CHECK(runs == 1);
    Keybindings::ProcessKeybindings();
    CHECK(runs == 1);
    Keybindings::keybinds.clear();
}

TEST_CASE("F5 and Ctrl+F5 are two keys, each bound once") {
    Keybindings::keybinds.clear();
    int plain = 0, ctrl = 0;
    std::string f5 = Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { plain++; }, Keybindings::KeybindFlags_None);
    std::string ctrlF5 = Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { ctrl++; }, Keybindings::KeybindFlags_None,
        Keybindings::KeybindModifiers_Ctrl);
    CHECK_FALSE(f5.empty());
    CHECK_FALSE(ctrlF5.empty());

    // each press runs its own function
    press(GLFW_KEY_F5, Keybindings::KeybindModifiers_Ctrl);
    Keybindings::ProcessKeybindings();
    CHECK(plain == 0);
    CHECK(ctrl == 1);
    press(GLFW_KEY_F5);
    Keybindings::ProcessKeybindings();
    CHECK(plain == 1);
    CHECK(ctrl == 1);

    // the same key with the same modifiers again is refused, with the reason
    std::string whyNot;
    CHECK(Keybindings::CreateKeybind(GLFW_KEY_F5, []() {}, Keybindings::KeybindFlags_None,
        Keybindings::KeybindModifiers_Ctrl, &whyNot).empty());
    CHECK(whyNot == "Ctrl+F5 is taken already");
    CHECK(Keybindings::CreateKeybind(GLFW_KEY_LEFT_SHIFT, []() {}, Keybindings::KeybindFlags_None,
        Keybindings::KeybindModifiers_None, &whyNot).empty());
    CHECK(whyNot == "key 340 can't be bound");
    CHECK(Keybindings::keybinds.size() == 2);
    Keybindings::keybinds.clear();
}

TEST_CASE("a key that's taken isn't bound a second time") {
    Keybindings::keybinds.clear();
    int first = 0, second = 0;
    std::string id = Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { first++; }, Keybindings::KeybindFlags_None);
    CHECK_FALSE(id.empty());
    std::string taken = Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { second++; }, Keybindings::KeybindFlags_ProcessWhileHidden);
    CHECK(taken.empty());
    CHECK(Keybindings::keybinds.size() == 1);

    press(GLFW_KEY_F5);
    Keybindings::ProcessKeybindings();
    CHECK(first == 1);
    CHECK(second == 0);

    CHECK_FALSE(Keybindings::DeleteKeybind(taken));
    CHECK(Keybindings::DeleteKeybind(id));
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F5) == nullptr);
    // free again now
    Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { second++; }, Keybindings::KeybindFlags_None);
    press(GLFW_KEY_F5);
    Keybindings::ProcessKeybindings();
    CHECK(second == 1);
    Keybindings::keybinds.clear();
}

TEST_CASE("a function can delete keybinds while they're being run") {
    Keybindings::keybinds.clear();
    int other = 0;
    std::string f5, f6;
    f5 = Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() {
        Keybindings::DeleteKeybind(f5);
        Keybindings::DeleteKeybind(f6);
        }, Keybindings::KeybindFlags_None);
    f6 = Keybindings::CreateKeybind(GLFW_KEY_F6, [&]() { other++; }, Keybindings::KeybindFlags_None);

    // both pressed in the same frame, F5 runs first and takes F6 away
    press(GLFW_KEY_F5);
    press(GLFW_KEY_F6);
    Keybindings::ProcessKeybindings();
    CHECK(other == 0);
    CHECK(Keybindings::keybinds.empty());
}

TEST_CASE("a keybind with bits that aren't modifiers or flags isn't bound") {
    Keybindings::keybinds.clear();
    std::string whyNot;
    // Alt with an unknown bit is refused, not bound as a second Alt+F5
    REQUIRE_FALSE(Keybindings::CreateKeybind(GLFW_KEY_F5, [] {}, Keybindings::KeybindFlags_None, Keybindings::KeybindModifiers_Alt).empty());
    CHECK(Keybindings::CreateKeybind(GLFW_KEY_F5, [] {}, Keybindings::KeybindFlags_None, Keybindings::KeybindModifiers_Alt | 8, &whyNot).empty());
    CHECK(whyNot == "modifiers 9 aren't only Alt, Ctrl and Shift");
    CHECK(Keybindings::CreateKeybind(GLFW_KEY_F6, [] {}, 2, Keybindings::KeybindModifiers_None, &whyNot).empty());
    CHECK(whyNot == "flags 2 aren't only ProcessWhileHidden");
    CHECK(Keybindings::keybinds.size() == 1);
    Keybindings::keybinds.clear();
}

TEST_CASE("a key bound and deleted again and again never runs out of ids") {
    Keybindings::keybinds.clear();
    // Windows only has ids up to 0xBFFF, they go round to 1
    std::string first = Keybindings::CreateKeybind(GLFW_KEY_F5, [] {}, Keybindings::KeybindFlags_None);
    std::string second = Keybindings::CreateKeybind(GLFW_KEY_F6, [] {}, Keybindings::KeybindFlags_None);
    int secondId = Keybindings::DoesKeybindExist(GLFW_KEY_F6)->GetId();
    int bound = 0;
    int sameAsFreed = 0;
    int onSecond = 0;
    for (int i = 0; i < 0xC000 * 2; i++) {
        int freed = Keybindings::DoesKeybindExist(GLFW_KEY_F5)->GetId();
        Keybindings::DeleteKeybind(first);
        first = Keybindings::CreateKeybind(GLFW_KEY_F5, [] {}, Keybindings::KeybindFlags_None);
        if (first.empty()) continue;
        bound++;
        int id = Keybindings::DoesKeybindExist(GLFW_KEY_F5)->GetId();
        // a press of the deleted key still in the queue mustn't reach the new one
        if (id == freed) sameAsFreed++;
        if (id == secondId) onSecond++;
    }
    CHECK(bound == 0xC000 * 2);
    CHECK(sameAsFreed == 0);
    CHECK(onSecond == 0);
    CHECK(Keybindings::DoesKeybindExist(GLFW_KEY_F6)->GetId() == secondId);
    Keybindings::keybinds.clear();
}

TEST_CASE("every key the player can pick has a name that reads back") {
    using namespace Keybindings;
    const KeybindModifiers all[] = { KeybindModifiers_Alt, KeybindModifiers_Ctrl, KeybindModifiers_Alt | KeybindModifiers_Shift,
        KeybindModifiers_Ctrl | KeybindModifiers_Shift, KeybindModifiers_Ctrl | KeybindModifiers_Alt };
    for (int key : PickableKeys()) {
        CHECK(GlfwKeyToVirtualKey(key) != 0);
        for (KeybindModifiers modifiers : all) {
            std::string name = KeyName(key, modifiers);
            INFO(name);
            CHECK(name.find("key ") == std::string::npos);
            int back = 0;
            KeybindModifiers backModifiers = 0;
            REQUIRE(ParseKeyName(name, back, backModifiers));
            CHECK(back == key);
            CHECK(backModifiers == modifiers);
        }
    }
    int key = 0;
    KeybindModifiers modifiers = 0;
    CHECK(KeyName(GLFW_KEY_EQUAL, KeybindModifiers_Alt) == "Alt+=");
    CHECK(KeyName(GLFW_KEY_KP_3, KeybindModifiers_Ctrl) == "Ctrl+Numpad 3");
    CHECK_FALSE(ParseKeyName("", key, modifiers));
    CHECK_FALSE(ParseKeyName("Alt+", key, modifiers));
    CHECK_FALSE(ParseKeyName("Alt+Nothing", key, modifiers));
    // KeyName puts Ctrl first, another order isn't one of its names
    CHECK_FALSE(ParseKeyName("Alt+Ctrl+M", key, modifiers));
}

TEST_CASE("a picked key needs Alt or Ctrl and isn't one Windows or the menu keeps") {
    using namespace Keybindings;
    CHECK(WhyNotPickable(GLFW_KEY_M, KeybindModifiers_Alt).empty());
    CHECK(WhyNotPickable(GLFW_KEY_M, KeybindModifiers_Ctrl | KeybindModifiers_Shift).empty());
    CHECK(WhyNotPickable(GLFW_KEY_M, KeybindModifiers_None) ==
        "M needs Alt or Ctrl, without them the key doesn't reach the overlay while the game is in front");
    CHECK(WhyNotPickable(GLFW_KEY_M, KeybindModifiers_Shift) ==
        "Shift+M needs Alt or Ctrl, without them the key doesn't reach the overlay while the game is in front");
    const std::pair<int, KeybindModifiers> windows[] = {
        { GLFW_KEY_TAB, KeybindModifiers_Alt }, { GLFW_KEY_F4, KeybindModifiers_Alt }, { GLFW_KEY_ESCAPE, KeybindModifiers_Alt },
        { GLFW_KEY_SPACE, KeybindModifiers_Alt }, { GLFW_KEY_ENTER, KeybindModifiers_Alt }, { GLFW_KEY_ESCAPE, KeybindModifiers_Ctrl },
        { GLFW_KEY_ESCAPE, KeybindModifiers_Ctrl | KeybindModifiers_Shift },
        { GLFW_KEY_TAB, KeybindModifiers_Alt | KeybindModifiers_Shift }, { GLFW_KEY_TAB, KeybindModifiers_Ctrl | KeybindModifiers_Alt },
        { GLFW_KEY_ESCAPE, KeybindModifiers_Alt | KeybindModifiers_Shift },
    };
    for (const auto& [key, modifiers] : windows) {
        CHECK(WhyNotPickable(key, modifiers) == KeyName(key, modifiers) + " is kept for Windows");
        CHECK(WhyNotPickable(key, modifiers, true) == KeyName(key, modifiers) + " is kept for Windows");
    }
    // Ctrl with Space or F4 is fine, Alt+Shift+` too
    CHECK(WhyNotPickable(GLFW_KEY_SPACE, KeybindModifiers_Ctrl).empty());
    CHECK(WhyNotPickable(GLFW_KEY_F4, KeybindModifiers_Ctrl).empty());
    CHECK(WhyNotPickable(GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Alt) == "Alt+` is kept for the overlay's menu");
    CHECK(WhyNotPickable(GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Alt, true).empty());
    CHECK(WhyNotPickable(GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Ctrl).empty());
    CHECK(WhyNotPickable(GLFW_KEY_GRAVE_ACCENT, KeybindModifiers_Alt | KeybindModifiers_Shift).empty());
    // a key with no row in the lists, ImGui can't read it for the menu key
    CHECK(WhyNotPickable(GLFW_KEY_F13, KeybindModifiers_Ctrl) == "Ctrl+F13 can't be bound");
    CHECK(WhyNotPickable(GLFW_KEY_LEFT_SHIFT, KeybindModifiers_Alt) == "Alt+key 340 can't be bound");
}

TEST_CASE("a key another program holds as a hotkey is seen, a free one isn't") {
    using namespace Keybindings;
    const KeybindModifiers held = KeybindModifiers_Ctrl | KeybindModifiers_Alt | KeybindModifiers_Shift;
    const UINT windowsHeld = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    if (OtherProgramHas(GLFW_KEY_F24, held)) SKIP("something on this PC has Ctrl+Alt+Shift+F24");
    // a free key stays free after the look
    CHECK_FALSE(OtherProgramHas(GLFW_KEY_F24, held));

    // the other program: a thread of its own holding the key
    std::promise<bool> registered;
    std::promise<void> letGo;
    std::thread other([&]() {
        bool ok = RegisterHotKey(nullptr, 1, windowsHeld, VK_F24) != FALSE;
        registered.set_value(ok);
        letGo.get_future().wait();
        if (ok) UnregisterHotKey(nullptr, 1);
    });
    bool ok = registered.get_future().get();
    CHECK(ok);
    CHECK(OtherProgramHas(GLFW_KEY_F24, held));
    // the same key without Shift is another hotkey
    CHECK_FALSE(OtherProgramHas(GLFW_KEY_F24, KeybindModifiers_Ctrl | KeybindModifiers_Alt));
    letGo.set_value();
    other.join();
    CHECK_FALSE(OtherProgramHas(GLFW_KEY_F24, held));
}
