#include "menu_summary.hpp"
#include <cstdio>

namespace MenuSummary {
    std::string Join(const std::string& a, const std::string& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        return a + " · " + b;
    }

    static bool wantsSomething(const Accounts::Status& status) {
        return status.state == Accounts::State::NeedsLogin || status.state == Accounts::State::Captcha;
    }

    Controls::Summary Account(Accounts::Site site, const Accounts::Status& status) {
        std::string label = Accounts::SiteLabel(site);
        Controls::Summary summary;
        if (wantsSomething(status)) summary.kind = Controls::Summary::Kind::Warning;
        switch (status.state) {
        case Accounts::State::LoggedOut: summary.text = "Not logged in"; break;
        case Accounts::State::WindowOpen: summary.text = "Log in in the " + label + " window"; break;
        case Accounts::State::LoggedIn:
            if (site == Accounts::Site::Appsample) summary.text = "Logged in as " + status.account;
            else summary.text = status.name.empty() ? "Logged in, account " + status.account : "Logged in as " + status.name;
            break;
        case Accounts::State::NeedsLogin: summary.text = label + " logged you out. Log in again."; break;
        case Accounts::State::Captcha: summary.text = label + " wants to check you're not a bot."; break;
        }
        return summary;
    }

    static std::string wanted(Accounts::Site site, const Accounts::Status& status) {
        std::string label = Accounts::SiteLabel(site);
        if (status.state == Accounts::State::NeedsLogin) return label + " logged out, log in again";
        if (status.state == Accounts::State::Captcha) return label + " wants a bot check";
        return "";
    }

    Controls::Summary AccountsPage(const Accounts::Status& hoyolab, const Accounts::Status& appsample) {
        Controls::Summary summary;
        summary.text = Join(wanted(Accounts::Site::HoyolabMap, hoyolab), wanted(Accounts::Site::Appsample, appsample));
        summary.kind = Controls::Summary::Kind::Warning;
        return summary;
    }

    ItemsText Items(const ItemFiles::Status& items) {
        static_assert(ItemFiles::RETRY == std::chrono::seconds(60), "the detail says every minute");
        using ItemFiles::Reason;
        using ItemFiles::State;
        ItemsText out;
        if (!items.started) return out;
        bool old = items.state == State::Old;
        std::string usingOld = old ? "Using the list from " + items.built : "";
        out.problem = true;
        if (items.state == State::Downloading) {
            out.line = "map items downloading " + std::to_string(items.percent) + "%";
            out.problem = false;
        }
        else if (items.reason == Reason::NoNetwork) {
            out.line = old ? "map items couldn't update" : "no map items yet";
            out.detail = old ? usingOld + ", trying again every minute." : "Trying again every minute (no internet?).";
        }
        else if (items.reason == Reason::NewerFormat) {
            out.line = "update the overlay to get the new map items";
            if (old) out.detail = usingOld + ".";
        }
        else if (items.reason == Reason::BrokenPack) {
            out.line = "the map items download was broken";
            if (old) out.detail = usingOld + ".";
            out.tryAgain = true;
        }
        else if (items.reason == Reason::CantSave) {
            out.line = "map items can't be saved";
            out.detail = "Check the overlay folder isn't read-only.";
            out.tryAgain = true;
        }
        else if (items.reason == Reason::DownloadOff) {
            out.line = old ? "map items from " + items.built + ", the download is off" : "no map items, the download is off";
            out.problem = false;
        }
        else {
            out.line = "map items up to date";
            out.problem = false;
        }
        return out;
    }

    MapText Map(const MapState& map) {
        MapText out;
        switch (map.data) {
        case MapState::Data::NoData:
            out.line = "Couldn't get the map data";
            out.detail = "Check your internet connection and restart.";
            out.problem = true;
            return out;
        case MapState::Data::Downloading:
            out.line = map.stage.empty() ? "Downloading map data" : map.stage;
            if (map.percent > 0) out.line += " " + std::to_string(map.percent) + "%";
            return out;
        case MapState::Data::Loading:
            out.line = "Loading map data";
            return out;
        case MapState::Data::Failed:
            out.line = "Map data failed to load";
            out.detail = "Check the console.";
            out.problem = true;
            return out;
        case MapState::Data::Ready:
            break;
        }
        if (map.visible) out.line = "On the map: " + map.mapName;
        else if (map.mapOpen) out.line = "Map is open, looking for where it is";
        else out.line = "Waiting for the in-game map (M)";
        return out;
    }

    Controls::Summary MapSection(const MapText& map, const ItemsText& items) {
        Controls::Summary summary;
        summary.text = Join(map.line, items.line);
        if (map.problem || items.problem) summary.kind = Controls::Summary::Kind::Strong;
        return summary;
    }

    std::string Usage(float cpu, float ramMb) {
        // before the first sample
        if (ramMb <= 0) return "";
        char text[64];
        snprintf(text, sizeof(text), "CPU %.1f%% · %.0f MB", cpu, ramMb);
        return text;
    }
}
