#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "item_catalog.hpp"
#include "net_stop.hpp"

// HoYoLAB's map items (points, types, regions, icons) of every map, built once
// a week by the map-index workflow and published as one pack on the
// "map-index" release, next to the map index.
//
// What's in a pack, format 2:
//   tree.json              the groups and their types, [{id, name, children: [{id, name}]}]
//   areas.json             the regions, [{id, name, map_id, l_x, l_y, r_x, r_y}]
//   positions/<type>.json  [{id, map_id, x_pos, y_pos, area_id, point_group, appsample}]
//                          point_group is {group_id, floor_id} for a point underground,
//                          null otherwise. appsample is there when appsample has the point.
//   icons/<type>.png       at most 64 px
// HoYoLAB's own key names, written without spaces. A point's region is its
// area_id, not the box it sits in (a few Inazuma puzzles are outside Inazuma's).
//
// The pack is those files one after the other, gzipped, and items.json says
// what's in it. The overlay keeps the one it uses as a single file,
// items/items.pack: the release's items.json on the first line, then the
// gzipped pack as it came. It's written only after the pack has loaded, and in
// one go (temp file and rename), so it's never half old, half new.

namespace ItemFiles {
    inline constexpr int FORMAT = 2;
    inline const std::string PACK_FILE = "items.pack.gz";
    inline const std::string META_FILE = "items.json";
    inline const std::string INSTALLED_FILE = "items.pack";

    // What the builder downloads, before it's written as the folder.
    struct Source {
        std::map<int, nlohmann::json> trees;  // map id: HoYoLAB's label tree
        std::map<int, nlohmann::json> areas;  // map id: HoYoLAB's region list
        // type: its points on every map, each with its map_id, in the order HoYoLAB sent them
        std::map<int, std::vector<nlohmann::json>> points;
        // HoYoLAB point id: appsample point id
        std::map<int64_t, int64_t> pairs;
        // points HoYoLAB sent that were left out, by the field that's missing ("label_id": 3)
        std::map<std::string, size_t> dropped;
    };

    // Writes tree.json, areas.json and the positions. Groups, types and regions
    // are once each, in the order of the first map that has them.
    extern bool write(const Source& source, const std::filesystem::path& folder);

    // Every file in the folder as "path\tsize\n" and then its bytes, sorted
    // by path, so the same files always make the same pack.
    extern bool pack(const std::filesystem::path& folder, std::string& raw);
    // Calls each with every file in a raw pack, in its order. False on a pack
    // that's cut short, has a name that isn't a plain relative path, or when
    // each says stop.
    extern bool readPack(const std::string& raw, const std::function<bool(const std::string& name, std::string_view data)>& each);
    // Writes the icons of these types from a raw pack into folder/icons, for
    // the ones HoYoLAB didn't send this time. How many it had.
    extern int keepIcons(const std::string& raw, const std::vector<int>& types, const std::filesystem::path& folder);

    // Why the items aren't the release's, for the menu line and the log.
    //   NoNetwork: the release couldn't be reached
    //   NewerFormat: the release's pack is a format this overlay can't read
    //   BrokenPack: the release's pack (or its items.json) didn't check out or didn't load
    //   CantSave: the pack is fine and in use, but items.pack couldn't be written
    //   DownloadOff: the player turned the download off
    enum class Reason { None, NoNetwork, NewerFormat, BrokenPack, CantSave, DownloadOff };

    // Checks a downloaded pack against its items.json (format, size, sha256)
    // and loads it. The catalog when every point and file of it loaded, else
    // nullptr and why in problem.
    extern std::shared_ptr<ItemCatalog::Catalog> open(const std::string& packed, const nlohmann::json& meta,
        const Net::Stopper* stop, std::string& problem);
    // Writes the pack as folder/items.pack. False when it can't.
    extern bool install(const std::string& packed, const nlohmann::json& meta, const std::filesystem::path& folder);

    // Where the overlay's map items are and how they got there.
    //   None: there are none
    //   Downloading: a new pack is coming, percent says how far
    //   Ready: the release's pack is in
    //   Old: an older pack is in, from built
    // reason says why it isn't Ready (or, with CantSave, why it won't be next start).
    enum class State { None, Downloading, Ready, Old };
    struct Status {
        State state = State::None;
        Reason reason = Reason::None;
        int percent = 0;
        std::string built;      // the pack's date, "2026-10-17", for Old
        bool started = false;   // false until the first check said how it's going
        bool checking = false;  // a check is running (or about to)
    };

    // A file from the map-index release by name, into body. False when it
    // couldn't be had; progress as for Net::get. The tests stand in for it.
    using Fetch = std::function<bool(const std::string& name, std::string& body, const Net::Progress& progress)>;
    // stopper, when given, cuts a download short from another thread
    extern Fetch fromRelease(Net::Stopper* stopper = nullptr);

    // One check: with download on, gets the release's pack when items.pack
    // isn't that one; then publishes the catalog of the best pack there is.
    // Tells onChange about every change on the way and returns the last one.
    // stop makes it give up at the next piece of a download or a load.
    // inUse is the catalog published now: a check never replaces it with the
    // same pack or with the one on disk, it only says how it went.
    // download is read as it goes: unticked partway, the download stops and says so.
    extern Status update(const std::filesystem::path& folder, const Fetch& fetch, const std::atomic<bool>& download,
        const Net::Stopper& stop, const std::function<void(const Status&)>& onChange,
        std::shared_ptr<const ItemCatalog::Catalog> inUse = nullptr);

    // how long it waits before checking again with no network (the menu line says "every minute")
    inline constexpr std::chrono::seconds RETRY{ 60 };
    // true when update couldn't reach the release, the one case that checks again on its own
    // after RETRY, with or without a list there; the rest waits for the player (the menu's
    // buttons) or the next start
    extern bool waitsForNetwork(const Status& status);

    // update on a thread of its own: at start, again every minute while the
    // release can't be reached, and whenever CheckNow or SetDownload asks.
    // Started and stopped in main; AskToStop lets every module start closing
    // before main waits for each.
    extern void Start(const std::filesystem::path& folder, bool download);
    extern void AskToStop();
    extern void Stop();
    extern Status GetStatus();
    // a check right away, for the menu's buttons
    extern void CheckNow();
    // the menu's checkbox: off, it doesn't go online for items; on, it checks right away
    extern void SetDownload(bool on);

    // What a pack holds on one map, as items.json's "maps" says it.
    struct MapCount {
        int points = 0;
        int types = 0;  // types with a point on it
        int pairs = 0;  // points with an appsample pair
    };
    extern std::map<int, MapCount> countMaps(const ItemCatalog::Catalog& catalog);

    // The builder's check of a raw pack before it's published: what would keep
    // it from loading in the overlay, a map of maps with no points, and a map
    // whose points, types or pairs fell sharply against the published
    // items.json (unless allowDrop). Empty when it can go out; counts is filled
    // when it loads.
    extern std::vector<std::string> checkBuilt(const std::string& raw, const std::vector<int>& maps,
        const nlohmann::json& published, bool allowDrop, std::map<int, MapCount>& counts);

    // --make-items <folder> [--allow-drop]: downloads everything into
    // folder/items, checks it, then writes the pack and items.json to
    // folder/out. 1 when it fails or its check does, or when the published
    // items.json can't be read to compare with, 3 when the published pack is
    // the same. --allow-drop also leaves out points HoYoLAB sends without a
    // good id or type, and replaces a published items.json that doesn't read.
    extern int make(const std::filesystem::path& folder, bool allowDrop = false);
}
