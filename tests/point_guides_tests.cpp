#include <windows.h>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sol/sol.hpp>
#include "point_guides.hpp"
#include "item_catalog.hpp"
#include "item_files.hpp"
#include "extensions.hpp"
#include "lua_functions.hpp"

// Guides on real saved answers (tests/data/guides, answers.md says what's in
// them): chest 637 in Mondstadt and its appsample pair 3881.

namespace fs = std::filesystem;

namespace {
    const fs::path DATA = fs::path(__FILE__).parent_path() / "data" / "guides";

    std::string file(const std::string& name) {
        std::ifstream in(DATA / name, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    const PointGuides::Point CHEST{ 637, 2, 17, 3881 };

    // Stands in for the sites, answering with the saved files, and for the
    // clock. Writes down every URL asked.
    struct Sites {
        std::mutex mutex;
        std::vector<std::string> asked;
        std::atomic<bool> down{ false };
        std::atomic<bool> googleDown{ false };
        std::atomic<bool> commentsDown{ false };
        // each request waits while it's set
        std::atomic<bool> hold{ false };
        std::chrono::steady_clock::time_point time = std::chrono::steady_clock::time_point() + std::chrono::hours(1);
        std::mutex timeMutex;
        decltype(PointGuides::fetch) realFetch = PointGuides::fetch;
        decltype(PointGuides::now) realNow = PointGuides::now;

        Sites() {
            PointGuides::stop();
            PointGuides::fetch = [this](const std::string& url, bool, Net::Response& out, Net::Stopper*) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    asked.push_back(url);
                }
                while (hold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (down) return false;
                out.status = 200;
                if (url.find("point/info") != std::string::npos) out.body = file(url.find("point_id=637&") != std::string::npos ? "info_637.json" : "info_missing.json");
                else if (url.find("lemonapi") != std::string::npos) {
                    if (commentsDown) return false;
                    out.body = file(url.find("docId=3881&") != std::string::npos ? "comments_3881.json" : "comments_empty.json");
                }
                else if (url.find("translate_a") != std::string::npos) {
                    if (googleDown) return false;
                    out.body = file("google_3881.json");
                }
                else if (url.find("mymemory") != std::string::npos) out.body = file("mymemory_es.json");
                else return false;
                return true;
            };
            PointGuides::now = [this]() {
                std::lock_guard<std::mutex> lock(timeMutex);
                return time;
            };
        }
        ~Sites() {
            PointGuides::stop();
            PointGuides::fetch = realFetch;
            PointGuides::now = realNow;
        }
        void wait(std::chrono::steady_clock::duration by) {
            std::lock_guard<std::mutex> lock(timeMutex);
            time += by;
        }
        int count(const std::string& part) {
            std::lock_guard<std::mutex> lock(mutex);
            int n = 0;
            for (const std::string& url : asked) n += url.find(part) != std::string::npos;
            return n;
        }
    };

    // asks until done() says the guide is as wanted, or 5 s
    bool askUntil(const PointGuides::Point& point, PointGuides::Guide& guide, const std::function<bool(const PointGuides::Guide&)>& done) {
        auto started = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            if (PointGuides::get(point, "en", guide) && done(guide)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    bool translated(const PointGuides::Guide& guide) {
        for (const auto& comment : guide.comments) {
            if (comment.translating) return false;
        }
        return true;
    }
}

TEST_CASE("HoYoLAB's guide reads, and a point it doesn't have is an answer with no text") {
    PointGuides::Guide guide;
    REQUIRE(PointGuides::readInfo(file("info_637.json"), guide));
    CHECK(guide.text == "Hidden inside a bush (destroying it is optional).");
    CHECK(guide.picture == "https://upload-static.hoyoverse.com/wiki-ys-map/2021/08/01/43715711/52bd4be62c366cf8f0e9cf6ec3f155c7_5002326503694988461.png");

    PointGuides::Guide missing;
    CHECK(PointGuides::readInfo(file("info_missing.json"), missing));
    CHECK(missing.text.empty());
    CHECK(missing.picture.empty());

    PointGuides::Guide broken;
    CHECK_FALSE(PointGuides::readInfo("<html>Too many requests</html>", broken));
    CHECK(PointGuides::infoUrl(CHEST, "en").find("map_id=2&point_id=637&app_sn=ys_obc&lang=en-us") != std::string::npos);
}

TEST_CASE("appsample's comments read most liked first, with its summary") {
    PointGuides::Guide guide;
    REQUIRE(PointGuides::readComments(file("comments_3881.json"), "en", guide));
    CHECK(guide.summary == "Chest is completely hidden inside a bush.");
    REQUIRE(guide.comments.size() == 30);
    for (size_t i = 1; i < guide.comments.size(); i++) CHECK(guide.comments[i - 1].likes >= guide.comments[i].likes);
    CHECK(guide.comments[0].likes == 432);
    CHECK(guide.comments[0].author == "I.E.");
    CHECK(guide.comments[0].date == "2021-07-02");
    CHECK(guide.comments.back().author == "L.E.");
    CHECK(guide.comments.back().text == "Joey");

    int pictures = 0, english = 0, translating = 0;
    for (const auto& comment : guide.comments) {
        if (!comment.picture.empty()) {
            pictures++;
            CHECK(comment.picture.rfind("https://game-cdn.appsample.com/gim/", 0) == 0);
        }
        if (comment.language == "en") {
            english++;
            CHECK_FALSE(comment.translating);
        }
        translating += comment.translating;
        CHECK(comment.text == comment.original);
        CHECK(comment.from.empty());
    }
    CHECK(pictures == 13);
    CHECK(english == 7);
    CHECK(translating == 23);

    PointGuides::Guide empty;
    REQUIRE(PointGuides::readComments(file("comments_empty.json"), "en", empty));
    CHECK(empty.comments.empty());
    CHECK(empty.summary.empty());
    CHECK(PointGuides::commentsUrl(CHEST).find("collection=o17&docId=3881&") != std::string::npos);
}

TEST_CASE("the translators' answers read") {
    std::vector<PointGuides::Translated> google;
    REQUIRE(PointGuides::readGoogle(file("google_3881.json"), 23, google));
    CHECK(google[6].text == "Chest hidden in a bush");
    CHECK(google[6].from == "ru");
    CHECK(google[7].text == "chest bush");
    CHECK(google[7].from == "en");
    // an answer for another number of texts isn't this one
    CHECK_FALSE(PointGuides::readGoogle(file("google_3881.json"), 22, google));
    // one text asked
    REQUIRE(PointGuides::readGoogle(file("google_one.json"), 1, google));
    CHECK(google[0].text == "chest in the bush");
    CHECK(google[0].from == "ru");

    std::string text;
    REQUIRE(PointGuides::readMyMemory(file("mymemory_es.json"), text));
    CHECK(text == "No, the attacks are on a date.");
    CHECK(PointGuides::languageName("ru") == "Russian");
    CHECK(PointGuides::languageName("zh-CN") == "Chinese");
    CHECK(PointGuides::myMemoryUrl("a b", "es", "en").find("q=a%20b&langpair=es%7Cen") != std::string::npos);
}

TEST_CASE("a guide comes in, then its comments in the overlay's language, with one Google request") {
    Sites sites;
    PointGuides::Guide guide;
    CHECK_FALSE(PointGuides::get(CHEST, "en", guide));
    REQUIRE(askUntil(CHEST, guide, translated));
    CHECK(guide.text == "Hidden inside a bush (destroying it is optional).");
    CHECK(guide.summary == "Chest is completely hidden inside a bush.");
    CHECK_FALSE(guide.failed);
    REQUIRE(guide.comments.size() == 30);
    CHECK(sites.count("translate_a") == 1);
    CHECK(sites.count("mymemory") == 0);

    int from = 0;
    for (const auto& comment : guide.comments) {
        if (comment.likes == 22) {
            CHECK(comment.text == "Chest hidden in a bush");
            CHECK(comment.from == "Russian");
            CHECK(comment.original != comment.text);
        }
        if (comment.likes == 15) {
            // tagged ru, Google says it's English
            CHECK(comment.text == "chest bush");
            CHECK(comment.from.empty());
        }
        from += !comment.from.empty();
    }
    CHECK(from == 2);
}

TEST_CASE("with Google down, MyMemory goes by appsample's tag, and untagged ones stay as written") {
    Sites sites;
    sites.googleDown = true;
    PointGuides::Guide guide;
    REQUIRE(askUntil(CHEST, guide, translated));
    // the tagged ones not in English: ro, nb, nb, ru, es, pt, es (answers.md)
    CHECK(sites.count("mymemory") == 7);
    CHECK(sites.count("langpair=ro%7Cen") == 1);
    for (const auto& comment : guide.comments) {
        if (comment.language.empty()) {
            CHECK(comment.text == comment.original);
            CHECK(comment.from.empty());
        }
    }
}

TEST_CASE("a guide is kept 10 minutes, then fetched again while the old one shows") {
    Sites sites;
    PointGuides::Point point{ 637, 2, 17, 0 };
    PointGuides::Guide guide;
    REQUIRE(askUntil(point, guide, [](const auto&) { return true; }));
    CHECK(sites.count("point/info") == 1);
    // no appsample pair, no comments asked
    CHECK(sites.count("lemonapi") == 0);

    sites.wait(std::chrono::minutes(9));
    CHECK(PointGuides::get(point, "en", guide));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(sites.count("point/info") == 1);

    sites.wait(std::chrono::minutes(1));
    CHECK(PointGuides::get(point, "en", guide));
    CHECK(guide.text == "Hidden inside a bush (destroying it is optional).");
    REQUIRE(askUntil(point, guide, [&](const auto&) { return sites.count("point/info") == 2; }));
}

TEST_CASE("a guide that failed is asked again after 10 s, not before") {
    Sites sites;
    sites.down = true;
    PointGuides::Point point{ 637, 2, 17, 0 };
    PointGuides::Guide guide;
    REQUIRE(askUntil(point, guide, [](const auto& got) { return got.failed; }));
    CHECK(sites.count("point/info") == 1);

    sites.down = false;
    sites.wait(std::chrono::seconds(5));
    CHECK(PointGuides::get(point, "en", guide));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(sites.count("point/info") == 1);

    sites.wait(std::chrono::seconds(5));
    REQUIRE(askUntil(point, guide, [](const auto& got) { return !got.failed; }));
    CHECK(sites.count("point/info") == 2);
    CHECK(guide.text == "Hidden inside a bush (destroying it is optional).");
}

TEST_CASE("the last 64 guides asked are kept") {
    Sites sites;
    PointGuides::Guide guide;
    auto point = [](int64_t id) { return PointGuides::Point{ id, 2, 17, 0 }; };
    for (int64_t id = 1; id <= 64; id++) {
        REQUIRE(askUntil(point(id), guide, [](const auto&) { return true; }));
        sites.wait(std::chrono::seconds(1));
    }
    // the first asked again, so the second is the oldest
    CHECK(PointGuides::get(point(1), "en", guide));
    sites.wait(std::chrono::seconds(1));
    REQUIRE(askUntil(point(65), guide, [](const auto&) { return true; }));
    CHECK(sites.count("point/info") == 65);

    CHECK(PointGuides::get(point(1), "en", guide));
    CHECK_FALSE(PointGuides::get(point(2), "en", guide));
    REQUIRE(askUntil(point(2), guide, [](const auto&) { return true; }));
    CHECK(sites.count("point/info") == 66);
}

TEST_CASE("a refresh keeps the comments when only they didn't come, and comments that came again keep their translation") {
    Sites sites;
    PointGuides::Guide guide;
    REQUIRE(askUntil(CHEST, guide, [](const auto& each) { return each.comments.size() == 30 && translated(each); }));
    CHECK(sites.count("translate_a") == 1);

    sites.commentsDown = true;
    sites.wait(std::chrono::minutes(10));
    REQUIRE(askUntil(CHEST, guide, [&](const auto&) { return sites.count("point/info") == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(PointGuides::get(CHEST, "en", guide));
    CHECK(guide.comments.size() == 30);
    CHECK(guide.summary == "Chest is completely hidden inside a bush.");
    CHECK(translated(guide));

    // asked again after 10 s, the translations aren't asked again
    sites.commentsDown = false;
    sites.wait(std::chrono::seconds(10));
    REQUIRE(askUntil(CHEST, guide, [&](const auto&) { return sites.count("lemonapi") == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(PointGuides::get(CHEST, "en", guide));
    CHECK(guide.comments.size() == 30);
    CHECK(translated(guide));
    CHECK(std::count_if(guide.comments.begin(), guide.comments.end(), [](const auto& each) { return each.from == "Russian"; }) == 2);
    CHECK(sites.count("translate_a") == 1);
}

TEST_CASE("guides asked past the 64 kept don't pile up waiting") {
    Sites sites;
    sites.hold = true;
    PointGuides::Guide guide;
    for (int round = 0; round < 5; round++) {
        for (int64_t id = 1; id <= 100; id++) PointGuides::get({ id, 2, 17, 0 }, "en", guide);
        sites.wait(std::chrono::seconds(1));
    }
    CHECK(PointGuides::queued() <= PointGuides::Kept);
    sites.hold = false;
}

TEST_CASE("GetPointGuide is refused for an extension whose sites leave out the guide's") {
    Sites sites;
    std::string raw;
    REQUIRE(ItemFiles::pack(fs::path(__FILE__).parent_path() / "data" / "items" / "folder", raw));
    auto catalog = ItemCatalog::load(raw);
    REQUIRE(catalog);
    ItemCatalog::publish(catalog);
    LuaFunctions::Items::beginFrame();

    sol::state lua;
    Extensions::Extension ext;
    Extensions::Extension* previous = Extensions::currentExtension;
    ext.name = "guide tests";
    ext.extensionPermissions = Extensions::defaultPermissions;
    ext.extensionPermissions.insert(Extensions::ExtensionPermissionTypes::NetworkAccess);
    ext.listRead = true;
    ext.sitesOnly = true;
    ext.sites = { "hoyolab.com", "lemonapi.com" };
    Extensions::currentExtension = &ext;
    lua.open_libraries(sol::lib::base);
    LuaFunctions::registerLuaFunctions(lua);

    // a point of the test pack with an appsample pair
    auto refused = lua.safe_script("return GetPointGuide(82140)", sol::script_pass_on_error);
    REQUIRE(refused.valid());
    CHECK(refused.get<sol::object>().is<bool>());
    CHECK_FALSE(refused.get<bool>());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(sites.count("") == 0);

    // nil while main.lua's sites aren't read yet
    ext.listRead = false;
    auto loading = lua.safe_script("return GetPointGuide(82140)", sol::script_pass_on_error);
    REQUIRE(loading.valid());
    CHECK(loading.get<sol::object>() == sol::lua_nil);
    ext.listRead = true;

    // with the four sites it's asked, by their hosts too; a point the files
    // don't have is false
    ext.sites = { "hoyolab.com", "lemonapi.com", "googleapis.com", "translated.net" };
    auto domains = lua.safe_script("return GetPointGuide(82140)", sol::script_pass_on_error);
    REQUIRE(domains.valid());
    CHECK(domains.get<sol::object>() == sol::lua_nil);
    ext.sites = PointGuides::sites();
    auto asked = lua.safe_script("return GetPointGuide(82140)", sol::script_pass_on_error);
    REQUIRE(asked.valid());
    CHECK(asked.get<sol::object>() == sol::lua_nil);
    auto unknown = lua.safe_script("return GetPointGuide(1)", sol::script_pass_on_error);
    REQUIRE(unknown.valid());
    CHECK_FALSE(unknown.get<bool>());
    Extensions::currentExtension = previous;
}
