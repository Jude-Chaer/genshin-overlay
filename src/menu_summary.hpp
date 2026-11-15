#pragma once
#include <string>
#include "accounts.hpp"
#include "controls.hpp"
#include "item_files.hpp"

// The summary lines of the overlay's own pages and sections in the menu, and
// what's left to say inside a section once its line says the state.

namespace MenuSummary {
    // the parts of a line, " · " between them
    extern std::string Join(const std::string& a, const std::string& b);

    // "Logged in as Traveller", warning while the site wants something
    extern Controls::Summary Account(Accounts::Site site, const Accounts::Status& status);
    // for the Accounts page, only while a site wants something: "appsample logged out, log in again"
    extern Controls::Summary AccountsPage(const Accounts::Status& hoyolab, const Accounts::Status& appsample);

    struct ItemsText {
        // "map items up to date", empty before the first check says
        std::string line;
        // the rest, under the summary: "Using the list from 2026-11-14, trying again every minute."
        std::string detail;
        bool problem = false;
        // a Try again button under the detail
        bool tryAgain = false;
    };
    extern ItemsText Items(const ItemFiles::Status& items);

    // what the map follower is doing, read by the caller
    struct MapState {
        enum class Data { NoData, Downloading, Loading, Failed, Ready };
        Data data = Data::Ready;
        // the downloader's stage, e.g. "Checking map data", and its percent
        std::string stage;
        int percent = 0;
        bool visible = false;
        bool mapOpen = false;
        std::string mapName;
    };
    struct MapText {
        std::string line;
        std::string detail;
        bool problem = false;
    };
    extern MapText Map(const MapState& map);

    // the Map section's line: the map's part, then the items'
    extern Controls::Summary MapSection(const MapText& map, const ItemsText& items);

    // "CPU 0.6% · 212 MB"
    extern std::string Usage(float cpu, float ramMb);
}
