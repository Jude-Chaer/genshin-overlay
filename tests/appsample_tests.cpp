#include <catch2/catch_test_macros.hpp>
#include <random>
#include <set>
#include "appsample.hpp"

using Appsample::Point;

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
    CHECK(points[1].x == 0.5);
    CHECK(points[1].y == 0.2);
}

TEST_CASE("appsample's file with a code isn't read") {
    std::string body = R"({"code":3,"compressionVersion":1,"headers":["id","type","mid","lng","lat"],
        "data":{"id":[1],"type":["o17"],"mid":[2],"lng":[0.5],"lat":[0.2]}})";
    std::vector<Point> points;
    CHECK_FALSE(Appsample::decode(body, points));
}

TEST_CASE("points pair up across a different scale and a moved region") {
    std::mt19937 random(7);
    std::uniform_real_distribution<double> place(-4000, 4000);
    std::vector<Point> ours, theirs;
    const double scale = 1.2e-4;
    int64_t next = 1;
    for (int area = 1; area <= 2; area++) {
        // the second region sits 2000 px further right on appsample
        double moved = area == 2 ? 2000 : 0;
        for (int type = 1; type <= 40; type++) {
            for (int i = 0; i < 6; i++) {
                double x = place(random) + area * 9000, y = place(random);
                int64_t id = next++;
                ours.push_back({ id, 2, type, area, x, y });
                // a few on each side that the other doesn't have
                if (i == 5 && type % 3 == 0) continue;
                theirs.push_back({ id + 100000, 2, type, 0, (x + moved + 1.5) * scale, -(y - 2) * scale });
            }
            if (type % 4 == 0) theirs.push_back({ next++ + 100000, 2, type, 0, (place(random) + area * 9000) * scale, place(random) * scale });
        }
    }

    std::map<int64_t, int64_t> pairs = Appsample::pair(ours, theirs);
    int right = 0, wrong = 0;
    for (const auto& [ourId, theirId] : pairs) {
        if (theirId == ourId + 100000) right++;
        else wrong++;
    }
    std::set<int64_t> ourIds;
    for (const Point& point : ours) ourIds.insert(point.id);
    int both = 0;
    for (const Point& point : theirs) both += ourIds.count(point.id - 100000) ? 1 : 0;
    CHECK(wrong == 0);
    CHECK(right == both);
}

TEST_CASE("points on a map appsample doesn't have stay unpaired") {
    std::vector<Point> ours = { { 1, 9, 17, 6, 100, 200 } };
    std::vector<Point> theirs = { { 5, 2, 17, 0, 0.01, -0.02 } };
    CHECK(Appsample::pair(ours, theirs).empty());
}
