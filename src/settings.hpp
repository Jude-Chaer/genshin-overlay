#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>
// The app's own settings, saved in settings.json next to the exe. A
// settings.json that can't be read is kept as settings.json.broken, and
// everything starts from the defaults.

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

    // the map items are downloaded from the release when there's a newer pack (the menu's "Download map items")
    extern bool downloadItems;

    // the menu's look by its shown name (Controls::LookName), and Follow Windows, Dark or Light
    extern std::string look;
    extern std::string lightDark;
    // the overlay's size in percent of what Windows' scale gives, one of Sizes:
    // the menu and everything else drawn with its font (map labels, extensions)
    extern int size;
    extern const std::vector<int> Sizes;

    // The file the settings are in. Empty means settings.json next to the exe,
    // the tests point it somewhere else.
    extern std::filesystem::path file;

    extern void Load();
    extern void Save();

    // extension settings live in the same file, under "extensions"
    extern nlohmann::json LoadExtensions();
    extern void SaveExtensions(const nlohmann::json& extensions);

    // What the player allowed an extension, kept per extension folder: the
    // permissions by name and the hash its files had then.
    struct Grants {
        bool saved = false;
        std::string hash;
        std::vector<std::string> names;
    };
    // Lists saved the old way (permission numbers under the files' hash) are
    // moved to the folder the first time that hash loads.
    extern Grants LoadGrants(const std::string& folder, const std::string& hash);
    extern void SaveGrants(const std::string& folder, const Grants& grants);
}
