#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/game_puzzles.hpp"

using namespace GamePuzzles;
using GameChests::Chest;
using GameChests::Marker;

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

static constexpr uint32_t GROUP = 133315090;

// one object in the game's APPEARED message, at HoYoLAB's (mapX, mapY)
static std::string object(uint32_t entity, uint32_t gadget, uint32_t config, uint32_t state, double mapX, double mapY) {
    std::string where = bytes(1, real(1, float(2321.0 - mapY)) + real(2, 200.0f) + real(3, float(-750.0 - mapX)));
    std::string kind = number(1, gadget) + number(2, GROUP) + number(3, config);
    if (state) kind += number(6, state);
    return bytes(8, number(2, entity) + bytes(4, where) + bytes(13, kind));
}
static std::string changed(uint32_t entity, uint32_t state) {
    return number(3, entity) + (state ? number(7, state) : "");
}
static std::string opened(uint32_t config) { return number(11, config) + number(12, GROUP); }

// feeds one message and gives what the moment before it finished
static std::vector<Done> feed(Tracker& tracker, uint16_t id, const std::string& body, uint64_t ms) {
    std::vector<Done> done;
    tracker.Feed(id, body, ms, done);
    return done;
}
// a later moment, so what the last one finished comes out
static std::vector<Done> after(Tracker& tracker, uint64_t ms) { return feed(tracker, 0, "", ms); }

TEST_CASE("A torch puzzle lit while listening is done, at its torches") {
    Tracker tracker;
    feed(tracker, GameChests::APPEARED, object(1, 70310011, 1, 0, 100, 100) + object(2, 70310011, 2, 0, 104, 100)
        + object(3, 70211102, 3, 101, 102, 104), 1);
    CHECK(after(tracker, 2).empty());
    feed(tracker, CHANGED, changed(1, 201), 10);
    CHECK(after(tracker, 11).empty());
    feed(tracker, CHANGED, changed(2, 201), 20);
    // the chest is still locked
    CHECK(after(tracker, 21).empty());
    feed(tracker, CHANGED, changed(3, 0), 30);
    std::vector<Done> done = after(tracker, 31);
    REQUIRE(done.size() == 1);
    CHECK(done[0].label == 71);
    CHECK(done[0].parts.size() == 2);
    CHECK(Match(done[0], { { 7, 71, 103, 101 }, { 8, 71, 160, 100 } }) == 7);
    // nothing twice
    feed(tracker, GameChests::OPENED, opened(3), 40);
    CHECK(after(tracker, 41).empty());
    // coming back after a relog it's only done already, for the hint
    feed(tracker, GameChests::APPEARED, object(4, 70310011, 1, 201, 100, 100) + object(5, 70310011, 2, 201, 104, 100), 50);
    done = after(tracker, 51);
    REQUIRE(done.size() == 1);
    CHECK(done[0].already);
}

TEST_CASE("Fire basins that show up one by one are done only after the last") {
    // Natlan basins from recorded traffic: lighting one brings the next
    Tracker tracker;
    feed(tracker, GameChests::APPEARED, object(1, 70310001, 1, 0, 100, 100), 1);
    feed(tracker, CHANGED, changed(1, 201), 10);
    feed(tracker, GameChests::APPEARED, object(2, 70310001, 2, 0, 104, 100), 10);
    CHECK(after(tracker, 11).empty());
    feed(tracker, CHANGED, changed(2, 201), 20);
    feed(tracker, GameChests::APPEARED, object(3, 70211121, 3, 0, 102, 104), 20);
    std::vector<Done> done = after(tracker, 21);
    REQUIRE(done.size() == 1);
    CHECK(done[0].label == 71);
    CHECK(done[0].parts.size() == 2);
}

TEST_CASE("A puzzle that comes in already done is left alone") {
    Tracker tracker;
    feed(tracker, GameChests::APPEARED, object(1, 70310011, 1, 201, 100, 100) + object(2, 70310011, 2, 201, 104, 100)
        + object(3, 70211102, 3, 0, 102, 104), 1);
    // it says so once, for a hint
    std::vector<Done> done = after(tracker, 2);
    REQUIRE(done.size() == 1);
    CHECK(done[0].label == 71);
    CHECK(done[0].already);
    // opening its chest now doesn't make it new
    feed(tracker, GameChests::OPENED, opened(3), 10);
    CHECK(after(tracker, 11).empty());
    // a won trial seen again the same way
    feed(tracker, GameChests::APPEARED, object(4, 70350083, 4, 202, 300, 300), 20);
    done = after(tracker, 21);
    REQUIRE(done.size() == 1);
    CHECK(done[0].already);
    feed(tracker, CHANGED, changed(4, 202), 30);
    CHECK(after(tracker, 31).empty());
}

TEST_CASE("A monument puzzle reset in the same moment isn't done") {
    Tracker tracker;
    feed(tracker, GameChests::APPEARED, object(1, 70900050, 1, 201, 100, 100) + object(2, 70900050, 2, 0, 110, 100), 1);
    // the last one lights and everything resets at once: the wrong order
    feed(tracker, CHANGED, changed(2, 201), 10);
    feed(tracker, CHANGED, changed(1, 0), 10);
    feed(tracker, CHANGED, changed(2, 0), 10);
    CHECK(after(tracker, 11).empty());
    feed(tracker, CHANGED, changed(1, 201), 20);
    feed(tracker, CHANGED, changed(2, 201), 30);
    // each monument can have its own marker
    std::vector<Done> done = after(tracker, 31);
    REQUIRE(done.size() == 2);
    CHECK(done[0].label == 70);
    CHECK(done[1].label == 70);
}

TEST_CASE("A trial is done when won or its chest opened, not when the chest shows up") {
    Tracker tracker;
    feed(tracker, GameChests::APPEARED, object(1, 70350083, 1, 0, 100, 100), 1);
    feed(tracker, CHANGED, changed(1, 201), 10);
    // the chest comes during the trial and goes again if the time runs out
    feed(tracker, GameChests::APPEARED, object(2, 70211121, 2, 0, 400, 100), 20);
    CHECK(after(tracker, 21).empty());
    feed(tracker, GameChests::OPENED, opened(2), 30);
    std::vector<Done> done = after(tracker, 31);
    REQUIRE(done.size() == 1);
    CHECK(done[0].label == 64);
    CHECK_FALSE(done[0].loose);
    // its marker is at the start, not at the chest
    CHECK(Match(done[0], { { 7, 64, 400, 103 }, { 8, 64, 102, 100 } }) == 8);
}

TEST_CASE("Seeds are done one by one, a Seelie only when seen flying home") {
    Tracker tracker;
    feed(tracker, GameChests::APPEARED, object(1, 70320004, 1, 0, 100, 100) + object(2, 70320004, 2, 0, 200, 100)
        + object(3, 70710006, 3, 0, 300, 300), 1);
    feed(tracker, CHANGED, changed(2, 201), 10);
    std::vector<Done> done = after(tracker, 11);
    REQUIRE(done.size() == 1);
    CHECK(done[0].label == 407);
    CHECK(done[0].parts[0].config == 2);
    // a court at home with no Seelie seen: the marker is where the Seelie started, not known
    feed(tracker, CHANGED, changed(3, 203), 20);
    CHECK(after(tracker, 21).empty());
}

TEST_CASE("A puzzle marker has to be the one clearly closest") {
    Done done;
    done.label = 71;
    Chest part;
    part.x = float(2321.0 - 100);
    part.z = float(-750.0 - 100);
    done.parts = { part };
    CHECK(Match(done, { { 7, 71, 103, 100 } }) == 7);
    CHECK(Match(done, { { 7, 71, 103, 100 }, { 8, 71, 95, 100 } }) == 0);
    CHECK(Match(done, { { 7, 71, 113, 100 } }) == 0);
    // another kind's marker doesn't count
    CHECK(Match(done, { { 7, 64, 101, 100 } }) == 0);
}
