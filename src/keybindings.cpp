#include <windows.h>
#include "keybindings.hpp"
#include "overlay.hpp"
#include "capture.hpp"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <algorithm>
#include <iostream>

// Keys go through RegisterHotKey instead of glfwGetKey, since the overlay never
// has focus. Windows hands a registered key to us even while the game has focus,
// and that works without admin. The game doesn't get the key then, so keys are
// only held while they're needed (see RefreshRegistrations).

namespace Keybindings {
    std::vector<std::unique_ptr<Keybind>> keybinds;

    static HWND hotkeyWindow = nullptr;
    static WNDPROC originalWndProc = nullptr;
    static int nextHotkeyId = 1;
    static bool gameInFront = false;
    static bool enabled = true;

    // Lua and the rest of the code use GLFW key codes, RegisterHotKey wants Windows ones
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

    // GLFW ignores WM_HOTKEY, so we put our own window proc in front of it
    static LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_HOTKEY) {
            for (auto& keybind : keybinds) {
                if (keybind->GetId() == (int)wParam) {
                    keybind->Press();
                }
            }
            return 0;
        }
        return CallWindowProcW(originalWndProc, hwnd, msg, wParam, lParam);
    }

    void Init(GLFWwindow* window) {
        hotkeyWindow = glfwGetWin32Window(window);
        originalWndProc = (WNDPROC)SetWindowLongPtrW(hotkeyWindow, GWLP_WNDPROC, (LONG_PTR)HotkeyWndProc);
        RefreshRegistrations();
    }

    void Shutdown() {
        for (auto& keybind : keybinds) {
            if (keybind->IsRegistered()) {
                UnregisterHotKey(hotkeyWindow, keybind->GetId());
            }
        }
        keybinds.clear();

        if (hotkeyWindow && originalWndProc) {
            SetWindowLongPtrW(hotkeyWindow, GWLP_WNDPROC, (LONG_PTR)originalWndProc);
        }
        hotkeyWindow = nullptr;
        originalWndProc = nullptr;
    }

    // A registered hotkey is swallowed everywhere, not just in the game, so keys
    // are only held while the game is in front. Otherwise typing ` in Discord
    // wouldn't work while the overlay runs. On top of that the game doesn't get
    // the key either, so keys are only held while the menu is open, unless
    // they're flagged ProcessWhileHidden.
    void RefreshRegistrations() {
        if (!hotkeyWindow) return;

        for (auto& keybind : keybinds) {
            bool wanted = enabled && gameInFront && (Overlay::menuOpen || keybind->HasFlag(KeybindFlags_ProcessWhileHidden));
            if (wanted == keybind->IsRegistered()) continue;

            if (wanted) {
                UINT virtualKey = GlfwKeyToVirtualKey(keybind->GetKey());
                UINT modifiers = MOD_NOREPEAT;
                if (keybind->GetModifiers() & KeybindModifiers_Alt) modifiers |= MOD_ALT;
                if (keybind->GetModifiers() & KeybindModifiers_Ctrl) modifiers |= MOD_CONTROL;
                if (keybind->GetModifiers() & KeybindModifiers_Shift) modifiers |= MOD_SHIFT;
                if (virtualKey == 0 || !RegisterHotKey(hotkeyWindow, keybind->GetId(), modifiers, virtualKey)) {
                    std::cerr << "Could not register key " << keybind->GetKey() << std::endl;
                    continue;
                }
                keybind->SetRegistered(true);
            }
            else {
                UnregisterHotKey(hotkeyWindow, keybind->GetId());
                keybind->SetRegistered(false);
            }
        }
    }

    void SetEnabled(bool on) {
        enabled = on;
        RefreshRegistrations();
    }

    void CreateKeybind(int glfwKey, std::function<void()> callbackFunction, KeybindFlags flags, KeybindModifiers modifiers) {
        if (DoesKeybindExist(glfwKey) != nullptr) {
            return;
        }
        keybinds.push_back(std::make_unique<Keybind>(nextHotkeyId++, glfwKey, modifiers, callbackFunction, flags));
        RefreshRegistrations();
    }

    // runs the callbacks for keys pressed since the last frame
    void ProcessKeybindings() {
        // take or give back the keys when switching between the game and anything else
        HWND game = Capture::findGameWindow();
        bool inFront = game && GetForegroundWindow() == game;
        if (inFront != gameInFront) {
            gameInFront = inFront;
            RefreshRegistrations();
        }

        std::vector<int> pressed;
        for (auto& keybind : keybinds) {
            if (keybind->ConsumePress()) {
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
        Keybind* keybind = DoesKeybindExist(glfwKey);
        if (keybind && keybind->IsRegistered()) {
            UnregisterHotKey(hotkeyWindow, keybind->GetId());
        }

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
