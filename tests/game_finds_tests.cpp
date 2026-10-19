#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/game_finds.hpp"
#include "packets/game_oculi.hpp"

using namespace GameFinds;

static std::string varint(uint64_t v) {
    std::string s;
    while (v >= 0x80) {
        s += char(v & 0x7f | 0x80);
        v >>= 7;
    }
    s += char(v);
    return s;
}
static std::string number(uint32_t field, uint64_t v) { return varint(field << 3) + varint(v); }
static std::string bytes(uint32_t field, const std::string& v) { return varint(field << 3 | 2) + varint(v.size()) + v; }
static std::string real(uint32_t field, float v) {
    std::string s = varint(field << 3 | 5);
    char b[4];
    std::memcpy(b, &v, 4);
    return s + std::string(b, 4);
}
static std::string vec(float x, float y, float z) { return real(1, x) + real(2, y) + real(3, z); }

static GamePackets::Command received(uint16_t id, const std::string& body) {
    GamePackets::Command command;
    command.id = id;
    command.body = body;
    return command;
}

// the player at x, z, what the game gets from us as the player moves
static GamePackets::Command moved(float x, float z) {
    std::string move = number(1, 0x200001) + bytes(2, bytes(1, vec(x, 200.0f, z)));
    GamePackets::Command command;
    command.id = GameChests::MOVED;
    command.direction = GamePackets::Direction::Sent;
    command.body = bytes(8, bytes(6, bytes(9, bytes(15, move))));
    return command;
}

// a Dendroculus from recorded traffic, and its marker in a positions folder of its own
static constexpr float X = -406.232f, Z = 3567.559f;
static constexpr uint32_t ENTITY = 8394556;
static constexpr int64_t POINT = 7;

static std::filesystem::path positionsWithOneDendroculus() {
    std::filesystem::path folder = std::filesystem::temp_directory_path() / "genshin-overlay-finds-test";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "403.json") << "[{\"id\":" << POINT << ",\"map_id\":2,\"x_pos\":" << (-Z - 750.0)
        << ",\"y_pos\":" << (-X + 2321.0) << "}]";
    return folder;
}

TEST_CASE("A puzzle that comes in already lit gives a ? and isn't found") {
    std::filesystem::path folder = positionsWithOneDendroculus();
    std::ofstream(folder / "71.json") << "[{\"id\":8,\"map_id\":2,\"x_pos\":100,\"y_pos\":100}]";
    Finder finder(folder);
    std::vector<Found> found;
    auto torch = [](uint32_t entity, uint32_t config, double mapX) {
        std::string where = bytes(1, vec(float(2321.0 - 100), 200.0f, float(-750.0 - mapX)));
        std::string kind = number(1, 70310001) + number(2, 133503104) + number(3, config) + number(6, 201);
        return bytes(8, number(2, entity) + bytes(4, where) + bytes(13, kind));
    };
    finder.Feed(received(GameChests::APPEARED, torch(1, 1, 100) + torch(2, 2, 104)), 1, found);
    // the moment is looked at when the next one comes
    CHECK(finder.Feed(received(0, ""), 2, found));
    CHECK(found.empty());
    REQUIRE(finder.Hints().count(8));
    CHECK(finder.Hints().at(8) == GameChests::Hint::ProbablyOpened);
}

TEST_CASE("Something found leaves the hints, it doesn't stay still there") {
    Finder finder(positionsWithOneDendroculus());
    std::vector<Found> found;
    std::string where = bytes(1, vec(X, 208.835f, Z));
    std::string kind = number(1, 70590063) + number(2, 133301245) + number(3, 245010);
    finder.Feed(received(GameChests::APPEARED, bytes(8, number(2, ENTITY) + bytes(4, where) + bytes(13, kind))), 500, found);

    REQUIRE(finder.Feed(moved(X, Z), 1000, found));
    REQUIRE(finder.Hints().count(POINT));
    CHECK(finder.Hints().at(POINT) == GameChests::Hint::StillThere);

    finder.Feed(received(GameOculi::GOT_ITEM, number(11, 11) + bytes(13, number(1, 107017) + number(2, 1))), 2000, found);
    finder.Feed(received(GameOculi::LEFT, number(5, ENTITY) + number(11, 2)), 2000, found);
    REQUIRE(found.size() == 1);
    CHECK(found[0].point == POINT);

    // looked at again once the player has been there a while
    REQUIRE(finder.Feed(moved(X, Z), 7000, found));
    REQUIRE(finder.Hints().count(POINT));
    CHECK(finder.Hints().at(POINT) == GameChests::Hint::ProbablyOpened);
}
