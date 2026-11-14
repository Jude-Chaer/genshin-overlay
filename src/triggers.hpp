#pragma once
#include <sol/sol.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include "controls.hpp"
#include "extensions.hpp"
#include "item_files.hpp"
#include "map_tracking.hpp"
#include "minimap.hpp"
#include "screens.hpp"

// AddTrigger(name, function, only): the function runs once each time its
// topic changes, with the table the topic's call gives (GetScreens,
// GetMapView, GetPlayer, GetItems...) and what changed. With only, it runs
// only when that is what changed. Nothing runs for the state at the start.

namespace Triggers {
    // How many triggers one extension may have
    inline const size_t MaxTriggers = 64;

    // the trigger names an extension can give, in the order the docs list them
    extern const std::vector<std::string>& names();
    // what `changed` can be for a trigger name, empty when it's an id or a name
    // the extension picks (a setting, a permission)
    extern std::vector<std::string> changes(const std::string& name);
    // the permission a trigger name needs to run, None for most
    extern Extensions::ExtensionPermissionTypes permissionFor(const std::string& name);

    // What the triggers are checked against, read once per turn of the draw loop
    struct State {
        Screens::Up screens;
        // the game frame of each screen's picture, 0 with none, only read
        // while a ScreenPicture trigger wants that screen
        std::array<uint64_t, (size_t)Screens::Screen::Count> pictures = {};
        // what GetMapView gives, with the floor picker's box from the overlay
        MapTracking::MapView view;
        RECT picker = {};
        // what GetPlayer gives, only read while a player trigger wants it
        MiniMap::Spot player;
        // what GetItems gives
        uint64_t itemsVersion = 0;
        ItemFiles::Status items;
        // the extension whose page the menu shows, by its place in registeredExtensions, -1 for none
        int pageShown = -1;
        // the player's look and whether it's dark
        Controls::Look look = Controls::Look::Windows;
        bool dark = true;
        // the look's accent, which follows Windows' in Windows 11 and Material You
        unsigned int accent = 0;
        // the menu's text size in pixels, which goes with the player's Size and Windows' scale
        float font = 0;
    };

    // The overlay as it is now. Reads the player only when wantsPlayer.
    extern State current(bool wantsPlayer);
    // whether a player trigger is set by an extension that may read the map
    extern bool wantsPlayer();
    // any trigger set at all, so the loop can skip reading the state
    extern bool any();
    // Runs the triggers whose topic changed since the last check, each once per change.
    // A trigger's first check only takes the state as it is, it doesn't run.
    // They run outside a frame, like keys: drawing and ImGUI_ do nothing there.
    extern void check(const State& now);
    // the draw loop's call: reads the state and checks, nothing when there are no triggers
    extern void checkNow();

    // AddTrigger(name, function, only) for the running extension, its id, 0 when refused (logged)
    extern int add(const std::string& name, sol::protected_function function, sol::optional<std::string> only);
    // RemoveTrigger(id), only the running extension's own
    extern void remove(int id);
    // all of the extension's triggers, see Extensions::unload
    extern void forget(Extensions::Extension* owner);
    // how many the extension has, for the tests
    extern size_t count(const Extensions::Extension* owner);
}
