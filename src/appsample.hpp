#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Points from appsample's map (genshin-impact-map.appsample.com), lined up with
// HoYoLAB's so a tap on one can show the comments people left on the other.
// Their types are HoYoLAB's with an o in front (o17 is 17, a common chest), but
// the positions are in their own units, with their own scale for each map and
// a few regions moved.

namespace Appsample {
    // A point as appsample has it, in its own units (lng, lat, lat going up).
    // Underground it says HoYoLAB's group and floor, 0 where it doesn't.
    struct Point {
        int64_t id = 0;
        int map = 0;
        int type = 0;
        int group = 0, floor = 0;
        double lng = 0, lat = 0;
    };

    // A HoYoLAB point to pair, in HoYoLAB map units (y going down).
    struct Ours {
        int64_t id = 0;
        int map = 0;
        int type = 0;
        int area = 0;
        int group = 0, floor = 0;  // 0 and 0 on the surface
        double x = 0, y = 0;
    };

    // reads markers_all.v5.compressed.json
    extern bool decode(const std::string& body, std::vector<Point>& out);

    // How one map went, for the builder's log.
    struct MapResult {
        int ours = 0, theirs = 0, paired = 0;
        int unplaced = 0;       // our points in regions with no clear offset
        double scale = 0;       // appsample units per HoYoLAB pixel
        double residual = 0;    // median distance of the pairs after the fit, in HoYoLAB pixels
        std::string skipped;    // why the whole map has no pairs, empty when it has
    };

    struct Result {
        std::map<int64_t, int64_t> pairs;  // HoYoLAB point id: appsample point id
        std::map<int, MapResult> maps;     // by map id, every map of ours
    };

    // A pair is a point of the same type on the same floor, each the other's
    // nearest after the fit, closer than a few pixels.
    extern Result pair(const std::vector<Ours>& hoyolab, const std::vector<Point>& appsample);
}
