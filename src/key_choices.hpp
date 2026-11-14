#pragma once
#include <functional>
#include <optional>
#include <string>
#include "extensions.hpp"

// The keys the player picks for extensions. Every key RegisterKeybind made
// keeps the author's key as its default. The player's own is saved in the
// extension's grants by the author's key name, "" for none.
namespace KeyChoices {
    // one of an extension's keys by the id RegisterKeybind gave, or the menu's key when ext is null
    struct Holder {
        Extensions::Extension* ext = nullptr;
        std::string id;
        bool menu() const { return ext == nullptr; }
        bool operator==(const Holder& other) const { return ext == other.ext && id == other.id; }
    };
    // who has the key, none when it's free. A key waiting to be allowed is its extension's too.
    extern std::optional<Holder> holderOf(int glfwKey, int modifiers);
    // the key RegisterKeybind made, nullptr when it's gone or the menu's
    extern Extensions::Extension::HeldKey* find(const Holder& holder);
    // the holder as the player reads it, like "Minimap (Open map)" or "the menu"
    extern std::string describe(const Holder& holder);

    struct Result {
        // the key can't be used at all
        std::string whyNot;
        // someone else has it, nothing changed
        std::optional<Holder> takenBy;
        bool done() const { return whyNot.empty() && !takenBy; }
    };
    // the player's key for one of ext's keys, it counts as allowed and turns Keybinds on
    extern Result pick(Extensions::Extension& ext, const std::string& id, int glfwKey, int modifiers);
    // the same, but the key is taken from whoever has it, who is left without
    // one. Not from the menu, it always has a key.
    extern Result give(Extensions::Extension& ext, const std::string& id, int glfwKey, int modifiers);
    // back to the author's key
    extern Result reset(Extensions::Extension& ext, const std::string& id);
    // no key at all
    extern void unset(Extensions::Extension& ext, const std::string& id);

    // The menu's key, from Settings::menuKey, Alt+` when that can't be used.
    // toggle runs on a press.
    extern void startMenuKey(std::function<void()> toggle);
    extern int menuKey();
    extern int menuModifiers();
    extern std::string menuKeyName();
    // the menu may have Alt+`, the others not. takeIt takes it from an extension.
    extern Result pickMenuKey(int glfwKey, int modifiers, bool takeIt = false);
    extern Result resetMenuKey();

    // the extension's Keys tab, one row per key
    extern void drawKeysTab(Extensions::Extension& ext);
    // the menu key's row, for the Home page
    extern void drawMenuKeyRow();
    // Record waits for keys, or took them this frame. It needs the focus like a text box.
    extern bool recording();
    extern void stopRecording();

    // RegisterKeybind's: the key a new one starts with, the player's saved one
    // or the author's. None when that's someone else's, the one who has it keeps it.
    extern void start(Extensions::Extension& ext, Extensions::Extension::HeldKey& key);
}
