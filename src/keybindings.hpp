#pragma once
#include <GLFW/glfw3.h>
#include <functional>
#include <vector>
#include <memory>

namespace Keybindings {

    enum KeybindFlags_ {
        KeybindFlags_None = 0,
        KeybindFlags_ProcessWhileHidden = 1 << 0,
    };
    typedef int KeybindFlags;

    class Keybind {
    public:
        Keybind(int glfwKey, std::function<void()> callback, KeybindFlags flags = KeybindFlags_None)
            : m_glfwKey(glfwKey), m_callback(callback), m_flags(flags), m_lastState(false) {
        }

        int GetKey() const { return m_glfwKey; }
        KeybindFlags GetFlags() const { return m_flags; }
        std::function<void()> GetCallback() const { return m_callback; }

        bool HasFlag(KeybindFlags flag) const { return (m_flags & flag) != 0; }

        // true only on the frame the key goes down
        bool Update(bool isPressed) {
            bool pressedNow = isPressed && !m_lastState;
            m_lastState = isPressed;
            return pressedNow;
        }

    private:
        int m_glfwKey;
        std::function<void()> m_callback;
        KeybindFlags m_flags;
        bool m_lastState;
    };

    extern std::vector<std::unique_ptr<Keybind>> keybinds;
    extern void Shutdown();
    extern void CreateKeybind(int glfwKey, std::function<void()> callbackFunction, KeybindFlags flags);
    extern void ProcessKeybindings();
    extern Keybind* DoesKeybindExist(int glfwKey);
    extern void DeleteKeybind(int glfwKey);

}
