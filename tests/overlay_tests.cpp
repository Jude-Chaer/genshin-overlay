#include <catch2/catch_test_macros.hpp>
#include "overlay.hpp"
#include "settings.hpp"

static Screens::Up screens(bool map, bool menu) {
    Screens::Up up;
    up.set(Screens::Screen::Map, map);
    up.set(Screens::Screen::Menu, menu);
    return up;
}

TEST_CASE("hide on map close opens and closes the menu with the map") {
    Settings::hideOnMapClose = true;
    Overlay::SetMenuOpen(false);
    Overlay::MenuFollowsMap(true);
    CHECK(Overlay::MenuOpen());
    Overlay::MenuFollowsMap(false);
    CHECK_FALSE(Overlay::MenuOpen());
    Settings::hideOnMapClose = false;
}

TEST_CASE("without hide on map close the map leaves the menu alone") {
    Settings::hideOnMapClose = false;
    Overlay::SetMenuOpen(true);
    Overlay::MenuFollowsMap(false);
    CHECK(Overlay::MenuOpen());
    Overlay::SetMenuOpen(false);
    Overlay::MenuFollowsMap(true);
    CHECK_FALSE(Overlay::MenuOpen());
}

TEST_CASE("a claimed box takes the mouse only while the map or the menu is open") {
    Overlay::MouseClaims claims;
    ImVec2 inside(150, 150), outside(50, 50);

    claims.claim(100, 100, 200, 200);
    CHECK(claims.takesMouse(inside, screens(true, false), false));
    claims.claim(100, 100, 200, 200);
    CHECK_FALSE(claims.takesMouse(outside, screens(true, false), false));
    claims.claim(100, 100, 200, 200);
    CHECK(claims.takesMouse(inside, screens(false, true), false));
    // map and menu closed: the cursor is in the game, the box doesn't count
    claims.claim(100, 100, 200, 200);
    CHECK_FALSE(claims.takesMouse(inside, screens(false, false), false));
}

TEST_CASE("a box counts for one frame, it's claimed again every frame") {
    Overlay::MouseClaims claims;
    claims.claim(100, 100, 200, 200);
    CHECK(claims.takesMouse(ImVec2(150, 150), screens(true, false), false));
    CHECK_FALSE(claims.takesMouse(ImVec2(150, 150), screens(true, false), false));
}

TEST_CASE("a box ends at its right and bottom edge") {
    Overlay::MouseClaims claims;
    claims.claim(100, 100, 200, 200);
    CHECK(claims.takesMouse(ImVec2(100, 100), screens(true, false), false));
    claims.claim(100, 100, 200, 200);
    CHECK_FALSE(claims.takesMouse(ImVec2(200, 150), screens(true, false), false));
    claims.claim(100, 100, 200, 200);
    CHECK_FALSE(claims.takesMouse(ImVec2(150, 200), screens(true, false), false));
}

TEST_CASE("the menu takes the mouse only while it's open") {
    Overlay::MouseClaims claims;
    CHECK(claims.takesMouse(ImVec2(10, 10), screens(false, true), true));
    // ImGui says the cursor is over one of its windows, but with the menu
    // closed that's the floor picker, and it claims its own box
    CHECK_FALSE(claims.takesMouse(ImVec2(10, 10), screens(true, false), true));
}

TEST_CASE("the boxes that took the mouse are kept with their owner, for showClickBoxes") {
    Overlay::MouseClaims claims;
    claims.claim(100, 100, 200, 200, "markers");
    claims.claim(0, 0, 10, 10);
    claims.takesMouse(ImVec2(150, 150), screens(true, false), false);
    REQUIRE(claims.counted().size() == 2);
    CHECK(claims.counted()[0].owner == "markers");
    CHECK(claims.counted()[1].owner.empty());
    // map and menu closed: none of them counts
    claims.claim(100, 100, 200, 200, "markers");
    claims.takesMouse(ImVec2(150, 150), screens(false, false), false);
    CHECK(claims.counted().empty());
}
