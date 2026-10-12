#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <map>
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
// With no last spot (start, teleport) the part of the map the big map showed
// last is searched first. Then the map matching is asked, the same index that
// finds the big map. Where neither has it, the minimap is searched for on the
// whole map shrunk small, which takes seconds.
//
// Only runs while somebody asks, see GetPlayer.

namespace MiniMap {
    struct Spot {
        // the minimap was on screen and found on the map the last time we looked
        bool found = false;
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

    // Follows the player from one look at the minimap to the next: around the
    // last spot first, anywhere when that fails, less and less often while
    // it stays lost.
    //
    // The game zooms the minimap in some places. A zoom it doesn't know yet
    // is found by trying a whole range of them at the last spot, and kept.
    // a part of a map, in map units
    struct Area {
        int mapId = 0;
        double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
    };

    class Watcher {
    public:
        explicit Watcher(Locate locate = nullptr) : m_locate(std::move(locate)) {}
        // Grab is the minimap's square as it was grabbed, seconds when.
        // Searched, if given, tells whether the whole map was searched.
        const Spot& Look(const cv::Mat& grab, double seconds, bool* searched = nullptr);
        // search the whole map on the next look that doesn't find it nearby
        void SearchSoon(double seconds);
        // What the big map showed when it closed. A teleport lands somewhere
        // in there, so that's searched before the whole map.
        void SearchFirst(const Area& area) { m_area = area; m_areaMap.release(); m_areaDrawn.clear(); }
        // there's a minimap on screen and it wasn't found
        bool Lost() const { return m_shown && !m_last.found; }

    private:
        Spot locate(const cv::Mat& grab, const cv::Mat& mini, bool there);
        Spot inArea(const cv::Mat& mini);
        Spot anyZoom(const cv::Mat& mini);
        Spot anyFloor(const cv::Mat& mini);
        Spot settle(const cv::Mat& mini, Spot best);

        Locate m_locate;
        Spot m_last;
        // the zooms seen so far
        std::vector<double> m_zooms{ 1.0, CITY_ZOOM };
        cv::Mat m_lastMini;
        double m_searchAt = 0.0, m_searchWait = 1.0;
        // when every floor at every zoom may be searched again
        double m_floorsAt = 0.0;
        // no whole map search before this, it was only just lost
        double m_farAt = 0.0;
        // when the other layers at the spot get compared next
        double m_layersAt = 0.0;
        // how well it fit at the last look at the other layers
        double m_layerScore = 0.0;
        // there was a minimap in the last look
        bool m_shown = false;
        // The N or Paimon was there and went away: something covers the
        // minimap, the big map mostly. No spot until one of them is back.
        bool m_sign = false, m_covered = false;
        Area m_area;
        // its whole map shrunk small, kept until the spot is found
        cv::Mat m_areaMap;
        // the same area drawn from the tiles, by zoom
        std::map<double, cv::Mat> m_areaDrawn;
    };

    // OpenCV splits its work over helper threads, and those keep spinning
    // for a while after. A look is too short for that to pay. While one of
    // these lives, OpenCV stays on the thread that made it.
    struct OneThread {
        OneThread();
        ~OneThread();
    };

    // Load, and starts the thread
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
