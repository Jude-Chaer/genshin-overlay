#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/game_chests.hpp"

using namespace GameChests;

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

// one object in the game's APPEARED message
static std::string object(uint32_t entity, uint32_t gadget, uint32_t group, uint32_t config, float x, float y, float z) {
    std::string where = bytes(1, real(1, x) + real(2, y) + real(3, z));
    return bytes(8, number(2, entity) + bytes(4, where) + bytes(13, number(1, gadget) + number(2, group) + number(3, config)));
}

// a chest the game has at x, z sits at HoYoLAB's (mapX, mapY)
static Chest chestAt(uint32_t gadget, double mapX, double mapY, uint32_t config = 1) {
    Chest chest;
    chest.group = 133104041;
    chest.config = config;
    chest.gadget = gadget;
    chest.x = float(2321.0 - mapY);
    chest.z = float(-750.0 - mapX);
    return chest;
}

TEST_CASE("ReadOpened gets the chest out of the game's message") {
    // a chest from recorded traffic, group 133104041 config 72
    std::string body = "\x58\x48\x60\xa9\x83\xbc\x3f";
    Chest chest;
    REQUIRE(ReadOpened(body, chest));
    CHECK(chest.group == 133104041);
    CHECK(chest.config == 72);

    CHECK_FALSE(ReadOpened("", chest));
    CHECK_FALSE(ReadOpened("\x58\x48", chest));
}

TEST_CASE("ReadAppeared keeps only the chests, with where they are") {
    std::string body = number(1, 5) + object(7, 70211002, 133104041, 72, 725.311f, 202.981f, 227.698f) +
        object(8, 70610001, 0, 0, 1.0f, 2.0f, 3.0f);
    std::vector<Chest> chests;
    ReadAppeared(body, chests);
    REQUIRE(chests.size() == 1);
    CHECK(chests[0].gadget == 70211002);
    CHECK(chests[0].group == 133104041);
    CHECK(chests[0].config == 72);
    CHECK(chests[0].x == 725.311f);
    CHECK(chests[0].z == 227.698f);

    double x, y;
    ToMap(chests[0], x, y);
    CHECK(x == -double(227.698f) - 750.0);
    CHECK(y == -double(725.311f) + 2321.0);
}

TEST_CASE("Match takes the chest's marker only when nothing else is close") {
    const uint32_t COMMON = 70211002, EXQUISITE = 70211012;
    Chest chest = chestAt(COMMON, 100, 100);
    std::vector<Marker> markers = { { 1, 17, 103, 100 } };
    CHECK(Match(chest, {}, markers) == 1);

    // another common chest marker that close
    std::vector<Marker> two = markers;
    two.push_back({ 2, 17, 106, 100 });
    CHECK(Match(chest, {}, two) == 0);
    // an exquisite one is no mix up, a buried one could be any chest
    std::vector<Marker> exquisite = markers;
    exquisite.push_back({ 3, 44, 104, 100 });
    CHECK(Match(chest, {}, exquisite) == 1);
    std::vector<Marker> buried = markers;
    buried.push_back({ 4, 24, 106, 100 });
    CHECK(Match(chest, {}, buried) == 0);

    // another common chest the game sent sits at the marker
    CHECK(Match(chest, { chestAt(COMMON, 104, 101, 2) }, markers) == 0);
    CHECK(Match(chest, { chestAt(EXQUISITE, 104, 101, 2) }, markers) == 1);
    // the chest itself doesn't count
    CHECK(Match(chest, { chest }, markers) == 1);

    CHECK(Match(chest, {}, { { 5, 17, 125, 100 } }) == 0);
    CHECK(Match(chest, {}, { { 6, 44, 103, 100 } }) == 0);

    // the same spot in the Chasm underground or a domain isn't Teyvat
    Chest chasm = chest, domain = chest;
    chasm.group = 166001001;
    domain.group = 220330001;
    CHECK(Match(chasm, {}, markers) == 0);
    CHECK(Match(domain, {}, markers) == 0);
}

TEST_CASE("Sealed and mora chests are chests too") {
    // what HoYoLAB calls a Sealed Chest, a Wooden and a Stone Pile Mora Chest
    std::string body = object(1, 70210063, 133104041, 1, 0, 0, 0) + object(2, 70210105, 133104041, 2, 0, 0, 0) +
        object(3, 70217020, 133104041, 3, 0, 0, 0) + object(4, 70610001, 133104041, 4, 0, 0, 0);
    std::vector<Chest> chests;
    ReadAppeared(body, chests);
    CHECK(chests.size() == 3);

    // a sealed chest fits a sealed marker, a mora chest only its own kind
    Chest sealed = chestAt(70210063, 100, 100), mora = chestAt(70210105, 100, 100);
    CHECK(Match(sealed, {}, { { 1, 75, 103, 100 } }) == 1);
    CHECK(Match(sealed, {}, { { 1, 17, 103, 100 } }) == 0);
    CHECK(Match(mora, {}, { { 2, 129, 103, 100 } }) == 2);
    CHECK(Match(mora, {}, { { 2, 359, 103, 100 } }) == 0);
    CHECK(Match(mora, {}, { { 2, 75, 103, 100 } }) == 0);
}

TEST_CASE("Tracker knows where an opened chest was") {
    Tracker tracker;
    Chest opened;
    bool placed = false;
    CHECK_FALSE(tracker.Feed(APPEARED, object(7, 70211002, 133104041, 72, 725.0f, 202.0f, 227.0f), opened, placed));
    CHECK(tracker.Chests().size() == 1);

    REQUIRE(tracker.Feed(OPENED, "\x58\x48\x60\xa9\x83\xbc\x3f", opened, placed));
    CHECK(placed);
    CHECK(opened.gadget == 70211002);
    CHECK(opened.x == 725.0f);
    CHECK(tracker.Chests().empty());

    // never sent, so no idea where it is
    REQUIRE(tracker.Feed(OPENED, "\x58\x48\x60\xa9\x83\xbc\x3f", opened, placed));
    CHECK_FALSE(placed);
    CHECK_FALSE(tracker.Feed(1, "", opened, placed));
}

// one move of the player in the game's MOVED message: 8, 6, 9 hold the 15s
static std::string moved(uint32_t entity, float x, float y, float z) {
    std::string move = bytes(15, number(1, entity) + bytes(2, bytes(1, real(1, x) + real(2, y) + real(3, z))));
    return bytes(8, bytes(6, bytes(9, move)));
}

TEST_CASE("ReadPlayer takes where the player's character moved") {
    Chest where;
    REQUIRE(ReadPlayer(moved(2097802, 725.0f, 202.0f, 227.0f), where));
    CHECK(where.x == 725.0f);
    CHECK(where.z == 227.0f);
    // seen live, still the player's
    CHECK(ReadPlayer(moved(2104768, 1.0f, 2.0f, 3.0f), where));
    // an enemy moving isn't the player
    CHECK_FALSE(ReadPlayer(moved(4201255, 1.0f, 2.0f, 3.0f), where));
    CHECK_FALSE(ReadPlayer("", where));
}

TEST_CASE("HintFor tells a chest still there from one probably opened") {
    const uint32_t COMMON = 70211002;
    std::vector<Marker> markers = { { 1, 17, 100, 100 }, { 2, 17, 300, 100 } };
    std::vector<Chest> chests = { chestAt(COMMON, 102, 100) };
    CHECK(HintFor(markers[0], chests, markers) == Hint::StillThere);
    CHECK(HintFor(markers[1], chests, markers) == Hint::ProbablyOpened);
    // a chest near the marker that can't surely be told is neither
    std::vector<Marker> close = { { 1, 17, 100, 100 }, { 3, 17, 104, 100 } };
    CHECK(HintFor(close[0], chests, close) == Hint::None);
    // a buried chest isn't there until it's dug up
    CHECK(HintFor({ 4, 69, 300, 100 }, chests, markers) == Hint::None);
    CHECK(HintFor({ 5, 24, 300, 100 }, chests, markers) == Hint::None);
}

TEST_CASE("Hints says probably opened only after the player was close a while") {
    const uint32_t COMMON = 70211002;
    std::vector<Marker> markers = { { 1, 17, 100, 100 }, { 2, 17, 220, 100 } };
    std::vector<Chest> chests = { chestAt(COMMON, 220, 101) };
    Hints hints;
    Chest player = chestAt(0, 110, 100);
    hints.Moved(player, 1000, chests, markers);
    // the chest the game sent proves its marker from further away
    CHECK(hints.All().at(2) == Hint::StillThere);
    // just got there, the game may still be sending
    CHECK(hints.All().count(1) == 0);
    CHECK_FALSE(hints.Due(1500));
    REQUIRE(hints.Due(7000));
    hints.Moved(player, 7000, chests, markers);
    CHECK(hints.All().at(1) == Hint::ProbablyOpened);

    // a chest showing up there later (a puzzle solved) takes the hint back
    chests.push_back(chestAt(COMMON, 101, 100, 2));
    hints.Moved(player, 8000, chests, markers);
    CHECK(hints.All().at(1) == Hint::StillThere);
}
