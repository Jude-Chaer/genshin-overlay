#pragma once
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <functional>
#include <vector>
#include <memory>
#include "helpers.hpp"

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
            : m_id(id), m_glfwKey(glfwKey), m_modifiers(modifiers), m_callback(callback), m_flags(flags), m_uuid(Helpers::GenerateUUID()) {
        }

        int GetId() const { return m_id; }
        int GetKey() const { return m_glfwKey; }
        std::string GetUUID() const { return m_uuid; }
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
        std::string m_uuid;
        KeybindModifiers m_modifiers;
        std::function<void()> m_callback;
        KeybindFlags m_flags;
        bool m_registered = false;
        bool m_pressed = false;
    };

    extern std::vector<std::unique_ptr<Keybind>> keybinds;
    extern void Init(GLFWwindow* window);
    extern void Shutdown();
    // The keybind's id for DeleteKeybind. Empty when it can't be bound, and whyNot
    // (when given) says why: the key with these modifiers is taken, or it's a key
    // Windows can't hold. F5 and Ctrl+F5 are two keybinds.
    // why the key can't be bound, empty when it can
    extern std::string WhyNotBindable(int glfwKey, KeybindFlags flags, KeybindModifiers modifiers);
    extern std::string CreateKeybind(int glfwKey, std::function<void()> callbackFunction, KeybindFlags flags,
        KeybindModifiers modifiers = KeybindModifiers_None, std::string* whyNot = nullptr);
    extern void ProcessKeybindings();
    extern void RefreshRegistrations();
    // lets go of every key, or takes them back
    extern void SetEnabled(bool enabled);
    // the Windows key for a GLFW one, 0 for a key that can't be bound
    extern unsigned int GlfwKeyToVirtualKey(int key);
    // the pickable GLFW key for a Windows one, 0 for none
    extern int PickableKeyFor(unsigned int virtualKey);
    // Another program holds the key as a Windows hotkey, so the overlay can't
    // have it until that one lets go. Programs that read keys some other way
    // aren't seen.
    extern bool OtherProgramHas(int glfwKey, KeybindModifiers modifiers);
    extern Keybind* DoesKeybindExist(int glfwKey, KeybindModifiers modifiers = KeybindModifiers_None);
    // the key as a player reads it, like Ctrl+F5
    extern std::string KeyName(int glfwKey, KeybindModifiers modifiers);
    // ImGui's key for a GLFW one, ImGuiKey_None for a key PickableKeys doesn't have
    extern ImGuiKey ImGuiKeyFor(int glfwKey);
    // the key with its modifiers, for ImGui::IsKeyChordPressed
    extern ImGuiKeyChord ImGuiChordFor(int glfwKey, KeybindModifiers modifiers);
    // KeyName backwards, false for a name it doesn't make
    extern bool ParseKeyName(const std::string& name, int& glfwKey, KeybindModifiers& modifiers);
    // the keys a player can pick from, in the order the list shows them
    extern const std::vector<int>& PickableKeys();
    // keys Windows or the game window need, no extension or pick may take them.
    // Alt+` is the menu's own.
    extern bool IsProtected(int glfwKey, KeybindModifiers modifiers);
    // Why an extension's or the player's key can't be used, empty when it can:
    // it needs Alt or Ctrl, isn't protected and Windows can hold it. menuKey for
    // the menu's own key, which may be Alt+`. Doesn't look at who holds it.
    extern std::string WhyNotPickable(int glfwKey, KeybindModifiers modifiers, bool menuKey = false);
    extern bool DeleteKeybind(const std::string& uuid);

}
