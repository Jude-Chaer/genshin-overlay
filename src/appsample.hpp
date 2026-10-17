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
    struct Point {
        int64_t id;
        int map;
        int type;
        int area;
        double x, y;
    };

    // reads markers_all.v5.compressed.json, x is lng and y is lat
    extern bool decode(const std::string& body, std::vector<Point>& out);

    // HoYoLAB point id to appsample point id, for the points that have one
    extern std::map<int64_t, int64_t> pair(const std::vector<Point>& hoyolab, const std::vector<Point>& appsample);
}
