#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/game_oculi.hpp"

using namespace GameOculi;
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

// one object in the game's APPEARED message
static std::string object(uint32_t entity, uint32_t gadget, uint32_t group, uint32_t config, float x, float y, float z) {
    std::string where = bytes(1, real(1, x) + real(2, y) + real(3, z));
    return bytes(8, number(2, entity) + bytes(4, where) + bytes(13, number(1, gadget) + number(2, group) + number(3, config)));
}

static std::string gotItem(uint32_t item, uint32_t from) {
    return number(11, from) + bytes(13, number(1, item) + number(2, 1));
}

static std::string left(uint32_t entity, uint32_t reason) { return number(5, entity) + number(11, reason); }

// an oculus the game has at x, z sits at HoYoLAB's (mapX, mapY)
static Chest oculusAt(uint32_t entity, double mapX, double mapY) {
    Chest oculus;
    oculus.entity = entity;
    oculus.group = 133301245;
    oculus.config = entity;
    oculus.gadget = 70590063;
    oculus.x = float(2321.0 - mapY);
    oculus.z = float(-750.0 - mapX);
    return oculus;
}

TEST_CASE("LabelOf knows the oculus items") {
    CHECK(LabelOf(107017) == 403);
    CHECK(LabelOf(107023) == 508);
    CHECK(LabelOf(107026) == 554);
    CHECK(LabelOf(101218) == 0);
}

TEST_CASE("Tracker puts the oculus item and the object that left together") {
    // a Dendroculus from recorded traffic
    std::string appeared = object(8394556, 70590063, 133301245, 245010, -406.232f, 208.835f, 3567.559f) +
        object(8394349, 70540042, 133301235, 235016, -416.362f, 204.406f, 3555.029f);
    Tracker tracker;
    Picked picked;
    CHECK_FALSE(tracker.Feed(GameChests::APPEARED, appeared, 1000, picked));
    CHECK(tracker.Objects().size() == 2);

    CHECK_FALSE(tracker.Feed(GOT_ITEM, gotItem(107017, 11), 2000, picked));
    REQUIRE(tracker.Feed(LEFT, left(8394556, 2), 2000, picked));
    CHECK(picked.label == 403);
    CHECK(picked.object.group == 133301245);
    CHECK(picked.object.config == 245010);
    CHECK(tracker.Objects().size() == 1);

    SECTION("a plant picked up after it isn't an oculus") {
        CHECK_FALSE(tracker.Feed(GOT_ITEM, gotItem(101219, 11), 3000, picked));
        CHECK_FALSE(tracker.Feed(LEFT, left(8394349, 2), 3000, picked));
    }
    SECTION("an object only going out of reach isn't picked up") {
        CHECK_FALSE(tracker.Feed(GOT_ITEM, gotItem(107017, 11), 3000, picked));
        CHECK_FALSE(tracker.Feed(LEFT, left(8394349, 1), 3000, picked));
    }
    SECTION("the item and the object leaving have to come together") {
        CHECK_FALSE(tracker.Feed(GOT_ITEM, gotItem(107017, 11), 3000, picked));
        CHECK_FALSE(tracker.Feed(LEFT, left(8394349, 2), 9000, picked));
    }
    SECTION("an oculus that wasn't picked up in the world doesn't count") {
        CHECK_FALSE(tracker.Feed(GOT_ITEM, gotItem(107017, 37), 3000, picked));
        CHECK_FALSE(tracker.Feed(LEFT, left(8394349, 2), 3000, picked));
    }
}

TEST_CASE("Match takes the oculus marker only when nothing else is close") {
    const int DENDRO = 403, HYDRO = 508;
    Chest oculus = oculusAt(1, 100, 100);
    std::vector<Marker> markers = { { 7, DENDRO, 105, 100 } };
    CHECK(Match(oculus, DENDRO, {}, markers) == 7);

    // another kind of oculus doesn't count
    CHECK(Match(oculus, HYDRO, {}, markers) == 0);
    markers.push_back({ 8, HYDRO, 101, 100 });
    CHECK(Match(oculus, DENDRO, {}, markers) == 7);

    // another dendroculus marker that close, it can't tell which
    markers.push_back({ 9, DENDRO, 100, 108 });
    CHECK(Match(oculus, DENDRO, {}, markers) == 0);
    markers.pop_back();

    // another dendroculus still there next to the marker
    CHECK(Match(oculus, DENDRO, { oculusAt(2, 106, 101) }, markers) == 0);
    // the one picked up is in the list too, it doesn't count
    CHECK(Match(oculus, DENDRO, { oculus }, markers) == 7);

    // too far
    CHECK(Match(oculusAt(1, 130, 100), DENDRO, {}, markers) == 0);
    // not on Teyvat
    Chest chasm = oculus;
    chasm.group = 166001001;
    CHECK(Match(chasm, DENDRO, {}, markers) == 0);
}
