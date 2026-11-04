#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include "appsample.hpp"

using Appsample::Ours;
using Appsample::Point;
namespace fs = std::filesystem;

TEST_CASE("appsample's file is read with the ids as steps") {
    std::string body = R"({"code":0,"compressionVersion":1,"headers":["id","type","mid","level","lng","lat","meta"],
        "data":{"id":[100,1,5],"type":["o17","o17","x9"],"mid":[2,2,7],"level":[0,0,0],
        "lng":[0.25,0.5,0.1],"lat":[-0.1,0.2,0.3],"meta":["","",""]}})";
    std::vector<Point> points;
    REQUIRE(Appsample::decode(body, points));
    // the third one's type isn't one of HoYoLAB's
    REQUIRE(points.size() == 2);
    CHECK(points[0].id == 100);
    CHECK(points[1].id == 101);
    CHECK(points[1].type == 17);
    CHECK(points[1].map == 2);
    CHECK(points[1].lng == 0.5);
    CHECK(points[1].lat == 0.2);
}

TEST_CASE("appsample's floor is read from meta") {
    // the meta values are real ones from appsample's file
    std::string body = R"({"code":0,"compressionVersion":1,"headers":["id","type","mid","level","lng","lat","meta"],
        "data":{"id":[1,1,1,1,1],"type":["o17","o17","o17","o17","o17"],"mid":[2,2,2,2,2],"level":[0,0,0,0,0],
        "lng":[0,0,0,0,0],"lat":[0,0,0,0,0],
        "meta":["l=96251|197|282","i;l=96257_96593_97449|203|292","","i","l=96251|197|"]}})";
    std::vector<Point> points;
    REQUIRE(Appsample::decode(body, points));
    REQUIRE(points.size() == 5);
    CHECK((points[0].group == 197 && points[0].floor == 282));
    CHECK((points[1].group == 203 && points[1].floor == 292));
    CHECK((points[2].group == 0 && points[2].floor == 0));
    CHECK((points[3].group == 0 && points[3].floor == 0));
    // a group with no floor
    CHECK((points[4].group == 197 && points[4].floor == 0));
}

TEST_CASE("appsample's points are on the surface when meta is missing or short") {
    std::vector<Point> points;
    // a meta column that's shorter than the others is left out
    std::string byColumn = R"({"code":0,"compressionVersion":1,"headers":["id","type","mid","lng","lat","meta"],
        "data":{"id":[1,1],"type":["o17","o17"],"mid":[2,2],"lng":[0,0],"lat":[0,0],"meta":["l=96251|197|282"]}})";
    REQUIRE(Appsample::decode(byColumn, points));
    REQUIRE(points.size() == 2);
    CHECK(points[0].group == 0);
    // one array per point, the second one stops before meta
    points.clear();
    std::string byRow = R"({"code":0,"headers":["id","type","mid","lng","lat","meta"],
        "data":[[1,"o17",2,0,0,"l=96251|197|282"],[2,"o17",2,0,0]]})";
    REQUIRE(Appsample::decode(byRow, points));
    REQUIRE(points.size() == 2);
    CHECK((points[0].group == 197 && points[0].floor == 282));
    CHECK(points[1].group == 0);
}

TEST_CASE("appsample's file with a code isn't read") {
    std::string body = R"({"code":3,"compressionVersion":1,"headers":["id","type","mid","lng","lat"],
        "data":{"id":[1],"type":["o17"],"mid":[2],"lng":[0.5],"lat":[0.2]}})";
    std::vector<Point> points;
    CHECK_FALSE(Appsample::decode(body, points));
}

TEST_CASE("appsample's file with a code that isn't a number isn't read") {
    std::string body = R"({"code":"busy","compressionVersion":"1","headers":["id","type","mid","lng","lat"],
        "data":{"id":[1],"type":["o17"],"mid":[2],"lng":[0.5],"lat":[0.2]}})";
    std::vector<Point> points;
    CHECK_FALSE(Appsample::decode(body, points));
}

TEST_CASE("points pair up across a different scale and a moved region") {
    std::mt19937 random(7);
    std::uniform_real_distribution<double> place(-4000, 4000);
    std::vector<Ours> ours;
    std::vector<Point> theirs;
    const double scale = 1.2e-4;
    int64_t next = 1;
    for (int area = 1; area <= 2; area++) {
        // the second region sits 2000 px further right on appsample
        double moved = area == 2 ? 2000 : 0;
        for (int type = 1; type <= 40; type++) {
            for (int i = 0; i < 6; i++) {
                double x = place(random) + area * 9000, y = place(random);
                int64_t id = next++;
                ours.push_back({ id, 2, type, area, 0, 0, x, y });
                // a few on each side that the other doesn't have
                if (i == 5 && type % 3 == 0) continue;
                theirs.push_back({ id + 100000, 2, type, 0, 0, (x + moved + 1.5) * scale, -(y - 2) * scale });
            }
            if (type % 4 == 0) theirs.push_back({ next++ + 100000, 2, type, 0, 0, (place(random) + area * 9000) * scale, place(random) * scale });
        }
    }

    Appsample::Result result = Appsample::pair(ours, theirs);
    int right = 0, wrong = 0;
    for (const auto& [ourId, theirId] : result.pairs) {
        if (theirId == ourId + 100000) right++;
        else wrong++;
    }
    std::set<int64_t> ourIds;
    for (const Ours& point : ours) ourIds.insert(point.id);
    int both = 0;
    for (const Point& point : theirs) both += ourIds.count(point.id - 100000) ? 1 : 0;
    CHECK(wrong == 0);
    CHECK(right == both);
    CHECK(result.maps[2].paired == right);
    CHECK(result.maps[2].unplaced == 0);
}

TEST_CASE("points on a map appsample doesn't have stay unpaired") {
    std::vector<Ours> ours = { { 1, 9, 17, 6, 0, 0, 100, 200 } };
    std::vector<Point> theirs = { { 5, 2, 17, 0, 0, 0.01, -0.02 } };
    Appsample::Result result = Appsample::pair(ours, theirs);
    CHECK(result.pairs.empty());
    CHECK(result.maps[9].skipped == "appsample has no points on it");
}

// Maps 37 and 40 cut from the real files (cut.py next to them). The pairs below
// were read off by eye, see "Appsample pairs" in tests/data/items/answers.md:
// chests and pearls that sit in twos or threes 2 to 4 px apart, where
// appsample's map is a pixel or two off inside a region.
TEST_CASE("close points on the real maps pair with the right one") {
    const fs::path DATA = fs::path(__FILE__).parent_path() / "data" / "items" / "appsample";
    std::vector<Ours> ours;
    std::ifstream rows(DATA / "ours.tsv");
    std::string line;
    while (std::getline(rows, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        Ours point;
        row >> point.id >> point.map >> point.type >> point.area >> point.group >> point.floor >> point.x >> point.y;
        ours.push_back(point);
    }
    REQUIRE(ours.size() == 1837);
    std::ifstream file(DATA / "markers.json", std::ios::binary);
    std::ostringstream body;
    body << file.rdbuf();
    std::vector<Point> theirs;
    REQUIRE(Appsample::decode(body.str(), theirs));
    REQUIRE(theirs.size() == 1774);

    Appsample::Result result = Appsample::pair(ours, theirs);
    const std::map<int64_t, int64_t> checked = {
        // map 40, Ancient Moon Pearl, underground
        { 100287, 100942 }, { 100288, 100941 }, { 100289, 100940 },
        { 100290, 100939 }, { 100291, 100938 }, { 100292, 100937 },
        // map 37
        { 96839, 97443 }, { 96840, 97442 }, { 96841, 97441 },
        { 99146, 99749 }, { 99147, 99748 },
        { 97335, 98125 }, { 97336, 98124 },
        { 99185, 99710 }, { 99186, 99709 },
        { 99208, 99687 }, { 99209, 99686 },
    };
    for (const auto& [ourId, theirId] : checked) {
        INFO("HoYoLAB point " << ourId);
        auto found = result.pairs.find(ourId);
        REQUIRE(found != result.pairs.end());
        CHECK(found->second == theirId);
    }
    for (int map : { 37, 40 }) {
        INFO("map " << map);
        CHECK(result.maps[map].skipped.empty());
        CHECK(result.maps[map].unplaced == 0);
        CHECK(result.maps[map].residual < 1.0);
    }
}
