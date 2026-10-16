#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <opencv2/core.hpp>
#include "locator/map_locator.hpp"

// Finds the player on the map from the minimap, while the big map is closed.
// The minimap always shows the map at the same size with north up. HoYoLAB's
// tiles are drawn at that size around where the player last was, and the
// minimap is searched for in them. That's about 2 milliseconds, 20 times a
// second.
//
// Under ground and under water the minimap shows a floor instead. HoYoLAB has
// a picture of every floor, those are tried when the tiles don't fit.
//
// When it isn't around the last spot (start, teleport) the places it was
// followed at lately are tried, then the part of the map the big map showed
// last. Then the map matching is asked, the same index that
// finds the big map. Where neither has it, the minimap is searched for on the
// whole map shrunk small, which takes seconds. That's done on a thread of its
// own, the minimap is still looked at meanwhile.
//
// Only runs while somebody asks, see GetPlayer.

namespace MiniMap {
    // What is known about where the player is, after a look.
    enum class State {
        // No minimap to read: a menu, a cutscene, a loading screen or the big
        // map covers it. Nothing is searched while it's covered. Once it's
        // back it's Found if the player is still at the last spot, Searching
        // if not.
        None,
        // The minimap is on the map at the spot, right now.
        Found,
        // It was Found a moment ago, the minimap never left the screen and it
        // doesn't fit anymore: a marker over it, or the zoom or the floor
        // changing. The player is still about at the last spot, so only
        // that's looked at. Searching after NEAR_FOR seconds of this.
        Near,
        // There's a minimap and the last spot doesn't help: the app just
        // started, a teleport, out of a domain. The places it can be at are
        // tried at every look and the whole map is being searched. Nowhere
        // once that search comes back with nothing.
        Searching,
        // The whole map was searched and the minimap fits nowhere: a domain,
        // or a place HoYoLAB has no map of. Still looked for like in
        // Searching, the whole map less and less often.
        Nowhere,
    };

    struct Spot {
        // the same as state == State::Found
        bool found = false;
        State state = State::None;
        // Seconds since it was last Found, 0 while it is. The spot is where
        // it was then. mapId is 0 as long as it never was.
        double age = 0.0;
        int mapId = 0;
        // the floor the minimap shows under ground or water, 0 on the surface
        int groupId = 0;
        int floorId = 0;
        double lat = 0.0;
        double lng = 0.0;
        // how much the minimap looks like the tiles there, -1 to 1
        double score = 0.0;
        // How far the game has the minimap zoomed in, 1 out in the world.
        // It shows UNITS_ACROSS / zoom map units.
        double zoom = 1.0;
        // the minimap's square on screen at that look, only set by GetPlayer
        RECT square{};
    };

    // side of the minimap when it's compared, in pixels
    constexpr int SIZE = 104;
    // map units from one side of that square to the other, out in the world
    constexpr double UNITS_ACROSS = 347.4;
    // the zoom in a city
    constexpr double CITY_ZOOM = 3.0;

    // the square around the minimap, in screen coords. Game is its client rect on screen.
    extern RECT rect(const RECT& game);
    // BGR or gray grab of that square to a gray SIZE x SIZE one
    extern cv::Mat shrink(const cv::Mat& grab);

    // reads the tile grids from the map data's manifest
    extern void Load(const std::filesystem::path& dataFolder);
    // Looks for the minimap around a spot it was at lately. Not found if it
    // went too far since, or if there's no minimap on screen. Wide looks 4
    // times as far around the spot, for about 5 times the work.
    extern Spot nearby(const cv::Mat& mini, int mapId, double lat, double lng, bool wide = false, double zoom = 1.0);
    // looks for it on every map, anywhere
    extern Spot anywhere(const cv::Mat& mini, double zoom = 1.0);

    // The map matching: where a grab of the minimap's square is on the map,
    // its middle as the spot.
    typedef std::function<MapLocator::Result(const cv::Mat& grab)> Locate;

    // a part of a map, in map units
    struct Area {
        int mapId = 0;
        double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
    };

    class Loader;

    // Follows the player from one look at the minimap to the next. Every
    // look tries the cheap things first and stops at the first that finds
    // it: around the last spot, then the quick places, then what a search
    // answered. What's searched and how often goes by the State.
    //
    // The game zooms the minimap in some places. A zoom it doesn't know yet
    // is found by trying a whole range of them at the last spot, and kept
    // if it stays.
    class Watcher {
    public:
        // With threads of its own a look never waits: the searches that take
        // long run on one and Look gets what they found at a later Look,
        // pictures that aren't in memory are read or downloaded on another.
        // Without, Look does all of it itself and takes as long as it takes.
        explicit Watcher(Locate locate = nullptr, bool ownThreads = false);
        ~Watcher();
        Watcher(const Watcher&) = delete;
        Watcher& operator=(const Watcher&) = delete;
        // Grab is the minimap's square as it was grabbed, seconds when.
        // Searched, if given, tells whether the whole map was searched.
        const Spot& Look(const cv::Mat& grab, double seconds, bool* searched = nullptr);
        // search the whole map on the next look that doesn't find it nearby
        void SearchSoon(double seconds);
        // What the big map showed when it closed. A teleport lands somewhere
        // in there, so that's searched before the whole map.
        void SearchFirst(const Area& area);
        // A place it was followed at, the newest last. The quick places look
        // around these too, the last spot can be a wrong find.
        void Remember(const Spot& spot);
        const std::vector<Spot>& Places() const { return m_places; }
        // a search is running on its thread
        bool Searching() const;

    private:
        // What a search is asked: the minimap as it was then and what was
        // known then. It runs on another thread, so it gets its own copies.
        struct Search {
            cv::Mat mini;
            Spot last;
            std::vector<double> zooms;
            // the part of the map the big map showed last, if any
            Area area;
            // there's a minimap on screen
            bool there = false;
            // around the last spot at any zoom and on its other layers
            bool around = false;
            // the whole maps
            bool everywhere = false;
            // every floor at every zoom
            bool floors = false;
            // it was due, not only asked for the area
            bool due = false;
        };
        struct Answer {
            Spot spot;
            Search search;
        };

        // the steps of a look, in their order
        Spot around(const cv::Mat& mini, bool sign, double seconds, bool loaded);
        void cameUp(double seconds);
        Spot quick(const cv::Mat& grab, const cv::Mat& mini, double seconds);
        Spot taken(const Answer& answer, const cv::Mat& mini, bool there, double seconds, bool* searched);
        State lost(bool there, double seconds) const;
        Search wanted(State state, const cv::Mat& mini, bool there, double seconds) const;
        const Spot& unfound(State state, double seconds);

        Spot locate(const cv::Mat& grab, const cv::Mat& mini, bool there, double seconds);
        void keep(double zoom, double seconds);
        void forgetNew();
        void ask(Search search);
        bool answered(Answer& answer);
        void drop();
        void run();
        // these are the search, on its thread
        Answer find(const Search& search);
        Spot inArea(const Search& search);
        Spot anyZoom(const Search& search);
        Spot anyFloor(const Search& search);
        Spot settle(const Search& search, Spot best);

        Locate m_locate;
        Spot m_last;
        // the zooms seen so far
        std::vector<double> m_zooms{ 1.0, CITY_ZOOM };
        // The newest of them and when it was found, 0 once it stays.
        double m_newZoom = 0.0, m_newZoomAt = 0.0;
        cv::Mat m_lastMini;
        std::vector<Spot> m_places;
        // the minimap the quick places didn't have, they aren't asked twice
        cv::Mat m_missed;
        // the minimap the area was last searched for, the same here
        cv::Mat m_areaAsked;
        // where a search found a minimap that had changed by the time it answered
        Spot m_answered;
        // when it was last Found
        double m_foundAt = 0.0;
        // it's Near until then
        double m_nearUntil = 0.0;
        // When a search is due next, and how long after that one the next.
        // The wait doubles with every whole map search that finds nothing.
        double m_searchAt = 0.0, m_searchWait = 1.0;
        // when every floor at every zoom may be searched again
        double m_floorsAt = 0.0;
        // when the other layers at the spot get compared next
        double m_layersAt = 0.0;
        // how well it fit at the last look at the other layers
        double m_layerScore = 0.0;
        // there was a minimap in the last look
        bool m_there = false;
        // The N or Paimon was there and went away: something covers the
        // minimap, the big map mostly. No spot until one of them is back.
        bool m_sign = false, m_covered = false;
        // the whole map was searched since it got lost, and it wasn't there
        bool m_empty = false;
        Area m_area;

        // reads and downloads for the looks, none without threads
        std::unique_ptr<Loader> m_loader;
        // how much of its work was done at the last look
        int m_loaded = 0;
        // when the floor pictures around the spot get asked for next
        double m_fetchAt = 0.0;

        std::thread m_thread;
        mutable std::mutex m_mutex;
        std::condition_variable m_wake;
        // guarded by m_mutex
        bool m_quit = false, m_asked = false, m_busy = false, m_done = false;
        Search m_search;
        Answer m_answer;
        // the search that's running isn't wanted anymore
        std::atomic<bool> m_dropped{ false };

        // Only the search touches these. The area they're of, its whole map
        // shrunk small and the area drawn from the tiles, by zoom. Kept until
        // the search is dropped, which is also when the spot is found.
        Area m_areaOf;
        cv::Mat m_areaMap;
        std::map<double, cv::Mat> m_areaDrawn;
    };

    // OpenCV splits its work over helper threads, and those keep spinning
    // for a while after. A look is too short for that to pay. While one of
    // these lives, OpenCV stays on the thread that made it.
    struct OneThread {
        OneThread();
        ~OneThread();
    };

    // starts the thread, it loads the map data once that was checked
    extern void Start(const std::filesystem::path& dataFolder);
    extern void Stop();
    // Where the player is. The minimap is only watched while this gets called,
    // it stops two seconds after the last call. Not found while the big map
    // is open.
    extern Spot GetPlayer();
    // An extension asked for the player in the last two seconds. The overlay
    // keeps running the extensions then, also with map and menu closed.
    extern bool Wanted();
}
