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

    // modifier keys held together with the key, can be combined
    enum KeybindModifiers_ {
        KeybindModifiers_None = 0,
        KeybindModifiers_Alt = 1 << 0,
        KeybindModifiers_Ctrl = 1 << 1,
        KeybindModifiers_Shift = 1 << 2,
    };
    typedef int KeybindModifiers;

    class Keybind {
    public:
        Keybind(int id, int glfwKey, KeybindModifiers modifiers, std::function<void()> callback, KeybindFlags flags = KeybindFlags_None)
            : m_id(id), m_glfwKey(glfwKey), m_modifiers(modifiers), m_callback(callback), m_flags(flags) {
        }

        int GetId() const { return m_id; }
        int GetKey() const { return m_glfwKey; }
        KeybindModifiers GetModifiers() const { return m_modifiers; }
        KeybindFlags GetFlags() const { return m_flags; }
        std::function<void()> GetCallback() const { return m_callback; }

        bool HasFlag(KeybindFlags flag) const { return (m_flags & flag) != 0; }

        bool IsRegistered() const { return m_registered; }
        void SetRegistered(bool registered) { m_registered = registered; }

        void Press() { m_pressed = true; }
        bool ConsumePress() {
            bool pressed = m_pressed;
            m_pressed = false;
            return pressed;
        }

    private:
        int m_id;
        int m_glfwKey;
        KeybindModifiers m_modifiers;
        std::function<void()> m_callback;
        KeybindFlags m_flags;
        bool m_registered = false;
        bool m_pressed = false;
    };

    extern std::vector<std::unique_ptr<Keybind>> keybinds;
    extern void Init(GLFWwindow* window);
    extern void Shutdown();
    extern void CreateKeybind(int glfwKey, std::function<void()> callbackFunction, KeybindFlags flags, KeybindModifiers modifiers = KeybindModifiers_None);
    extern void ProcessKeybindings();
    extern void RefreshRegistrations();
    // lets go of every key, or takes them back
    extern void SetEnabled(bool enabled);
    extern Keybind* DoesKeybindExist(int glfwKey);
    extern void DeleteKeybind(int glfwKey);

}
