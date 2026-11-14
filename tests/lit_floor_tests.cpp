#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "locator/lit_floor.hpp"
#include "locator/map_locator.hpp"

namespace fs = std::filesystem;

// Real map screenshots with the floor the game draws lit read by hand, and
// HoYoLAB's art of the floors in view, in data/lit_floor.

static const fs::path DATA = fs::path(__FILE__).parent_path() / "data" / "lit_floor";

static cv::Mat readPicture(const fs::path& path, int flags) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uchar> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return bytes.empty() ? cv::Mat() : cv::imdecode(bytes, flags);
}

static std::vector<std::vector<std::string>> rows(const fs::path& path) {
    std::vector<std::vector<std::string>> out;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::stringstream text(line);
        for (std::string field; std::getline(text, field, '\t');) fields.push_back(field);
        out.push_back(fields);
    }
    return out;
}

static const std::map<int, MapLocator::FloorArt>& floors() {
    static std::map<int, MapLocator::FloorArt> floors;
    if (!floors.empty()) return floors;
    for (const auto& f : rows(DATA / "floors.tsv")) {
        int floorId = std::stoi(f[1]);
        cv::Mat image = readPicture(DATA / "floors" / (f[1] + ".png"), cv::IMREAD_UNCHANGED);
        floors[floorId] = MapLocator::MakeFloorArt(image, std::stoi(f[0]), floorId,
            std::stod(f[2]), std::stod(f[3]), std::stod(f[4]), std::stod(f[5]));
    }
    return floors;
}

TEST_CASE("the floor the game draws lit, not the faded ones round it", "[lit_floor]") {
    auto shots = rows(DATA / "shots.tsv");
    REQUIRE(shots.size() == 6);
    for (const auto& f : shots) {
        INFO(f[0] << ": " << f[6]);
        cv::Mat shot = readPicture(DATA / (f[0] + ".png"), cv::IMREAD_GRAYSCALE);
        REQUIRE_FALSE(shot.empty());
        MapLocator::ScreenView view{ std::stod(f[2]), std::stod(f[3]), std::stod(f[4]) };

        std::vector<const MapLocator::FloorArt*> arts;
        std::stringstream inView(f[5]);
        for (std::string id; std::getline(inView, id, ',');) {
            auto art = floors().find(std::stoi(id));
            REQUIRE(art != floors().end());
            REQUIRE_FALSE(art->second.gray.empty());
            arts.push_back(&art->second);
        }
        int lit = MapLocator::LitFloor(shot, MapLocator::GameUiMask(shot.cols, shot.rows), view, arts);
        CHECK((lit < 0 ? 0 : arts[lit]->floorId) == std::stoi(f[1]));
    }
}

TEST_CASE("floor art is kept small, the map units it covers stay", "[lit_floor]") {
    cv::Mat image(2000, 1000, CV_8UC4, cv::Scalar(10, 20, 30, 255));
    MapLocator::FloorArt art = MapLocator::MakeFloorArt(image, 5, 7, -100, -200, 300, 600);
    CHECK(art.gray.rows == MapLocator::FLOOR_ART_SIZE);
    CHECK(art.gray.cols == MapLocator::FLOOR_ART_SIZE / 2);
    CHECK(art.alpha.size() == art.gray.size());
    CHECK(art.left == -100);
    CHECK(art.bottom == 600);
    CHECK(art.floorId == 7);
}
