#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

// Gets the data the map matching needs into a folder: manifest.tsv plus either
// a ready index.bin or the map images to build one from.
//
// The manifest is planned from HoYoLAB's map API every time, so it always
// matches the current game map. A prebuilt index is published on this repo's
// "map-index" release (see .github/workflows/map-index.yml). If it was built
// from the same manifest we download that (~26 MB). If not, for example right
// after a game update, we download the map images (~120 MB) and the index is
// built locally the first time it's loaded.

namespace MapData {
    struct File {
        std::string url;
        std::string path;  // relative to the data folder
    };

    struct Plan {
        std::string manifest;
        std::vector<File> files;
        std::set<std::string> tileDirs;
    };

    // stage is a short description, percent is 0 to 100
    typedef std::function<void(const char* stage, int percent)> Progress;

    extern bool makePlan(Plan& plan);
    extern bool downloadMissing(const std::filesystem::path& folder, const std::vector<File>& files, const Progress& progress,
        const std::atomic<bool>* cancel = nullptr);
    extern bool downloadPrebuiltIndex(const std::filesystem::path& folder, const Plan& plan, const Progress& progress);
    extern bool downloadItems();
    extern bool downloadItemIcons();
    extern bool downloadItemPositionData();

    // What the app runs on startup. Returns false only when there's nothing usable,
    // with no internet it keeps using whatever is already in the folder.
    extern bool ensure(const std::filesystem::path& folder, const Progress& progress, const std::atomic<bool>* cancel = nullptr);

    // --make-index: downloads everything, builds the index and writes the two
    // release files to folder/out. Returns 3 when the published index is already current.
    extern int makeIndex(const std::filesystem::path& folder);
}
