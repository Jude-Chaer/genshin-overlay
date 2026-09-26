#include <windows.h>
#include "keybindings.hpp"
#include "overlay.hpp"
#include "capture.hpp"
#include <algorithm>

// Keys are read with GetAsyncKeyState every frame instead of glfwGetKey, since
// the overlay never has focus. The game still gets the key too.
// This only works while the game has focus because we run as admin: the game
// does, and Windows hides an admin window's keys from normal programs.
// RegisterHotKey didn't work in game either.

namespace Keybindings {
    std::vector<std::unique_ptr<Keybind>> keybinds;

    // Lua and the rest of the code use GLFW key codes, GetAsyncKeyState wants Windows ones
    static UINT GlfwKeyToVirtualKey(int key) {
        if ((key >= GLFW_KEY_0 && key <= GLFW_KEY_9) || (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)) {
            return (UINT)key;
        }
        if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) {
            return VK_F1 + (key - GLFW_KEY_F1);
        }
        if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) {
            return VK_NUMPAD0 + (key - GLFW_KEY_KP_0);
        }

        switch (key) {
        case GLFW_KEY_SPACE: return VK_SPACE;
        case GLFW_KEY_GRAVE_ACCENT: return VK_OEM_3;
        case GLFW_KEY_MINUS: return VK_OEM_MINUS;
        case GLFW_KEY_EQUAL: return VK_OEM_PLUS;
        case GLFW_KEY_LEFT_BRACKET: return VK_OEM_4;
        case GLFW_KEY_RIGHT_BRACKET: return VK_OEM_6;
        case GLFW_KEY_BACKSLASH: return VK_OEM_5;
        case GLFW_KEY_SEMICOLON: return VK_OEM_1;
        case GLFW_KEY_APOSTROPHE: return VK_OEM_7;
        case GLFW_KEY_COMMA: return VK_OEM_COMMA;
        case GLFW_KEY_PERIOD: return VK_OEM_PERIOD;
        case GLFW_KEY_SLASH: return VK_OEM_2;
        case GLFW_KEY_ESCAPE: return VK_ESCAPE;
        case GLFW_KEY_ENTER: return VK_RETURN;
        case GLFW_KEY_TAB: return VK_TAB;
        case GLFW_KEY_BACKSPACE: return VK_BACK;
        case GLFW_KEY_INSERT: return VK_INSERT;
        case GLFW_KEY_DELETE: return VK_DELETE;
        case GLFW_KEY_HOME: return VK_HOME;
        case GLFW_KEY_END: return VK_END;
        case GLFW_KEY_PAGE_UP: return VK_PRIOR;
        case GLFW_KEY_PAGE_DOWN: return VK_NEXT;
        case GLFW_KEY_LEFT: return VK_LEFT;
        case GLFW_KEY_RIGHT: return VK_RIGHT;
        case GLFW_KEY_UP: return VK_UP;
        case GLFW_KEY_DOWN: return VK_DOWN;
        default: return 0;
        }
    }

    void Shutdown() {
        keybinds.clear();
    }

    void CreateKeybind(int glfwKey, std::function<void()> callbackFunction, KeybindFlags flags) {
        if (DoesKeybindExist(glfwKey) != nullptr) {
            return;
        }
        keybinds.push_back(std::make_unique<Keybind>(glfwKey, callbackFunction, flags));
    }

    // runs the callbacks for keys that went down since the last frame
    void ProcessKeybindings() {
        // only react while the game is in front, so typing ` somewhere else does nothing.
        // With no game running they always work, which helps when testing.
        HWND game = Capture::findGameWindow();
        bool listening = !game || GetForegroundWindow() == game;

        std::vector<int> pressed;
        for (auto& keybind : keybinds) {
            UINT virtualKey = GlfwKeyToVirtualKey(keybind->GetKey());
            bool down = listening && virtualKey != 0 && (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
            if (!keybind->Update(down)) continue;
            if (Overlay::menuOpen || keybind->HasFlag(KeybindFlags_ProcessWhileHidden)) {
                pressed.push_back(keybind->GetKey());
            }
        }

        // callbacks can create or delete keybinds, so look each one up again
        for (int key : pressed) {
            Keybind* keybind = DoesKeybindExist(key);
            if (!keybind) continue;

            auto callback = keybind->GetCallback();
            if (callback) {
                callback();
            }
        }
    }

    void DeleteKeybind(int glfwKey) {
        keybinds.erase(std::remove_if(keybinds.begin(), keybinds.end(),
            [glfwKey](const std::unique_ptr<Keybind>& keybind) {
                return keybind->GetKey() == glfwKey;
            }), keybinds.end());
    }

    Keybind* DoesKeybindExist(int glfwKey) {
        for (auto& keybind : keybinds) {
            if (keybind->GetKey() == glfwKey) {
                return keybind.get();
            }
        }
        return nullptr;
    }
}
