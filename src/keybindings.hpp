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
        Keybind(int id, int glfwKey, std::function<void()> callback, KeybindFlags flags = KeybindFlags_None)
            : m_id(id), m_glfwKey(glfwKey), m_callback(callback), m_flags(flags) {
        }

        int GetId() const { return m_id; }
        int GetKey() const { return m_glfwKey; }
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
        std::function<void()> m_callback;
        KeybindFlags m_flags;
        bool m_registered = false;
        bool m_pressed = false;
    };

    extern std::vector<std::unique_ptr<Keybind>> keybinds;
    extern void Init(GLFWwindow* window);
    extern void Shutdown();
    extern void CreateKeybind(int glfwKey, std::function<void()> callbackFunction, KeybindFlags flags);
    extern void ProcessKeybindings();
    extern void RefreshRegistrations();
    extern Keybind* DoesKeybindExist(int glfwKey);
    extern void DeleteKeybind(int glfwKey);

}
