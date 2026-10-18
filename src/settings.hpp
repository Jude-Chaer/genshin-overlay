#pragma once
#include <nlohmann/json.hpp>
#include <unordered_set>
#include "extensions.hpp"
// The app's own settings, saved in settings.json next to the exe.

namespace Settings {
    // Start as admin. Only needed to see M and Esc while the game has focus,
    // so the map overlay can close the moment the map does.
	extern bool hideOnMapClose;

    // Switches for parts of the map following, in the menu under Advanced.
    // the map's tiles keep the follower lined up while the map moves
    extern bool lineUpWithMap;
    // patches that only see open water don't count when following
    extern bool ignoreSea;
    // the map is drawn where it will be when the picture shows up, not where it was seen
    extern bool guessAhead;
    // nothing is guessed ahead while the map zooms
    extern bool steadyZoom;
    // SIFT looks again when the map stands still but looks different, as after picking a floor
    extern bool lookAgainStill;
    // following runs on its own thread instead of in the draw loop
    extern bool followOnThread;
    // the patches are shrunk on the GPU before they're read back
    extern bool shrinkOnGpu;
    // SIFT tries the game 480 wide first, and 960 only if that isn't a strong match
    extern bool findSmallFirst;

    // listen to the game's traffic (needs Npcap), off until the player turns it on
    extern bool readGameTraffic;

    // The file the settings are in. Empty means settings.json next to the exe,
    // the tests point it somewhere else.
    extern std::filesystem::path file;

    extern void Load();
    extern void Save();

    // extension settings live in the same file, under "extensions"
    extern nlohmann::json LoadExtensions();
    extern void SaveExtensions(const nlohmann::json& extensions);

    extern void SaveExtensionPermissions(std::string extensionHash, std::unordered_set<Extensions::ExtensionPermissionTypes> permissions);
    extern std::unordered_set<Extensions::ExtensionPermissionTypes> LoadExtensionPermissions(std::string extensionHash);
}
