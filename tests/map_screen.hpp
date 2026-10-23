#pragma once
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgcodecs.hpp>
#include "map_data.hpp"

// Real screenshots of the big map and HoYoLAB's floors, for the tests of
// what's read off the map screen.
namespace MapScreen {
    inline const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data";
    // the screenshots are cut to this part of a 2000x1125 picture
    constexpr int CUT_X = 1330, CUT_Y = 85;

    inline cv::Mat readPicture(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::vector<uchar> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return bytes.empty() ? cv::Mat() : cv::imdecode(bytes, cv::IMREAD_COLOR);
    }

    // a screenshot back at its place in a 2000x1125 picture
    inline cv::Mat screen(const std::string& name) {
        cv::Mat part = readPicture(DATA / "map_screen" / (name + ".png"));
        REQUIRE_FALSE(part.empty());
        cv::Mat frame(1125, 2000, CV_8UC3, cv::Scalar::all(0));
        part.copyTo(frame(cv::Rect(CUT_X, CUT_Y, part.cols, part.rows)));
        return frame;
    }

    // all of HoYoLAB's underground floors, from data/floors.tsv
    inline std::vector<MapData::Floor> floors() {
        std::vector<MapData::Floor> out;
        std::ifstream file(DATA / "floors.tsv");
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            MapData::Floor f;
            if (fields >> f.mapId >> f.groupId >> f.floorId >> f.left >> f.top >> f.right >> f.bottom) out.push_back(f);
        }
        REQUIRE_FALSE(out.empty());
        return out;
    }
}
