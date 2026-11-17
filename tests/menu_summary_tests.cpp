#include <catch2/catch_test_macros.hpp>
#include "menu_summary.hpp"

using Accounts::Site;
using Accounts::State;
using Kind = Controls::Summary::Kind;

static Accounts::Status account(State state, const std::string& id = "", const std::string& name = "") {
    Accounts::Status status;
    status.state = state;
    status.account = id;
    status.name = name;
    return status;
}

TEST_CASE("an account's line says who's logged in, a warning when the site wants something") {
    CHECK(MenuSummary::Account(Site::HoyolabMap, account(State::LoggedIn, "123456789", "Traveller")).text == "Logged in as Traveller");
    CHECK(MenuSummary::Account(Site::HoyolabMap, account(State::LoggedIn, "123456789")).text == "Logged in, account 123456789");
    CHECK(MenuSummary::Account(Site::Appsample, account(State::LoggedIn, "me@example.com", "ignored")).text == "Logged in as me@example.com");
    CHECK(MenuSummary::Account(Site::Appsample, account(State::LoggedOut)).text == "Not logged in");
    CHECK(MenuSummary::Account(Site::Appsample, account(State::LoggedOut)).kind == Kind::Plain);
    Controls::Summary out = MenuSummary::Account(Site::HoyolabMap, account(State::NeedsLogin, "123456789"));
    CHECK(out.text == "HoYoLAB Map logged you out. Log in again.");
    CHECK(out.kind == Kind::Warning);
    CHECK(MenuSummary::Account(Site::HoyolabMap, account(State::Captcha)).kind == Kind::Warning);
}

TEST_CASE("the Accounts page's line is there only while a site wants something") {
    CHECK(MenuSummary::AccountsPage(account(State::LoggedIn, "1"), account(State::LoggedOut)).text.empty());
    CHECK(MenuSummary::AccountsPage(account(State::WindowOpen), account(State::LoggedIn, "a@b.c")).text.empty());
    Controls::Summary asks = MenuSummary::AccountsPage(account(State::LoggedIn, "1"), account(State::NeedsLogin, "a@b.c"));
    CHECK(asks.text == "appsample logged out, log in again");
    CHECK(asks.kind == Kind::Warning);
    CHECK(MenuSummary::AccountsPage(account(State::Captcha), account(State::NeedsLogin)).text
        == "HoYoLAB Map wants a bot check · appsample logged out, log in again");
}

static ItemFiles::Status items(ItemFiles::State state, ItemFiles::Reason reason = ItemFiles::Reason::None) {
    ItemFiles::Status status;
    status.state = state;
    status.reason = reason;
    status.started = true;
    status.built = "2026-11-14";
    return status;
}

TEST_CASE("the map items' part of the Map line is short, the rest is said inside") {
    using ItemFiles::Reason;
    using ItemFiles::State;
    ItemFiles::Status waiting;
    CHECK(MenuSummary::Items(waiting).line.empty());

    MenuSummary::ItemsText ready = MenuSummary::Items(items(State::Ready));
    CHECK(ready.line == "map items up to date");
    CHECK(ready.detail.empty());
    CHECK_FALSE(ready.problem);

    ItemFiles::Status downloading = items(State::Downloading);
    downloading.percent = 40;
    CHECK(MenuSummary::Items(downloading).line == "map items downloading 40%");

    MenuSummary::ItemsText old = MenuSummary::Items(items(State::Old, Reason::NoNetwork));
    CHECK(old.line == "map items couldn't update");
    CHECK(old.detail == "Using the list from 2026-11-14, trying again every minute.");
    CHECK(old.problem);
    CHECK(MenuSummary::Items(items(State::None, Reason::NoNetwork)).line == "no map items yet");

    MenuSummary::ItemsText broken = MenuSummary::Items(items(State::Old, Reason::BrokenPack));
    CHECK(broken.tryAgain);
    CHECK(broken.detail == "Using the list from 2026-11-14.");
    CHECK(MenuSummary::Items(items(State::None, Reason::CantSave)).tryAgain);

    MenuSummary::ItemsText off = MenuSummary::Items(items(State::None, Reason::DownloadOff));
    CHECK(off.line == "no map items, the download is off");
    CHECK_FALSE(off.problem);
    MenuSummary::ItemsText offOld = MenuSummary::Items(items(State::Old, Reason::DownloadOff));
    CHECK(offOld.line == "map items from 2026-11-14, the download is off");
    CHECK_FALSE(offOld.problem);
}

TEST_CASE("the Map line puts the map first and stands out when either part has a problem") {
    using Data = MenuSummary::MapState::Data;
    MenuSummary::MapState map;
    map.visible = true;
    map.mapName = "Teyvat";
    MenuSummary::ItemsText fine = MenuSummary::Items(items(ItemFiles::State::Ready));
    Controls::Summary line = MenuSummary::MapSection(MenuSummary::Map(map), fine);
    CHECK(line.text == "On the map: Teyvat · map items up to date");
    CHECK(line.kind == Kind::Plain);

    map.visible = false;
    CHECK(MenuSummary::Map(map).line == "Waiting for the in-game map (M)");
    map.mapOpen = true;
    CHECK(MenuSummary::Map(map).line == "Map is open, looking for where it is");

    map.data = Data::Downloading;
    map.stage = "Checking map data";
    CHECK(MenuSummary::Map(map).line == "Checking map data");
    map.stage = "Downloading map images";
    map.percent = 40;
    CHECK(MenuSummary::Map(map).line == "Downloading map images 40%");

    map.data = Data::NoData;
    MenuSummary::MapText none = MenuSummary::Map(map);
    CHECK(none.detail == "Check your internet connection and restart.");
    CHECK(MenuSummary::MapSection(none, fine).kind == Kind::Strong);
    CHECK(MenuSummary::MapSection(MenuSummary::Map({}), MenuSummary::Items(items(ItemFiles::State::Old, ItemFiles::Reason::NoNetwork))).kind == Kind::Strong);
}

TEST_CASE("Usage's line has CPU and RAM, nothing before the first sample") {
    CHECK(MenuSummary::Usage(0.57f, 212.4f) == "CPU 0.6% · 212 MB");
    CHECK(MenuSummary::Usage(0, 0).empty());
}
