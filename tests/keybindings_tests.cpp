#include <windows.h>
#include <GLFW/glfw3.h>
#include <catch2/catch_test_macros.hpp>
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
