#pragma once
#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <opencv2/core.hpp>

// Gets the data the map matching needs into a folder: manifest.tsv plus either
// a ready index.bin or the map images to build one from.
//
// The manifest is planned from HoYoLAB's map API every time, so it always
// matches the current game map. A prebuilt index is published on this repo's
// "map-index" release (see .github/workflows/map-index.yml). If it was built
// from the same manifest we download that (~26 MB). If not, for example right
// after a game update, we download the map images (~120 MB) and the index is
// built locally the first time it's loaded.
//
// The release also has every map shrunk small, for finding the minimap on a
// whole map. Those are downloaded too. One that isn't there, or is of an
// older map, the app makes from the tiles when it's needed.

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

    // one map's tiles on disk: cols x rows files of tileSize pixels, named
    // x_y.webp. Tile pixel = map units + origin.
    struct Grid {
        int cols = 0, rows = 0, tileSize = 256;
        double originX = 0.0, originY = 0.0;
        std::filesystem::path dir;
    };

    // An underground floor's picture, and the box of map units it covers on
    // its map.
    struct Floor {
        int mapId = 0, groupId = 0, floorId = 0;
        double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
        std::filesystem::path path;
        std::string url;
    };

    struct Manifest {
        std::map<int, Grid> grids;
        std::vector<Floor> floors;
    };

    // what the manifest in a data folder says there is
    extern Manifest readManifest(const std::filesystem::path& folder);
    extern std::filesystem::path tilePath(const Grid& grid, int x, int y);
    // Makes sure a file is on disk, downloaded if it isn't yet. False if it
    // couldn't be. What HoYoLAB doesn't have is kept as an empty file.
    extern bool fetch(const std::string& url, const std::filesystem::path& path);
    // Written under another name first, so a file that's there is whole.
    extern bool writeFile(const std::filesystem::path& path, const std::string& data);
    extern bool fetchTile(int mapId, const Grid& grid, int x, int y);

    // A picture as gray, shrunk. Black where it's see-through, like where
    // there's no tile. Empty if it isn't on disk.
    extern cv::Mat readGray(const std::filesystem::path& path, int shrunkBy = 1);

    // a whole map is kept shrunk by this
    constexpr int SMALL_BY = 8;
    extern std::filesystem::path wholeMapPath(const std::filesystem::path& folder, const Grid& grid);
    // Makes that picture from the map's tiles, downloading the ones that
    // aren't on disk. False, and nothing saved, if a tile couldn't be got.
    extern bool makeWholeMap(const std::filesystem::path& folder, int mapId, const Grid& grid, const std::atomic<bool>* cancel = nullptr);

    // Draws a part of a map from its tiles, the way the game should show it
    // there. Keeps the tiles it read, so every thread has its own.
    class Tiles {
    public:
        // gets a tile that isn't on disk, true if it's there now
        typedef std::function<bool(int mapId, const Grid& grid, int x, int y)> Fetch;
        // How many decoded tiles stay in memory. Without a fetch a tile that
        // isn't on disk is drawn black.
        explicit Tiles(size_t kept, Fetch fetch = nullptr) : m_kept(kept), m_fetch(std::move(fetch)) {}
        // side x side pixels of perPixel map units, the first one's corner at
        // left, top. From tiles shrunk by shrunkBy, black where there are none.
        cv::Mat Draw(int mapId, const Grid& grid, double left, double top, int side, double perPixel, int shrunkBy);
        void Clear();

    private:
        const cv::Mat& tile(int mapId, const Grid& grid, int x, int y, int shrunkBy);

        size_t m_kept;
        Fetch m_fetch;
        using Key = std::tuple<int, int, int, int>;  // map, x, y, shrunk by
        std::map<Key, cv::Mat> m_tiles;
        std::deque<Key> m_order;
    };

    // stage is a short description, percent is 0 to 100
    typedef std::function<void(const char* stage, int percent)> Progress;

    extern bool makePlan(Plan& plan);
    extern bool downloadMissing(const std::filesystem::path& folder, const std::vector<File>& files, const Progress& progress,
        const std::atomic<bool>* cancel = nullptr);
    extern bool downloadPrebuiltIndex(const std::filesystem::path& folder, const Plan& plan, const Progress& progress);
    extern bool downloadItems();
    extern bool downloadItemIcons(int map_id);
    extern bool downloadItemPositionData(int map_id);

    // What the app runs on startup. Returns false only when there's nothing usable,
    // with no internet it keeps using whatever is already in the folder.
    extern bool ensure(const std::filesystem::path& folder, const Progress& progress, const std::atomic<bool>* cancel = nullptr);

    // --make-index: downloads everything, builds the index and writes the two
    // release files and the small whole maps to folder/out. Returns 3 when the published index is already current.
    extern int makeIndex(const std::filesystem::path& folder);
}
