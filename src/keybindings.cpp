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
    // Windows takes hotkey ids from 1 to 0xBFFF for a program
    static const int LAST_HOTKEY_ID = 0xBFFF;

    // the next id after the last one given that no keybind has, going round to 1 after
    // the last, so one bound and deleted over and over never runs out; 0 for none.
    // Not the one just freed: a press of the old key can still be waiting in the queue.
    static int lastHotkeyId = 0;
    static int freeHotkeyId() {
        std::vector<bool> used(LAST_HOTKEY_ID + 1, false);
        for (const auto& keybind : keybinds) {
            if (keybind->GetId() > 0 && keybind->GetId() <= LAST_HOTKEY_ID) used[keybind->GetId()] = true;
        }
        for (int step = 1; step <= LAST_HOTKEY_ID; step++) {
            int id = (lastHotkeyId + step - 1) % LAST_HOTKEY_ID + 1;
            if (!used[id]) return lastHotkeyId = id;
        }
        return 0;
    }
    static bool gameInFront = false;
    static bool enabled = true;

    // Lua and the rest of the code use GLFW key codes, RegisterHotKey wants Windows ones
    UINT GlfwKeyToVirtualKey(int key) {
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

    static UINT windowsModifiers(KeybindModifiers modifiers) {
        UINT out = MOD_NOREPEAT;
        if (modifiers & KeybindModifiers_Alt) out |= MOD_ALT;
        if (modifiers & KeybindModifiers_Ctrl) out |= MOD_CONTROL;
        if (modifiers & KeybindModifiers_Shift) out |= MOD_SHIFT;
        return out;
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
                UINT modifiers = windowsModifiers(keybind->GetModifiers());
                if (virtualKey == 0 || !RegisterHotKey(hotkeyWindow, keybind->GetId(), modifiers, virtualKey)) {
                    std::cerr << "Could not register key " << KeyName(keybind->GetKey(), keybind->GetModifiers()) << std::endl;
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

    std::string WhyNotBindable(int glfwKey, KeybindFlags flags, KeybindModifiers modifiers) {
        if (GlfwKeyToVirtualKey(glfwKey) == 0) {
            return "key " + std::to_string(glfwKey) + " can't be bound";
        }
        // any other bit would make Alt+F5 and Alt+F5 with it two keys to the check below, but one to Windows
        const KeybindModifiers allModifiers = KeybindModifiers_Alt | KeybindModifiers_Ctrl | KeybindModifiers_Shift;
        if ((modifiers & ~allModifiers) != 0) {
            return "modifiers " + std::to_string(modifiers) + " aren't only Alt, Ctrl and Shift";
        }
        if ((flags & ~KeybindFlags_ProcessWhileHidden) != 0) {
            return "flags " + std::to_string(flags) + " aren't only ProcessWhileHidden";
        }
        // a press runs one function, so a key with the same modifiers is bound once
        if (DoesKeybindExist(glfwKey, modifiers) != nullptr) {
            return KeyName(glfwKey, modifiers) + " is taken already";
        }
        return "";
    }

    std::string CreateKeybind(int glfwKey, std::function<void()> callbackFunction, KeybindFlags flags, KeybindModifiers modifiers,
        std::string* whyNot) {
        auto refuse = [&](const std::string& reason) {
            if (whyNot) *whyNot = reason;
            return std::string();
        };
        std::string cant = WhyNotBindable(glfwKey, flags, modifiers);
        if (!cant.empty()) return refuse(cant);
        int id = freeHotkeyId();
        if (id == 0) {
            return refuse("there are too many keybinds");
        }
        auto keybind = std::make_unique<Keybind>(id, glfwKey, modifiers, callbackFunction, flags);

        std::string uuid = keybind->GetUUID();

        keybinds.push_back(std::move(keybind));
        RefreshRegistrations();

        return uuid;
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

        std::vector<std::string> pressed;
        for (auto& keybind : keybinds) {
            if (keybind->ConsumePress()) {
                pressed.push_back(keybind->GetUUID());
            }
        }

        // callbacks can create or delete keybinds, so look each one up again
        for (const std::string& uuid : pressed) {
            auto it = std::find_if(keybinds.begin(), keybinds.end(),
                [&uuid](const std::unique_ptr<Keybind>& keybind) { return keybind->GetUUID() == uuid; });
            if (it == keybinds.end()) continue;

            auto callback = (*it)->GetCallback();
            if (callback) {
                callback();
            }
        }
    }

    bool DeleteKeybind(const std::string& uuid) {
        auto it = std::find_if(keybinds.begin(), keybinds.end(),
            [&uuid](const std::unique_ptr<Keybind>& keybind) {
                return keybind->GetUUID() == uuid;
            });

        if (it != keybinds.end()) {
            if ((*it)->IsRegistered()) {
                UnregisterHotKey(hotkeyWindow, (*it)->GetId());
            }

            keybinds.erase(it);

            RefreshRegistrations();
            return true;
        }
        return false;
    }

    Keybind* DoesKeybindExist(int glfwKey, KeybindModifiers modifiers) {
        for (auto& keybind : keybinds) {
            if (keybind->GetKey() == glfwKey && keybind->GetModifiers() == modifiers) {
                return keybind.get();
            }
        }
        return nullptr;
    }

    // the name of a key without modifiers, empty for one without a name
    static std::string plainName(int glfwKey) {
        if (glfwKey >= GLFW_KEY_F1 && glfwKey <= GLFW_KEY_F24) return "F" + std::to_string(glfwKey - GLFW_KEY_F1 + 1);
        if ((glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z) || (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9)) {
            return std::string(1, (char)glfwKey);
        }
        if (glfwKey >= GLFW_KEY_KP_0 && glfwKey <= GLFW_KEY_KP_9) return "Numpad " + std::to_string(glfwKey - GLFW_KEY_KP_0);
        switch (glfwKey) {
        case GLFW_KEY_GRAVE_ACCENT: return "`";
        case GLFW_KEY_MINUS: return "-";
        case GLFW_KEY_EQUAL: return "=";
        case GLFW_KEY_LEFT_BRACKET: return "[";
        case GLFW_KEY_RIGHT_BRACKET: return "]";
        case GLFW_KEY_BACKSLASH: return "\\";
        case GLFW_KEY_SEMICOLON: return ";";
        case GLFW_KEY_APOSTROPHE: return "'";
        case GLFW_KEY_COMMA: return ",";
        case GLFW_KEY_PERIOD: return ".";
        case GLFW_KEY_SLASH: return "/";
        case GLFW_KEY_SPACE: return "Space";
        case GLFW_KEY_ESCAPE: return "Esc";
        case GLFW_KEY_ENTER: return "Enter";
        case GLFW_KEY_TAB: return "Tab";
        case GLFW_KEY_BACKSPACE: return "Backspace";
        case GLFW_KEY_INSERT: return "Insert";
        case GLFW_KEY_DELETE: return "Delete";
        case GLFW_KEY_HOME: return "Home";
        case GLFW_KEY_END: return "End";
        case GLFW_KEY_PAGE_UP: return "Page Up";
        case GLFW_KEY_PAGE_DOWN: return "Page Down";
        case GLFW_KEY_LEFT: return "Left";
        case GLFW_KEY_RIGHT: return "Right";
        case GLFW_KEY_UP: return "Up";
        case GLFW_KEY_DOWN: return "Down";
        default: return "";
        }
    }

    std::string KeyName(int glfwKey, KeybindModifiers modifiers) {
        std::string name;
        if (modifiers & KeybindModifiers_Ctrl) name += "Ctrl+";
        if (modifiers & KeybindModifiers_Alt) name += "Alt+";
        if (modifiers & KeybindModifiers_Shift) name += "Shift+";
        std::string plain = plainName(glfwKey);
        if (plain.empty()) return name + "key " + std::to_string(glfwKey);
        return name + plain;
    }
}
