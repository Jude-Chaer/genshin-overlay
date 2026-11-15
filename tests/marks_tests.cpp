#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <nlohmann/json.hpp>
#include "accounts.hpp"
#include "asks.hpp"
#include "credentials.hpp"
#include "files.hpp"
#include "item_catalog.hpp"
#include "item_files.hpp"
#include "lua_functions.hpp"
#include "marks.hpp"
#include "settings.hpp"
#include "test_extension.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using Accounts::Site;
using Permission = Extensions::ExtensionPermissionTypes;

namespace {
    const fs::path FOLDER = fs::path(__FILE__).parent_path() / "data" / "items" / "folder";
    const fs::path WORK = fs::temp_directory_path() / "genshin-overlay-tests" / "marks";

    // HoYoLAB Map and appsample as they keep the marks, answering like them
    struct Sites {
        std::mutex mutex;
        std::map<int, std::set<int64_t>> hoyolab;
        std::set<int64_t> appsample;
        bool appsampleDocument = true;
        // the retcode HoYoLAB gives every request, 0 for none
        int retcode = 0;
        std::vector<std::string> requests;
        std::vector<std::string> bodies;
        std::vector<std::string> headers;
        // while set, every request waits
        std::atomic<bool> hold = false;
        std::atomic<int> held = 0;
        // appsample turns the ID token down this many times
        int refuse = 0;

        static bool answer(Net::Response& out, int status, const json& body) {
            out.status = status;
            out.body = body.dump();
            return true;
        }

        bool operator()(const std::string& method, const std::string& url, const std::string& head, const std::string& body, Net::Response& out) {
            if (hold) held++;
            while (hold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(method + " " + url);
            bodies.push_back(body);
            headers.push_back(head);
            if (url.find("hoyolab.com") != std::string::npos && retcode)
                return answer(out, 200, { { "retcode", retcode }, { "message", "no" }, { "data", nullptr } });
            if (url.find("getUserFullInfo") != std::string::npos)
                return answer(out, 200, { { "retcode", 0 }, { "data", { { "user_info", { { "nickname", "Paimonfan" } } } } } });
            if (url.find("mark_map_point_list") != std::string::npos) {
                int map = std::stoi(url.substr(url.find("map_id=") + 7));
                json list = json::array();
                for (int64_t id : hoyolab[map]) list.push_back({ { "point_id", id }, { "map_id", map } });
                return answer(out, 200, { { "retcode", 0 }, { "message", "OK" }, { "data", { { "list", list } } } });
            }
            if (url.find("batch_mark_map_point") != std::string::npos) {
                json sent = json::parse(body);
                for (const json& point : sent["list"]) {
                    if (point["is_delete"].get<bool>()) hoyolab[sent["map_id"].get<int>()].erase(point["point_id"].get<int64_t>());
                    else hoyolab[sent["map_id"].get<int>()].insert(point["point_id"].get<int64_t>());
                }
                return answer(out, 200, { { "retcode", 0 }, { "message", "OK" }, { "data", json::object() } });
            }
            if (url.find("googleapis.com") != std::string::npos && refuse > 0) {
                refuse--;
                return answer(out, 401, { { "error", { { "code", 401 }, { "status", "UNAUTHENTICATED" } } } });
            }
            if (url.find(":commit") != std::string::npos) {
                json transform = json::parse(body)["writes"][0]["updateTransforms"][0];
                bool remove = transform.contains("removeAllFromArray");
                for (const json& value : transform[remove ? "removeAllFromArray" : "appendMissingElements"]["values"]) {
                    int64_t id = std::stoll(value.begin().value().get<std::string>());
                    if (remove) appsample.erase(id);
                    else appsample.insert(id);
                }
                appsampleDocument = true;
                return answer(out, 200, { { "writeResults", json::array({ json::object() }) } });
            }
            if (url.find("firestore.googleapis.com") != std::string::npos) {
                if (!appsampleDocument) return answer(out, 404, { { "error", { { "code", 404 }, { "status", "NOT_FOUND" } } } });
                json values = json::array();
                for (int64_t id : appsample) values.push_back({ { "stringValue", std::to_string(id) } });
                return answer(out, 200, { { "name", "users/uid1" }, { "fields", { { "markerIds", { { "arrayValue", { { "values", values } } } } } } } });
            }
            return answer(out, 404, json::object());
        }

        int count(const std::string& part) {
            std::lock_guard<std::mutex> lock(mutex);
            return (int)std::count_if(requests.begin(), requests.end(), [&](const std::string& r) { return r.find(part) != std::string::npos; });
        }
    };

    // the test pack with a level on two pairs, as the item files would give
    // it: 17630 at 3, 17629 at 5
    std::shared_ptr<ItemCatalog::Catalog> testCatalog() {
        fs::path folder = WORK / "items";
        fs::remove_all(folder);
        fs::create_directories(folder);
        fs::copy(FOLDER, folder, fs::copy_options::recursive);
        json points = json::parse(Files::readText(folder / "positions" / "231.json"));
        for (json& point : points) {
            if (point["id"] == 17630) point["appsample_level"] = 3;
            if (point["id"] == 17629) point["appsample_level"] = 5;
        }
        std::ofstream(folder / "positions" / "231.json", std::ios::binary) << points.dump();
        std::string raw;
        REQUIRE(ItemFiles::pack(folder, raw));
        auto catalog = ItemCatalog::load(raw);
        REQUIRE(catalog);
        return catalog;
    }

    // the worker on the test sites, with the item files, logged in as
    // "hoyo1" and "uid1" unless a test logs out
    struct Running {
        Sites sites;
        std::map<Site, std::string> accounts = { { Site::HoyolabMap, "hoyo1" }, { Site::Appsample, "uid1" } };
        std::mutex mutex;
        fs::path backups = WORK / "backups";

        explicit Running(bool realLogins = false) {
            fs::remove_all(backups);
            ItemCatalog::publish(testCatalog());
            Marks::LoginFor login = [this](Site site) -> std::optional<Marks::Login> {
                std::lock_guard<std::mutex> lock(mutex);
                std::string account = accounts[site];
                if (account.empty()) return std::nullopt;
                return Marks::Login{ site == Site::HoyolabMap ? "Cookie: ltuid_v2=" + account + "\r\n" : "Authorization: Bearer token\r\n", account, "" };
            };
            Marks::SetForTests(realLogins ? nullptr : login, [this](auto&... args) { return sites(args...); }, backups);
            Marks::Init();
        }
        ~Running() {
            Marks::Shutdown();
            Marks::SetForTests(nullptr, nullptr);
            ItemCatalog::publish(nullptr);
        }
    };

    std::optional<Marks::Result> waitFor(int id, const void* owner = nullptr) {
        auto started = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            if (auto result = Marks::Take(id, owner)) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::nullopt;
    }

    Marks::Request change(std::vector<Site> sites, std::vector<int64_t> add, std::vector<int64_t> remove) {
        Marks::Request request;
        request.sites = sites;
        request.action = Marks::Action::Change;
        request.add = add;
        request.remove = remove;
        return request;
    }

    std::vector<int64_t> sorted(std::vector<int64_t> ids) {
        std::sort(ids.begin(), ids.end());
        return ids;
    }
}

TEST_CASE("a fetch reads every map on HoYoLAB Map and appsample's pairs") {
    Running run;
    run.sites.hoyolab[2] = { 17631, 82140 };
    run.sites.hoyolab[9] = { 33158 };
    // 999999 has no HoYoLAB pair, it's left out
    run.sites.appsample = { 20503, 999999 };
    Marks::Request request;
    request.sites = { Site::HoyolabMap, Site::Appsample };
    auto result = waitFor(Marks::Start(request));
    REQUIRE(result);
    REQUIRE(result->sites.size() == 2);
    CHECK(result->sites[0].ok);
    CHECK(result->sites[0].marks == std::vector<int64_t>{ 17631, 33158, 82140 });
    CHECK(result->sites[1].ok);
    CHECK(result->sites[1].marks == std::vector<int64_t>{ 17631 });
    // the maps the item files have points on, each once
    CHECK(run.sites.count("mark_map_point_list") == 4);
    CHECK(run.sites.headers[0].find("Cookie: ltuid_v2=hoyo1") == 0);
    CHECK(run.sites.headers[0].find("x-rpc-map_version") != std::string::npos);
    CHECK(run.sites.requests.back().find("users/uid1?mask.fieldPaths=markerIds") != std::string::npos);
    // a fetch writes nothing and backs nothing up
    CHECK(run.sites.count("POST") == 0);
    CHECK(Marks::Backups(run.backups).empty());
}

TEST_CASE("appsample with no document yet has nothing marked") {
    Running run;
    run.sites.appsampleDocument = false;
    Marks::Request request;
    request.sites = { Site::Appsample };
    auto result = waitFor(Marks::Start(request));
    REQUIRE(result);
    CHECK(result->sites[0].ok);
    CHECK(result->sites[0].marks.empty());
}

TEST_CASE("a change sends only what's different, after a backup of what was there") {
    Running run;
    run.sites.hoyolab[2] = { 17631, 17630 };
    run.sites.appsample = { 20503 };
    auto result = waitFor(Marks::Start(change({ Site::HoyolabMap, Site::Appsample }, { 17631, 17628, 33158 }, { 17630 })));
    REQUIRE(result);
    const Marks::SiteResult& hoyolab = result->sites[0];
    const Marks::SiteResult& appsample = result->sites[1];
    CHECK(hoyolab.ok);
    CHECK(hoyolab.added == 2);
    CHECK(hoyolab.removed == 1);
    CHECK(run.sites.hoyolab[2] == std::set<int64_t>{ 17631, 17628 });
    CHECK(run.sites.hoyolab[9] == std::set<int64_t>{ 33158 });
    // one batch per map and way
    CHECK(run.sites.count("batch_mark_map_point") == 3);
    // 17630 isn't marked on appsample, so there's nothing to unmark there
    CHECK(appsample.ok);
    CHECK(appsample.added == 2);
    CHECK(appsample.removed == 0);
    CHECK(run.sites.appsample == std::set<int64_t>{ 20503, 20506, 42251 });

    std::vector<Marks::Backup> backups = Marks::Backups(run.backups);
    REQUIRE(backups.size() == 2);
    std::vector<int64_t> saved;
    REQUIRE(Marks::ReadBackup(run.backups, hoyolab.backup, Site::HoyolabMap, "hoyo1", saved));
    CHECK(saved == std::vector<int64_t>{ 17630, 17631 });
    REQUIRE(Marks::ReadBackup(run.backups, appsample.backup, Site::Appsample, "uid1", saved));
    CHECK(saved == std::vector<int64_t>{ 20503 });

    // nothing different: nothing sent, no backup
    int posts = run.sites.count("POST");
    result = waitFor(Marks::Start(change({ Site::HoyolabMap }, { 17631 }, { 17630 })));
    REQUIRE(result);
    CHECK(result->sites[0].ok);
    CHECK(result->sites[0].backup.empty());
    CHECK(run.sites.count("POST") == posts);
    CHECK(Marks::Backups(run.backups).size() == 2);
}

TEST_CASE("appsample gets only paired points from its level 5 up, unknown levels go") {
    Running run;
    // 17630 is level 3, 17629 level 5, 17628's level isn't known, 55943 has no pair
    auto result = waitFor(Marks::Start(change({ Site::Appsample }, { 17630, 17629, 17628, 55943, 4242 }, {})));
    REQUIRE(result);
    CHECK(result->sites[0].ok);
    CHECK(sorted(result->sites[0].skipped) == std::vector<int64_t>{ 4242, 17630, 55943 });
    CHECK(run.sites.appsample == std::set<int64_t>{ 20505, 20506 });
    // the ids go as strings, the way their site keeps them, and only the change
    json commit = json::parse(run.sites.bodies.back());
    json write = commit["writes"][0];
    CHECK(write["update"]["name"] == "projects/genshin-impact/databases/(default)/documents/users/uid1");
    CHECK(write["updateMask"]["fieldPaths"].empty());
    CHECK(write["updateTransforms"][0]["fieldPath"] == "markerIds");
    CHECK(write["updateTransforms"][0]["appendMissingElements"]["values"].size() == 2);
    CHECK(write["updateTransforms"][0]["appendMissingElements"]["values"][0].contains("stringValue"));
}

TEST_CASE("a fetch with ids says which of them the site can't take, and writes nothing") {
    Running run;
    Marks::Request request = change({ Site::Appsample }, { 17630, 17629, 17628, 55943, 4242 }, {});
    request.action = Marks::Action::Fetch;
    auto result = waitFor(Marks::Start(request));
    REQUIRE(result);
    CHECK(result->sites[0].ok);
    CHECK(sorted(result->sites[0].skipped) == std::vector<int64_t>{ 4242, 17630, 55943 });
    CHECK(run.sites.count("POST") == 0);
}

TEST_CASE("HoYoLAB Map points not in the item files can't be marked but can be unmarked") {
    Running run;
    run.sites.hoyolab[2] = { 4242 };
    auto result = waitFor(Marks::Start(change({ Site::HoyolabMap }, { 5151 }, { 4242 })));
    REQUIRE(result);
    CHECK(result->sites[0].skipped == std::vector<int64_t>{ 5151 });
    CHECK(result->sites[0].removed == 1);
    CHECK(run.sites.hoyolab[2].empty());
}

TEST_CASE("unmarking more than one waits for an answer; with nobody to ask they stay marked") {
    Running run;
    run.sites.hoyolab[2] = { 17631, 17630, 17629 };
    auto result = waitFor(Marks::Start(change({ Site::HoyolabMap }, { 17628 }, { 17631, 17630 })));
    REQUIRE(result);
    CHECK(result->sites[0].ok);
    CHECK(result->sites[0].added == 1);
    CHECK(result->sites[0].removed == 0);
    CHECK(sorted(result->sites[0].kept) == std::vector<int64_t>{ 17630, 17631 });
    CHECK(run.sites.hoyolab[2] == std::set<int64_t>{ 17628, 17629, 17630, 17631 });

    // one goes without a question
    result = waitFor(Marks::Start(change({ Site::HoyolabMap }, {}, { 17631 })));
    REQUIRE(result);
    CHECK(result->sites[0].removed == 1);
    CHECK(result->sites[0].kept.empty());

    // so does one point marked on both sites
    run.sites.appsample = { 20504 };
    result = waitFor(Marks::Start(change({ Site::HoyolabMap, Site::Appsample }, {}, { 17630 })));
    REQUIRE(result);
    CHECK(result->sites[0].removed == 1);
    CHECK(result->sites[1].removed == 1);
    CHECK(run.sites.appsample.empty());
    CHECK_FALSE(run.sites.hoyolab[2].count(17630));
}

TEST_CASE("a job for an account that's gone isn't run for the next one") {
    Running run;
    run.sites.hoyolab[2] = { 17631 };
    // the first one is held up on the site
    run.sites.hold = true;
    Marks::Request fetch;
    fetch.sites = { Site::HoyolabMap };
    int first = Marks::Start(fetch);
    // queued behind it for hoyo1, then hoyo2 logs in
    int second = Marks::Start(change({ Site::HoyolabMap }, { 17628 }, {}));
    {
        std::lock_guard<std::mutex> lock(run.mutex);
        run.accounts[Site::HoyolabMap] = "hoyo2";
    }
    run.sites.hold = false;
    REQUIRE(waitFor(first));
    auto result = waitFor(second);
    REQUIRE(result);
    CHECK_FALSE(result->sites[0].ok);
    CHECK(result->sites[0].error == "account_changed");
    CHECK_FALSE(run.sites.hoyolab[2].count(17628));
}

TEST_CASE("a job waiting for an answer holds up only its owner's jobs, Forget drops them") {
    Running run;
    run.sites.hoyolab[2] = { 17631, 17630 };
    int owner = 0;
    Marks::Request held = change({ Site::HoyolabMap }, {}, { 17631, 17630 });
    held.owner = &owner;
    int first = Marks::Start(held);
    Marks::Request fetch;
    fetch.sites = { Site::HoyolabMap };
    fetch.owner = &owner;
    int behind = Marks::Start(fetch);
    // nobody's question is answered, the overlay's own jobs still run
    fetch.owner = nullptr;
    auto result = waitFor(Marks::Start(fetch));
    REQUIRE(result);
    CHECK(result->sites[0].marks == std::vector<int64_t>{ 17630, 17631 });
    CHECK(Marks::WaitingOf(&owner) == 2);
    CHECK_FALSE(Marks::Done(behind));
    Marks::Forget(&owner);
    CHECK(Marks::WaitingOf(&owner) == 0);
    CHECK_FALSE(Marks::Take(first, &owner));
    CHECK(run.sites.hoyolab[2] == std::set<int64_t>{ 17630, 17631 });
}

TEST_CASE("a forgotten job stops before its next request") {
    Running run;
    run.sites.hoyolab[2] = { 17631 };
    int owner = 0;
    Marks::Request request = change({ Site::HoyolabMap }, { 17628, 33158 }, {});
    request.owner = &owner;
    run.sites.hold = true;
    Marks::Start(request);
    auto started = std::chrono::steady_clock::now();
    while (run.sites.held == 0 && std::chrono::steady_clock::now() - started < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Marks::Forget(&owner);
    CHECK(Marks::WaitingOf(&owner) == 0);
    run.sites.hold = false;
    // the overlay's own job behind it runs once it has stopped
    Marks::Request fetch;
    fetch.sites = { Site::HoyolabMap };
    REQUIRE(waitFor(Marks::Start(fetch)));
    CHECK(run.sites.count("POST") == 0);
    CHECK(run.sites.hoyolab[2] == std::set<int64_t>{ 17631 });
}

TEST_CASE("appsample's turned down ID token is refreshed and sent once more") {
    Running run;
    run.sites.appsample = { 20503 };
    run.sites.refuse = 1;
    Marks::Request request;
    request.sites = { Site::Appsample };
    auto result = waitFor(Marks::Start(request));
    REQUIRE(result);
    CHECK(result->sites[0].ok);
    CHECK(result->sites[0].marks == std::vector<int64_t>{ 17631 });
    CHECK(run.sites.count("firestore") == 2);

    // turned down again: not a log out, the account is still there
    run.sites.refuse = 2;
    result = waitFor(Marks::Start(change({ Site::Appsample }, { 17628 }, {})));
    REQUIRE(result);
    CHECK(result->sites[0].error == "failed");
    CHECK(run.sites.appsample == std::set<int64_t>{ 20503 });

    // logged out by then
    run.sites.refuse = 1;
    Marks::Request held = request;
    run.sites.hold = true;
    int id = Marks::Start(held);
    auto started = std::chrono::steady_clock::now();
    while (run.sites.held == 0 && std::chrono::steady_clock::now() - started < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
        std::lock_guard<std::mutex> lock(run.mutex);
        run.accounts[Site::Appsample] = "";
    }
    run.sites.hold = false;
    result = waitFor(id);
    REQUIRE(result);
    CHECK(result->sites[0].error == "logged_out");
}

TEST_CASE("a change plans only what's different, a point added and removed stays marked") {
    Marks::Plan plan = Marks::PlanChange({ 1, 2, 3 }, { 3, 4, 4, 5 }, { 1, 1, 5, 6, 2 });
    CHECK(plan.add == std::vector<int64_t>{ 4, 5 });
    CHECK(plan.remove == std::vector<int64_t>{ 1, 2 });
    plan = Marks::PlanChange({ 7 }, { 7 }, { 7 });
    CHECK(plan.add.empty());
    CHECK(plan.remove.empty());
}

TEST_CASE("an appsample unmark takes the id kept as a string or a number") {
    json commit = json::parse(Marks::AppsampleCommit("uid1", { 20503 }, true));
    json values = commit["writes"][0]["updateTransforms"][0]["removeAllFromArray"]["values"];
    CHECK(values == json::array({ { { "stringValue", "20503" } }, { { "integerValue", "20503" } } }));
    commit = json::parse(Marks::AppsampleCommit("uid1", { 20503 }, false));
    CHECK(commit["writes"][0]["updateTransforms"][0]["appendMissingElements"]["values"] == json::array({ { { "stringValue", "20503" } } }));
}

TEST_CASE("a backup is read only for its own account") {
    fs::path folder = WORK / "own-backups";
    fs::remove_all(folder);
    std::string first = Marks::SaveBackup(folder, Site::HoyolabMap, "hoyo1", { 3, 1, 2 });
    std::string second = Marks::SaveBackup(folder, Site::HoyolabMap, "hoyo1", { 4 });
    REQUIRE_FALSE(first.empty());
    // two in the same second are both kept
    CHECK(second != first);
    std::vector<int64_t> marks;
    CHECK(Marks::ReadBackup(folder, first, Site::HoyolabMap, "hoyo1", marks));
    CHECK(marks == std::vector<int64_t>{ 1, 2, 3 });
    CHECK_FALSE(Marks::ReadBackup(folder, first, Site::HoyolabMap, "hoyo2", marks));
    CHECK_FALSE(Marks::ReadBackup(folder, first, Site::Appsample, "hoyo1", marks));
    CHECK_FALSE(Marks::ReadBackup(folder, "../" + first, Site::HoyolabMap, "hoyo1", marks));
    // the account isn't in the file as it is
    CHECK(Files::readText(folder / (first + ".json")).find("hoyo1") == std::string::npos);
    CHECK(Marks::Backups(folder).size() == 2);
}

TEST_CASE("HoYoLAB Map's captcha and logged out answers go to the account") {
    std::wstring prefix = Credentials::prefix;
    Credentials::prefix = L"genshin-overlay-tests/";
    fs::remove(Settings::file);
    REQUIRE(Accounts::FoundHoyolabMap("123456789", "v2_token"));
    Accounts::Frame();
    {
        Running run(true);
        // the nickname is read once the account is logged in
        Marks::Frame();
        auto started = std::chrono::steady_clock::now();
        while (Accounts::Get(Site::HoyolabMap).name.empty() && std::chrono::steady_clock::now() - started < std::chrono::seconds(5))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(Accounts::Get(Site::HoyolabMap).name == "Paimonfan");
        CHECK(run.sites.count("getUserFullInfo?uid=123456789") == 1);
        CHECK(run.sites.headers[0].find("Cookie: ltuid_v2=123456789; ltoken_v2=v2_token") == 0);

        run.sites.retcode = -5001;
        Marks::Request request;
        request.sites = { Site::HoyolabMap };
        auto result = waitFor(Marks::Start(request));
        REQUIRE(result);
        CHECK(result->sites[0].error == "captcha");
        CHECK(Accounts::Get(Site::HoyolabMap).state == Accounts::State::Captcha);

        run.sites.retcode = 0;
        result = waitFor(Marks::Start(request));
        REQUIRE(result);
        CHECK(result->sites[0].ok);
        CHECK(Accounts::Get(Site::HoyolabMap).state == Accounts::State::LoggedIn);

        run.sites.retcode = -100;
        result = waitFor(Marks::Start(request));
        REQUIRE(result);
        CHECK(result->sites[0].error == "logged_out");
        CHECK(Accounts::Get(Site::HoyolabMap).state == Accounts::State::NeedsLogin);
        CHECK_FALSE(Credentials::Read("hoyolab_map"));
    }
    Accounts::LogOut(Site::HoyolabMap);
    Credentials::prefix = prefix;
}

TEST_CASE("Marks_Start asks the player before unmarking several, the answer decides") {
    Running run;
    Extensions::Extension* ext = TestExtension::load("marks_start", "function Init() end");
    REQUIRE(ext);
    Extensions::allow(*ext, Permission::HoyolabMapAccount);
    Extensions::runInit(*ext);
    sol::state& lua = *ext->luaState;

    for (Asks::Answer answer : { Asks::Answer::DontAllow, Asks::Answer::Allow, Asks::Answer::NotNow }) {
        run.sites.hoyolab[2] = { 17631, 17630 };
        REQUIRE(TestExtension::run(ext, R"(
            _G.done = nil
            _G.id = Marks_Start("hoyolab_map", "change", { 17628 }, { 17631, 17630 }, function(result) _G.done = result end)
        )"));
        REQUIRE(lua["id"].get_type() == sol::type::number);
        auto started = std::chrono::steady_clock::now();
        while (Asks::queue().empty() && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) TestExtension::turn();
        REQUIRE(Asks::queue().size() == 1);
        const Asks::Ask& ask = Asks::queue().front();
        CHECK(ask.kind == Asks::Kind::Unmark);
        REQUIRE(ask.unmarks.size() == 1);
        CHECK(ask.unmarks[0].site == "HoYoLAB Map");
        CHECK(ask.unmarks[0].count == 2);
        CHECK(ask.unmarks[0].which == std::vector<std::pair<std::string, int>>{ { "Inazuma, Light-Up Tile Puzzle", 2 } });
        CHECK(ask.unmarkPoints == 2);
        if (answer == Asks::Answer::Allow) {
            // unmarked elsewhere while the question is up: the answer reads the marks again
            std::lock_guard<std::mutex> lock(run.sites.mutex);
            run.sites.hoyolab[2].erase(17630);
        }
        Asks::answer(answer);
        started = std::chrono::steady_clock::now();
        while (lua["done"] == sol::lua_nil && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) TestExtension::turn();
        sol::table done = lua["done"];
        REQUIRE(done.valid());
        sol::table hoyolab = done["hoyolab_map"];
        if (answer == Asks::Answer::DontAllow) {
            CHECK(hoyolab["ok"].get<bool>());
            CHECK(hoyolab["removed"].get<int>() == 0);
            CHECK(hoyolab["kept"].get<sol::table>().size() == 2);
            CHECK(run.sites.hoyolab[2] == std::set<int64_t>{ 17628, 17630, 17631 });
        }
        else if (answer == Asks::Answer::Allow) {
            CHECK(hoyolab["removed"].get<int>() == 1);
            CHECK(run.sites.hoyolab[2] == std::set<int64_t>{ 17628 });
        }
        else {
            // Cancel stops the adds too
            CHECK(hoyolab["error"].get<std::string>() == "cancelled");
            CHECK(run.sites.hoyolab[2] == std::set<int64_t>{ 17630, 17631 });
        }
        run.sites.hoyolab[2].clear();
    }
    TestExtension::unloadAll();
}

TEST_CASE("an answer covers only the points it was asked for, more marked meanwhile are asked again") {
    Running run;
    Extensions::Extension* ext = TestExtension::load("marks_again", "function Init() end");
    REQUIRE(ext);
    Extensions::allow(*ext, Permission::HoyolabMapAccount);
    Extensions::allow(*ext, Permission::AppsampleAccount);
    Extensions::runInit(*ext);
    sol::state& lua = *ext->luaState;
    run.sites.hoyolab[2] = { 17631, 17630 };
    run.sites.appsample = { 20503, 20504 };
    REQUIRE(TestExtension::run(ext, R"(
        _G.done = nil
        _G.id = Marks_Start({ "hoyolab_map", "appsample" }, "change", {}, { 17631, 17630, 17629 }, function(result) _G.done = result end)
    )"));
    auto waitAsk = [] {
        auto started = std::chrono::steady_clock::now();
        while (Asks::queue().empty() && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) TestExtension::turn();
        REQUIRE(Asks::queue().size() == 1);
        return Asks::queue().front();
    };
    Asks::Ask ask = waitAsk();
    // two points, each on both sites
    REQUIRE(ask.unmarks.size() == 2);
    CHECK(ask.unmarks[0].count == 2);
    CHECK(ask.unmarks[1].count == 2);
    CHECK(ask.unmarkPoints == 2);
    {
        std::lock_guard<std::mutex> lock(run.sites.mutex);
        run.sites.hoyolab[2].insert(17629);
    }
    Asks::answer(Asks::Answer::Allow);
    ask = waitAsk();
    CHECK(ask.unmarkPoints == 3);
    CHECK(run.sites.hoyolab[2].size() == 3);
    Asks::answer(Asks::Answer::Allow);
    auto started = std::chrono::steady_clock::now();
    while (lua["done"] == sol::lua_nil && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) TestExtension::turn();
    sol::table done = lua["done"];
    REQUIRE(done.valid());
    CHECK(done["hoyolab_map"]["removed"].get<int>() == 3);
    CHECK(done["appsample"]["removed"].get<int>() == 2);
    CHECK(run.sites.hoyolab[2].empty());
    CHECK(run.sites.appsample.empty());
    TestExtension::unloadAll();
}

TEST_CASE("switching an account permission off ends the extension's jobs for that site, with an answer") {
    Running run;
    Extensions::Extension* ext = TestExtension::load("marks_off", "function Init() end");
    REQUIRE(ext);
    Extensions::allow(*ext, Permission::HoyolabMapAccount);
    Extensions::runInit(*ext);
    run.sites.hold = true;
    REQUIRE(TestExtension::run(ext, R"(
        _G.done = nil
        Marks_Start("hoyolab_map", "change", { 17628 }, {}, function(result) _G.done = result end)
        Marks_Start("hoyolab_map", "fetch")
    )"));
    auto started = std::chrono::steady_clock::now();
    while (run.sites.held == 0 && std::chrono::steady_clock::now() - started < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(Marks::WaitingOf(ext) == 2);
    Extensions::switchPermission(*ext, Permission::HoyolabMapAccount, false);
    run.sites.hold = false;
    // the running change stops before it sends, the waiting fetch is cancelled
    started = std::chrono::steady_clock::now();
    sol::state_view lua(*ext->luaState);
    while (lua["done"] == sol::lua_nil && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) TestExtension::turn();
    sol::table done = lua["done"];
    REQUIRE(done.valid());
    CHECK_FALSE(done["hoyolab_map"]["ok"].get<bool>());
    CHECK(run.sites.count("POST") == 0);
    CHECK(Marks::WaitingOf(ext) == 1);
    TestExtension::unloadAll();
}

TEST_CASE("Marks_Start needs the site's account permission and gives Marks_Result once") {
    Running run;
    run.sites.hoyolab[2] = { 17631 };
    Extensions::Extension* ext = TestExtension::load("marks_result", "function Init() end");
    REQUIRE(ext);
    Extensions::runInit(*ext);
    sol::state& lua = *ext->luaState;
    REQUIRE(TestExtension::run(ext, R"(_G.refused = Marks_Start("hoyolab_map", "fetch"))"));
    CHECK(lua["refused"] == sol::lua_nil);

    Extensions::allow(*ext, Permission::HoyolabMapAccount);
    REQUIRE(TestExtension::run(ext, R"(
        _G.id = Marks_Start({ "hoyolab_map" }, "fetch")
        _G.bad = Marks_Start("hoyolab_map", "change", { "17631" })
        _G.unknown = Marks_Start("somewhere", "fetch")
    )"));
    CHECK(lua["bad"] == sol::lua_nil);
    CHECK(lua["unknown"] == sol::lua_nil);
    REQUIRE(lua["id"].get_type() == sol::type::number);
    auto started = std::chrono::steady_clock::now();
    while (!Marks::Done(lua["id"].get<int>()) && std::chrono::steady_clock::now() - started < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(TestExtension::run(ext, "_G.first = Marks_Result(_G.id) _G.again = Marks_Result(_G.id)"));
    sol::table first = lua["first"];
    REQUIRE(first.valid());
    CHECK(first["hoyolab_map"]["ok"].get<bool>());
    CHECK(first["hoyolab_map"]["marks"][1].get<int64_t>() == 17631);
    CHECK(lua["again"] == sol::lua_nil);
    TestExtension::unloadAll();
}

TEST_CASE("HoYoLAB Map's and appsample's real answers are read") {
    auto saved = [](const char* file, int status) {
        Net::Response response;
        response.status = status;
        response.body = Files::readText(fs::path(__FILE__).parent_path() / "data" / "marks" / file);
        REQUIRE_FALSE(response.body.empty());
        return response;
    };
    std::vector<int64_t> ids;
    CHECK(Marks::ReadHoyolabList(saved("hoyolab_list.json", 200), ids));
    CHECK(ids == std::vector<int64_t>{ 56, 58, 59 });
    ids.clear();
    CHECK(Marks::ReadHoyolabList(saved("hoyolab_list_empty.json", 200), ids));
    CHECK(ids.empty());
    Marks::HoyolabAnswer refused = Marks::ReadHoyolab(saved("hoyolab_refused.json", 200));
    CHECK(refused.read);
    CHECK(refused.retcode == -2004);
    CHECK_FALSE(Marks::ReadHoyolabList(saved("hoyolab_refused.json", 200), ids));
    CHECK(Marks::ReadHoyolab(saved("hoyolab_batch.json", 200)).retcode == 0);
    CHECK(Marks::ReadHoyolabName(saved("hoyolab_user.json", 200)) == "Paimonfan");

    CHECK(Marks::ReadAppsampleUser(saved("appsample_user.json", 200), ids));
    CHECK(ids == std::vector<int64_t>{ 29202, 28219 });
    CHECK_FALSE(Marks::ReadAppsampleUser(saved("appsample_refused.json", 401), ids));
    CHECK(json::parse(saved("appsample_commit.json", 200).body).contains("writeResults"));
}

TEST_CASE("HoYoLAB Map's and appsample's answers are read") {
    Net::Response list;
    list.status = 200;
    list.body = R"({"retcode":0,"message":"OK","data":{"list":null}})";
    std::vector<int64_t> ids;
    CHECK(Marks::ReadHoyolabList(list, ids));
    CHECK(ids.empty());
    list.body = R"({"retcode":-100,"message":"Please login","data":null})";
    CHECK_FALSE(Marks::ReadHoyolabList(list, ids));
    CHECK(Marks::ReadHoyolab(list).retcode == -100);

    Net::Response name;
    name.body = "{\"retcode\":0,\"data\":{\"user_info\":{\"nickname\":\"Pai\\nmon\"}}}";
    CHECK(Marks::ReadHoyolabName(name) == "Paimon");
    // cut at 64 bytes, never inside a character
    name.body = "{\"retcode\":0,\"data\":{\"user_info\":{\"nickname\":\"" + std::string(63, 'a') + "\\u30d1\\u30a4\"}}}";
    CHECK(Marks::ReadHoyolabName(name) == std::string(63, 'a'));

    Net::Response user;
    user.status = 200;
    user.body = R"({"name":"x","fields":{"markerIds":{"arrayValue":{}}}})";
    CHECK(Marks::ReadAppsampleUser(user, ids));
    CHECK(ids.empty());
    user.body = R"({"fields":{"markerIds":{"arrayValue":{"values":[{"stringValue":"20503"},{"integerValue":"7"},{"stringValue":"x1"}]}}}})";
    CHECK(Marks::ReadAppsampleUser(user, ids));
    CHECK(ids == std::vector<int64_t>{ 20503, 7 });
    user.status = 403;
    CHECK_FALSE(Marks::ReadAppsampleUser(user, ids));
}
