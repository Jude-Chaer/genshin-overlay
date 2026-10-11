#include <windows.h>
#include <GLFW/glfw3.h>
#include <catch2/catch_test_macros.hpp>
#include "keybindings.hpp"

using Keybindings::GlfwKeyToVirtualKey;

// There's no overlay window in the tests, so no key is really taken from
// Windows. A press is faked the way the window proc does it.
static void press(int glfwKey) {
    Keybindings::Keybind* keybind = Keybindings::DoesKeybindExist(glfwKey);
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

TEST_CASE("a key that's taken isn't bound a second time") {
    Keybindings::keybinds.clear();
    int first = 0, second = 0;
    Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { first++; }, Keybindings::KeybindFlags_None);
    Keybindings::CreateKeybind(GLFW_KEY_F5, [&]() { second++; }, Keybindings::KeybindFlags_ProcessWhileHidden,
        Keybindings::KeybindModifiers_Ctrl);
    CHECK(Keybindings::keybinds.size() == 1);

    press(GLFW_KEY_F5);
    Keybindings::ProcessKeybindings();
    CHECK(first == 1);
    CHECK(second == 0);

    Keybindings::DeleteKeybind(GLFW_KEY_F5);
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
    Keybindings::CreateKeybind(GLFW_KEY_F5, []() {
        Keybindings::DeleteKeybind(GLFW_KEY_F5);
        Keybindings::DeleteKeybind(GLFW_KEY_F6);
        }, Keybindings::KeybindFlags_None);
    Keybindings::CreateKeybind(GLFW_KEY_F6, [&]() { other++; }, Keybindings::KeybindFlags_None);

    // both pressed in the same frame, F5 runs first and takes F6 away
    press(GLFW_KEY_F5);
    press(GLFW_KEY_F6);
    Keybindings::ProcessKeybindings();
    CHECK(other == 0);
    CHECK(Keybindings::keybinds.empty());
}
