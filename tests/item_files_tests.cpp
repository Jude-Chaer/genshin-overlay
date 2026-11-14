#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "item_catalog.hpp"
#include "files.hpp"
#include "item_files.hpp"
#include "item_patch.hpp"
#include "map_data.hpp"
#include "net_stop.hpp"

// The pack in tests/data/items was cut from the item files of 2026-10-17,
// answers.md there says what's in it.

namespace fs = std::filesystem;
using json = nlohmann::json;
using ItemFiles::Reason;
using ItemFiles::State;

namespace {
    const fs::path DATA = fs::path(__FILE__).parent_path() / "data" / "items";
    const fs::path WORK = fs::temp_directory_path() / "genshin-overlay-tests" / "items";

    // every file under the folder by its path in it
    std::map<std::string, std::string> files(const fs::path& folder) {
        std::map<std::string, std::string> out;
        if (!fs::exists(folder)) return out;
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            if (entry.is_regular_file()) out[fs::relative(entry.path(), folder).generic_string()] = Files::readText(entry.path());
        }
        return out;
    }

    json meta() {
        return json::parse(Files::readText(DATA / "items.json"));
    }

    std::string goodPack() {
        return Files::readText(DATA / "items.pack.gz");
    }

    // a raw pack as the release would have it: gzipped, with an items.json that matches
    void asReleased(const std::string& raw, std::string& packed, json& released) {
        REQUIRE(Files::gzip(raw, packed));
        released = meta();
        released["size"] = packed.size();
        released["rawSize"] = raw.size();
        released["sha256"] = Files::sha256(raw);
    }

    // a fresh copy of the test folder to change, whichever test runs first
    fs::path copyOfFolder() {
        fs::path folder = WORK / "built";
        fs::remove_all(folder);
        fs::create_directories(WORK);
        fs::copy(DATA / "folder", folder, fs::copy_options::recursive);
        return folder;
    }

    // the test folder with positions/<type>.json replaced, as a released pack
    void releasedWith(int type, const std::string& positions, std::string& packed, json& released) {
        fs::path folder = copyOfFolder();
        REQUIRE(Files::writeFile(folder / "positions" / (std::to_string(type) + ".json"), positions));
        std::string raw;
        REQUIRE(ItemFiles::pack(folder, raw));
        asReleased(raw, packed, released);
    }

    // the test folder with every positions file emptied, so the pack has no points at all
    void releasedEmpty(std::string& packed, json& released) {
        fs::path folder = copyOfFolder();
        for (const auto& entry : fs::directory_iterator(folder / "positions")) REQUIRE(Files::writeFile(entry.path(), "[]"));
        std::string raw;
        REQUIRE(ItemFiles::pack(folder, raw));
        asReleased(raw, packed, released);
    }

    std::shared_ptr<ItemCatalog::Catalog> opened(const std::string& packed, const json& released) {
        std::string problem;
        auto catalog = ItemFiles::open(packed, released, nullptr, problem);
        if (!catalog) CHECK(!problem.empty());
        return catalog;
    }
}

TEST_CASE("the test pack is the test folder, packed the same way") {
    std::string raw;
    REQUIRE(ItemFiles::pack(DATA / "folder", raw));
    CHECK(raw.size() == meta()["rawSize"].get<size_t>());
    CHECK(Files::sha256(raw) == meta()["sha256"].get<std::string>());
}

TEST_CASE("the good pack opens with every point and icon of its folder") {
    auto catalog = opened(goodPack(), meta());
    REQUIRE(catalog);
    CHECK(catalog->pointCount() == 35);
    CHECK(catalog->icon(231) == Files::readText(DATA / "folder" / "icons" / "231.png"));
}

TEST_CASE("a pack is refused when a path would leave the folder or a size is past the end") {
    for (const char* name : { "bad-dotdot.pack", "bad-absolute.pack", "bad-size.pack" }) {
        INFO(name);
        std::string raw = Files::readText(DATA / name);
        REQUIRE(!raw.empty());
        CHECK_FALSE(ItemFiles::readPack(raw, [](const std::string&, std::string_view) { return true; }));
        std::string packed;
        json released;
        asReleased(raw, packed, released);
        CHECK(opened(packed, released) == nullptr);
    }
}

TEST_CASE("a pack is refused when Windows would put a file somewhere else") {
    auto any = [](const std::string&, std::string_view) { return true; };
    // a device instead of a file, or a name with its last dot dropped (tree.json. is tree.json)
    for (const char* name : { "positions/nul.json", "icons/COM1.png", "aux", "tree.json.", "icons./1.png" }) {
        INFO(name);
        CHECK_FALSE(ItemFiles::readPack(std::string(name) + "\t2\n[]", any));
    }
    // names a pack really has still go through
    CHECK(ItemFiles::readPack("positions/231.json\t2\n[]icons/con-ish.png\t1\nx", any));
}

TEST_CASE("a pack that doesn't match its items.json is refused") {
    std::string packed = goodPack();
    json other = meta();
    SECTION("damaged on the way") {
        packed[packed.size() / 2] ^= 0x55;
    }
    SECTION("another sha256") {
        other["sha256"] = std::string(64, '0');
    }
    SECTION("another size") {
        other["rawSize"] = other["rawSize"].get<size_t>() + 1;
    }
    SECTION("format 1") {
        other["format"] = 1;
    }
    SECTION("values of the wrong kind") {
        other["rawSize"] = "17400";
    }
    CHECK(opened(packed, other) == nullptr);
}

TEST_CASE("a release file is unzipped only up to the size it's allowed") {
    std::string raw, problem;
    json released = meta();
    size_t rawSize = released["rawSize"].get<size_t>();
    CHECK(Files::unpack(goodPack(), released, rawSize, raw, problem));
    CHECK(raw.size() == rawSize);
    // a meta file that says more than the cap is refused before anything is unzipped
    raw.clear();
    CHECK_FALSE(Files::unpack(goodPack(), released, rawSize - 1, raw, problem));
    CHECK(raw.empty());
    CHECK(problem == "its meta file has no sha256 or a wrong size");
}

TEST_CASE("a download that throws stops the others and the error comes out after") {
    std::atomic<int> started{ 0 };
    auto each = [&](size_t i) {
        started++;
        if (i == 3) throw std::runtime_error("icon 3");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    try {
        Files::inParallel(10000, each);
        FAIL("nothing was thrown");
    }
    catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "icon 3");
    }
    // the pool stops starting new ones once one throws
    CHECK(started < 10000);
}

TEST_CASE("a release's meta file says which format it is") {
    json read;
    std::string body = Files::readText(DATA / "items.json");
    CHECK(Files::readMeta(body, 2, read) == Files::Meta::Ok);
    CHECK(read["rawSize"] == meta()["rawSize"]);
    // an overlay that reads format 1 can't, one that reads 3 sees an old file
    CHECK(Files::readMeta(body, 1, read) == Files::Meta::Newer);
    CHECK(Files::readMeta(body, 3, read) == Files::Meta::Broken);
    CHECK(Files::readMeta("{\"format\":\"2\"}", 2, read) == Files::Meta::Broken);
    CHECK(Files::readMeta("<html>", 2, read) == Files::Meta::Broken);
}

TEST_CASE("a pack that checks out but whose points don't load is refused") {
    std::string packed;
    json released;
    SECTION("no points at all") {
        releasedEmpty(packed, released);
    }
    SECTION("a point HoYoLAB renamed a field of") {
        releasedWith(231, R"([{"id":1,"map_id":2,"x":1,"y_pos":2,"area_id":3,"point_group":null}])", packed, released);
    }
    SECTION("a positions file that isn't JSON") {
        releasedWith(231, "{ cut short", packed, released);
    }
    CHECK(opened(packed, released) == nullptr);
}

namespace {
    using Counts = std::map<int, ItemFiles::MapCount>;
    const std::vector<int> TEST_MAPS = { 2, 7, 9, 34 };

    // an items.json as the builder publishes it, with these counts for map 2
    json publishedWith(int points, int types, int pairs) {
        json published = meta();
        published["maps"]["2"] = { { "points", points }, { "types", types }, { "pairs", pairs } };
        return published;
    }

    std::string rawOf(const fs::path& folder) {
        std::string raw;
        REQUIRE(ItemFiles::pack(folder, raw));
        return raw;
    }
}

TEST_CASE("the builder counts each map's points, types and pairs") {
    Counts counts;
    CHECK(ItemFiles::checkBuilt(rawOf(DATA / "folder"), TEST_MAPS, meta(), false, counts).empty());
    // answers.md, per map
    REQUIRE(counts.size() == 4);
    auto is = [&](int map, int points, int types, int pairs) {
        INFO("map " << map);
        CHECK(counts[map].points == points);
        CHECK(counts[map].types == types);
        CHECK(counts[map].pairs == pairs);
    };
    is(2, 31, 4, 23);
    is(7, 1, 1, 1);
    is(9, 1, 1, 1);
    is(34, 2, 1, 0);
}

TEST_CASE("the builder doesn't publish a pack whose points HoYoLAB renamed a field of") {
    fs::path folder = copyOfFolder();
    // HoYoLAB's real file, with x_pos renamed on every point
    std::string positions = Files::readText(folder / "positions" / "189.json");
    for (size_t at; (at = positions.find("\"x_pos\"")) != std::string::npos;) positions.replace(at, 7, "\"x\"");
    REQUIRE(Files::writeFile(folder / "positions" / "189.json", positions));
    Counts counts;
    auto problems = ItemFiles::checkBuilt(rawOf(folder), TEST_MAPS, meta(), false, counts);
    REQUIRE(problems.size() == 1);
    // the field is named, so the workflow's log says what changed
    CHECK(problems[0].find("11 without a good x_pos") != std::string::npos);
}

TEST_CASE("the builder doesn't publish a pack with a map that has no points") {
    Counts counts;
    std::vector<int> maps = TEST_MAPS;
    maps.push_back(40);
    auto problems = ItemFiles::checkBuilt(rawOf(DATA / "folder"), maps, meta(), false, counts);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0] == "map 40 has no points");
}

TEST_CASE("the builder doesn't publish a map that lost many points, types or pairs") {
    std::string raw = rawOf(DATA / "folder");
    Counts counts;
    // map 2 has 31 points, 4 types and 23 pairs
    SECTION("a few less than published goes out") {
        CHECK(ItemFiles::checkBuilt(raw, TEST_MAPS, publishedWith(35, 5, 26), false, counts).empty());
    }
    SECTION("points") {
        auto problems = ItemFiles::checkBuilt(raw, TEST_MAPS, publishedWith(40, 4, 23), false, counts);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0] == "map 2 has 31 points, the published pack 40");
    }
    SECTION("types") {
        CHECK(ItemFiles::checkBuilt(raw, TEST_MAPS, publishedWith(31, 6, 23), false, counts).size() == 1);
    }
    SECTION("pairs") {
        CHECK(ItemFiles::checkBuilt(raw, TEST_MAPS, publishedWith(31, 4, 30), false, counts).size() == 1);
    }
    SECTION("a published map that's gone") {
        json published = meta();
        published["maps"]["37"] = { { "points", 807 }, { "types", 70 }, { "pairs", 753 } };
        CHECK(ItemFiles::checkBuilt(raw, TEST_MAPS, published, false, counts).size() == 3);
    }
    SECTION("unless the drop is meant") {
        CHECK(ItemFiles::checkBuilt(raw, TEST_MAPS, publishedWith(400, 40, 300), true, counts).empty());
    }
}

TEST_CASE("an icon that didn't come is taken from the published pack") {
    std::string raw;
    REQUIRE(ItemFiles::pack(DATA / "folder", raw));
    fs::remove_all(WORK);
    // 231 has an icon in the pack, 189 has none anywhere
    CHECK(ItemFiles::keepIcons(raw, { 231, 189 }, WORK / "items") == 1);
    std::map<std::string, std::string> kept = files(WORK / "items");
    REQUIRE(kept.size() == 1);
    CHECK(kept["icons/231.png"] == Files::readText(DATA / "folder" / "icons" / "231.png"));
}

namespace {
    // The release as update sees it, without the network: the files it has
    // by name, and what was asked for. A missing file is one that couldn't
    // be had, like with no internet.
    struct Release {
        std::map<std::string, std::string> files;
        std::vector<std::string> asked;
        // stopped by the fake download partway, like closing the overlay
        Net::Stopper* stopAt = nullptr;
        // unticked by the fake download partway, like the menu's checkbox
        std::atomic<bool>* untickAt = nullptr;

        ItemFiles::Fetch fetch() {
            return [this](const std::string& name, std::string& body, const Net::Progress& progress) {
                asked.push_back(name);
                auto file = files.find(name);
                if (file == files.end()) return false;
                // comes in pieces, so there's a percent to show
                for (size_t got = 4096; progress && got < file->second.size(); got += 4096) {
                    if (stopAt && got >= 8192) stopAt->stop();
                    if (untickAt && got >= 8192) *untickAt = false;
                    if (!progress(got)) return false;
                }
                if (progress && !progress(file->second.size())) return false;
                body = file->second;
                return true;
            };
        }
    };

    Release released() {
        Release release;
        release.files[ItemFiles::META_FILE] = Files::readText(DATA / "items.json");
        release.files[ItemFiles::PACK_FILE] = goodPack();
        return release;
    }

    struct Run {
        ItemFiles::Status status;
        std::vector<ItemFiles::Status> shown;
        uint64_t published = 0;  // catalogs published by it
    };

    // inUse is what the overlay has published before this check, none for a fresh start
    Run update(const fs::path& items, Release& release, bool download = true, Net::Stopper* stop = nullptr,
        std::shared_ptr<const ItemCatalog::Catalog> inUse = nullptr) {
        Net::Stopper never;
        std::atomic<bool> on{ download };
        Run run;
        uint64_t before = ItemCatalog::version();
        run.status = ItemFiles::update(items, release.fetch(), release.untickAt ? *release.untickAt : on, stop ? *stop : never,
            [&](const ItemFiles::Status& status) { run.shown.push_back(status); }, inUse);
        run.published = ItemCatalog::version() - before;
        return run;
    }

    // the catalog the run published is the one in use, and it's the whole test pack
    void inUse(const Run& run) {
        REQUIRE(run.published == 1);
        auto now = ItemCatalog::current();
        REQUIRE(now);
        CHECK(now->version == ItemCatalog::version());
        CHECK(now->pointCount() == 35);
    }

    fs::path noItems() {
        fs::remove_all(WORK);
        return WORK / "game" / "items";
    }

    // the test pack installed, as a player who had it would have
    fs::path packInstalled() {
        fs::path items = noItems();
        REQUIRE(ItemFiles::install(goodPack(), meta(), items));
        return items;
    }

    // a release whose items.json is changed, and whose pack is that one
    Release releasedAs(const std::string& packed, const json& published) {
        Release release;
        release.files[ItemFiles::META_FILE] = published.dump();
        release.files[ItemFiles::PACK_FILE] = packed;
        return release;
    }
}

TEST_CASE("with no item files, the release's pack comes in with its percent and is ready") {
    fs::path items = noItems();
    Release release = released();
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    CHECK(run.status.reason == Reason::None);
    inUse(run);
    // one file, the release's items.json on its first line and the pack after it
    std::map<std::string, std::string> there = files(items);
    REQUIRE(there.size() == 1);
    CHECK(there[ItemFiles::INSTALLED_FILE] == meta().dump() + "\n" + goodPack());
    REQUIRE(run.shown.size() >= 3);
    CHECK(run.shown.front().state == State::Downloading);
    CHECK(run.shown.front().percent == 0);
    CHECK(run.shown[run.shown.size() - 2].state == State::Downloading);
    CHECK(run.shown[run.shown.size() - 2].percent == 100);
    CHECK(run.shown.back().state == State::Ready);
}

TEST_CASE("item files that are the release's are loaded without downloading anything") {
    fs::path items = packInstalled();
    Release release = released();
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    inUse(run);
    CHECK(release.asked == std::vector<std::string>{ ItemFiles::META_FILE });
    REQUIRE(run.shown.size() == 1);
    CHECK(run.shown[0].state == State::Ready);
}

TEST_CASE("with no item files and no internet, there are none, and it says why") {
    fs::path items = noItems();
    Release release;
    Run run = update(items, release);
    CHECK(run.status.state == State::None);
    CHECK(run.status.reason == Reason::NoNetwork);
    CHECK(ItemFiles::waitsForNetwork(run.status));
    CHECK(run.published == 0);
    CHECK_FALSE(fs::exists(items));
    REQUIRE(run.shown.size() == 1);
    CHECK(run.shown[0].reason == Reason::NoNetwork);
}

TEST_CASE("with no internet, the item files there are used and said to be old, with their date") {
    fs::path items = packInstalled();
    Release release;
    Run run = update(items, release);
    CHECK(run.status.state == State::Old);
    CHECK(run.status.reason == Reason::NoNetwork);
    CHECK(run.status.built == "2026-10-17");
    // with a list there, it keeps trying too, so the line goes once the internet is back
    CHECK(ItemFiles::waitsForNetwork(run.status));
    inUse(run);
}

TEST_CASE("a newer pack that doesn't download leaves the old item files in use") {
    fs::path items = packInstalled();
    std::map<std::string, std::string> before = files(items);
    Release release = released();
    json newer = meta();
    newer["sha256"] = std::string(64, 'a');
    release.files[ItemFiles::META_FILE] = newer.dump();
    release.files.erase(ItemFiles::PACK_FILE);
    Run run = update(items, release);
    CHECK(run.status.state == State::Old);
    CHECK(run.status.reason == Reason::NoNetwork);
    inUse(run);
    CHECK(files(items) == before);
}

TEST_CASE("a newer pack that's damaged leaves the old item files in use") {
    fs::path items = packInstalled();
    std::map<std::string, std::string> before = files(items);
    Release release = released();
    json newer = meta();
    newer["sha256"] = std::string(64, 'a');
    release.files[ItemFiles::META_FILE] = newer.dump();
    Run run = update(items, release);
    CHECK(run.status.state == State::Old);
    CHECK(run.status.reason == Reason::BrokenPack);
    inUse(run);
    CHECK(files(items) == before);
}

TEST_CASE("a pack that checks out but has no points keeps the old file and isn't fetched again on its own") {
    fs::path items = packInstalled();
    std::map<std::string, std::string> before = files(items);
    std::string packed;
    json empty;
    releasedEmpty(packed, empty);
    Release release = releasedAs(packed, empty);
    Run run = update(items, release);
    CHECK(run.status.state == State::Old);
    CHECK(run.status.reason == Reason::BrokenPack);
    CHECK_FALSE(ItemFiles::waitsForNetwork(run.status));
    inUse(run);
    CHECK(files(items) == before);

    // and with no list before it, there are none, still without the minute retry
    items = noItems();
    Run first = update(items, release);
    CHECK(first.status.state == State::None);
    CHECK(first.status.reason == Reason::BrokenPack);
    CHECK_FALSE(ItemFiles::waitsForNetwork(first.status));
    CHECK(first.published == 0);
    CHECK_FALSE(fs::exists(items));
}

TEST_CASE("a release in a format this overlay can't read keeps the item files there and doesn't retry") {
    Release release = released();
    json later = meta();
    later["format"] = ItemFiles::FORMAT + 1;
    release.files[ItemFiles::META_FILE] = later.dump();

    fs::path items = packInstalled();
    Run run = update(items, release);
    CHECK(run.status.state == State::Old);
    CHECK(run.status.reason == Reason::NewerFormat);
    CHECK(release.asked == std::vector<std::string>{ ItemFiles::META_FILE });
    inUse(run);

    items = noItems();
    Run none = update(items, release);
    CHECK(none.status.state == State::None);
    CHECK(none.status.reason == Reason::NewerFormat);
    CHECK_FALSE(ItemFiles::waitsForNetwork(none.status));
}

TEST_CASE("an items.json that isn't one is a broken pack, not a missing network") {
    fs::path items = noItems();
    Release release = released();
    release.files[ItemFiles::META_FILE] = "<html>not here</html>";
    Run run = update(items, release);
    CHECK(run.status.state == State::None);
    CHECK(run.status.reason == Reason::BrokenPack);
}

TEST_CASE("an installed file that can't be loaded is downloaded again, even when it's the release's") {
    fs::path items = packInstalled();
    std::string damaged = Files::readText(items / ItemFiles::INSTALLED_FILE);
    damaged[damaged.size() / 2] ^= 0x55;
    REQUIRE(Files::writeFile(items / ItemFiles::INSTALLED_FILE, damaged));
    Release release = released();
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    inUse(run);
    CHECK(files(items)[ItemFiles::INSTALLED_FILE] == meta().dump() + "\n" + goodPack());
}

TEST_CASE("an installed file of another format isn't used") {
    fs::path items = noItems();
    json later = meta();
    later["format"] = ItemFiles::FORMAT + 1;
    REQUIRE(ItemFiles::install(goodPack(), later, items));
    Release release;
    Run run = update(items, release);
    CHECK(run.status.state == State::None);
    CHECK(run.published == 0);
}

TEST_CASE("a pack that can't be saved is still used, and it says it can't save") {
    fs::path items = noItems();
    // a folder where items.pack goes, so it can't be written there
    fs::create_directories(items / ItemFiles::INSTALLED_FILE / "in the way");
    Release release = released();
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    CHECK(run.status.reason == Reason::CantSave);
    CHECK_FALSE(ItemFiles::waitsForNetwork(run.status));
    inUse(run);
}

// The checks after the first one (the menu's buttons, the checkbox, the minute retry)

TEST_CASE("a check with the same pack in use loads nothing again") {
    fs::path items = packInstalled();
    Release release = released();
    Run first = update(items, release);
    inUse(first);
    auto now = ItemCatalog::current();
    Run again = update(items, release, true, nullptr, now);
    CHECK(again.status.state == State::Ready);
    CHECK(again.status.reason == Reason::None);
    CHECK(again.published == 0);
    CHECK(ItemCatalog::current() == now);
}

TEST_CASE("a check that fails keeps the list in use, not the one on disk") {
    // the list in use couldn't be saved, and another pack is on disk
    fs::path items = noItems();
    fs::create_directories(items / ItemFiles::INSTALLED_FILE / "in the way");
    Release release = released();
    Run first = update(items, release);
    REQUIRE(first.status.reason == Reason::CantSave);
    inUse(first);
    auto now = ItemCatalog::current();
    fs::remove_all(items / ItemFiles::INSTALLED_FILE);
    std::string otherPack;
    json otherMeta;
    releasedWith(231, "[]", otherPack, otherMeta);
    REQUIRE(ItemFiles::install(otherPack, otherMeta, items));

    Release offline;
    Run again = update(items, offline, true, nullptr, now);
    CHECK(again.status.state == State::Old);
    CHECK(again.status.reason == Reason::NoNetwork);
    CHECK(again.status.built == "2026-10-17");
    CHECK(again.published == 0);
    CHECK(ItemCatalog::current() == now);
}

TEST_CASE("trying again after it couldn't save writes the pack in use without loading it again") {
    fs::path items = noItems();
    fs::create_directories(items / ItemFiles::INSTALLED_FILE / "in the way");
    Release release = released();
    Run first = update(items, release);
    REQUIRE(first.status.reason == Reason::CantSave);
    auto now = ItemCatalog::current();
    // the player made the folder writable
    fs::remove_all(items / ItemFiles::INSTALLED_FILE);
    Run again = update(items, release, true, nullptr, now);
    CHECK(again.status.state == State::Ready);
    CHECK(again.status.reason == Reason::None);
    CHECK(again.published == 0);
    CHECK(files(items)[ItemFiles::INSTALLED_FILE] == meta().dump() + "\n" + goodPack());
}

TEST_CASE("turning the download off keeps the list in use") {
    fs::path items = packInstalled();
    Release release = released();
    Run first = update(items, release);
    inUse(first);
    auto now = ItemCatalog::current();
    Run off = update(items, release, false, nullptr, now);
    CHECK(off.status.state == State::Old);
    CHECK(off.status.reason == Reason::DownloadOff);
    CHECK(off.published == 0);
    CHECK(ItemCatalog::current() == now);
}

TEST_CASE("with the download off, nothing goes online and the list there is used") {
    fs::path items = packInstalled();
    Release release = released();
    Run run = update(items, release, false);
    CHECK(release.asked.empty());
    CHECK(run.status.state == State::Old);
    CHECK(run.status.reason == Reason::DownloadOff);
    inUse(run);

    items = noItems();
    Run none = update(items, release, false);
    CHECK(release.asked.empty());
    CHECK(none.status.state == State::None);
    CHECK(none.status.reason == Reason::DownloadOff);
    CHECK_FALSE(ItemFiles::waitsForNetwork(none.status));
}

TEST_CASE("the unpacked folder of an older build is removed, and the pack comes again") {
    fs::path items = noItems();
    for (const char* name : { "tree.json", "areas.json", "positions/231.json", "icons/231.png", "version.txt", "built.txt" }) {
        REQUIRE(Files::writeFile(items / name, "old"));
    }
    fs::path old = items;
    old += ".old";
    REQUIRE(Files::writeFile(old / "tree.json", "older"));
    Release release = released();
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    std::map<std::string, std::string> there = files(WORK / "game");
    CHECK(there.size() == 1);
    CHECK(there.count("items/" + ItemFiles::INSTALLED_FILE) == 1);
}

TEST_CASE("closing the overlay stops a download partway and changes nothing") {
    fs::path items = noItems();
    Release release = released();
    Net::Stopper stop;
    release.stopAt = &stop;
    Run run = update(items, release, true, &stop);
    CHECK(run.status.state == State::None);
    CHECK(run.published == 0);
    CHECK_FALSE(fs::exists(items));
    CHECK(run.shown.back().state == State::Downloading);
    CHECK(run.shown.back().percent < 100);
}

TEST_CASE("unticking the download partway stops it and says the download is off, not no internet") {
    fs::path items = noItems();
    Release release = released();
    std::atomic<bool> on{ true };
    release.untickAt = &on;
    Run run = update(items, release);
    CHECK(run.status.state == State::None);
    CHECK(run.status.reason == Reason::DownloadOff);
    CHECK_FALSE(ItemFiles::waitsForNetwork(run.status));
    CHECK(run.published == 0);
    CHECK_FALSE(fs::exists(items));
    // nothing on the way says no internet
    for (const ItemFiles::Status& shown : run.shown) CHECK(shown.reason != Reason::NoNetwork);
}

TEST_CASE("a pack bigger than its items.json says, or with no size, is stopped as broken") {
    fs::path items = noItems();
    Release release = released();
    json wrong = meta();
    SECTION("smaller than it is") {
        wrong["size"] = 5000;
    }
    SECTION("no size") {
        wrong.erase("size");
    }
    release.files[ItemFiles::META_FILE] = wrong.dump();
    Run run = update(items, release);
    CHECK(run.status.state == State::None);
    CHECK(run.status.reason == Reason::BrokenPack);
    CHECK_FALSE(fs::exists(items));
}

namespace {
    // One side of update/ added to the test folder, as a pack and items.json would have it.
    struct Side {
        std::string raw, packed;
        json meta;
    };

    Side sideOf(const std::vector<fs::path>& extra) {
        fs::path folder = copyOfFolder();
        for (const fs::path& from : extra) {
            fs::copy(from, folder, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        Side side;
        REQUIRE(ItemFiles::pack(folder, side.raw));
        asReleased(side.raw, side.packed, side.meta);
        side.meta["file"] = ItemFiles::packName(side.meta["sha256"].get<std::string>());
        return side;
    }

    Side before() { return sideOf({ DATA / "update" / "before" }); }
    Side after() { return sideOf({ DATA / "update" / "before", DATA / "update" / "after" }); }
    // between them: only 294 changed
    Side between() {
        fs::path only294 = WORK / "only294";
        fs::remove_all(only294);
        fs::create_directories(only294 / "positions");
        fs::copy(DATA / "update" / "after" / "positions" / "294.json", only294 / "positions");
        return sideOf({ DATA / "update" / "before", only294 });
    }

    // the patch from one side to the next, on the release and in its items.json
    void addPatch(Release& release, json& published, const Side& from, const Side& to) {
        std::string steps = ItemPatch::make(from.raw, to.raw), packed;
        REQUIRE(Files::gzip(steps, packed));
        std::string fromHash = from.meta["sha256"], toHash = to.meta["sha256"];
        std::string name = ItemFiles::patchName(fromHash, toHash);
        release.files[name] = packed;
        if (!published.contains("patches")) published["patches"] = json::array();
        published["patches"].push_back({ { "from", fromHash }, { "to", toHash }, { "file", name }, { "size", packed.size() },
            { "rawSize", steps.size() }, { "built", "2026-11-14T13:30:02Z" } });
        release.files[ItemFiles::META_FILE] = published.dump();
    }

    // the release with to as its pack, under its own name only
    Release releasedSide(const Side& to) {
        Release release;
        release.files[ItemFiles::META_FILE] = to.meta.dump();
        release.files[to.meta["file"].get<std::string>()] = to.packed;
        return release;
    }

    fs::path installed(const Side& side) {
        fs::path items = noItems();
        REQUIRE(ItemFiles::install(side.packed, side.meta, items));
        return items;
    }

    // the list in use is update/after's, as answers.md says
    void isAfter() {
        auto now = ItemCatalog::current();
        REQUIRE(now);
        CHECK(now->pointCount() == 35 + 113 + 42 + 27 + 1);
        REQUIRE(now->point(60111));
        CHECK(now->point(60111)->appsample == 0);
        REQUIRE(now->point(48113));
        CHECK(now->point(48113)->group == 43);
        CHECK(now->point(48113)->floor == 53);
        CHECK(now->point(48113)->appsample == 48694);
        CHECK(now->point(93667) == nullptr);
        REQUIRE(now->points(830).size() == 1);
        CHECK(now->points(830)[0].id == 105949);
        CHECK(now->icon(830).size() == 7570);
    }

    // what's installed is the side's items.json and a pack that unzips to its raw pack
    void installedIs(const fs::path& items, const Side& side) {
        std::string all = Files::readText(items / ItemFiles::INSTALLED_FILE);
        size_t end = all.find('\n');
        REQUIRE(end != std::string::npos);
        CHECK(json::parse(all.substr(0, end)) == side.meta);
        std::string raw;
        REQUIRE(Files::gunzip(all.substr(end + 1), raw, side.raw.size()));
        CHECK(raw == side.raw);
    }
}

TEST_CASE("a newer pack comes as a patch from the installed one") {
    Side from = before(), to = after();
    fs::path items = installed(from);
    Release release = releasedSide(to);
    json published = to.meta;
    addPatch(release, published, from, to);
    std::string problem;
    std::shared_ptr<const ItemCatalog::Catalog> inUse = ItemFiles::open(from.packed, from.meta, nullptr, problem);
    REQUIRE(inUse);
    Run run = update(items, release, true, nullptr, inUse);
    CHECK(run.status.state == State::Ready);
    CHECK(run.status.reason == Reason::None);
    CHECK(run.published == 1);
    CHECK(release.asked == std::vector<std::string>{ ItemFiles::META_FILE, published["patches"][0]["file"].get<std::string>() });
    isAfter();
    installedIs(items, { to.raw, to.packed, published });
}

TEST_CASE("two patches in a row are both applied, then the next check needs nothing") {
    Side from = before(), mid = between(), to = after();
    fs::path items = installed(from);
    Release release = releasedSide(to);
    json published = to.meta;
    addPatch(release, published, from, mid);
    addPatch(release, published, mid, to);
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    CHECK(release.asked.size() == 3);
    isAfter();
    installedIs(items, { to.raw, to.packed, published });

    release.asked.clear();
    Run again = update(items, release, true, nullptr, ItemCatalog::current());
    CHECK(again.status.state == State::Ready);
    CHECK(again.published == 0);
    CHECK(release.asked == std::vector<std::string>{ ItemFiles::META_FILE });
}

TEST_CASE("the chain starts at the installed pack, older patches are skipped") {
    Side from = before(), mid = between(), to = after();
    fs::path items = installed(mid);
    Release release = releasedSide(to);
    json published = to.meta;
    addPatch(release, published, from, mid);
    addPatch(release, published, mid, to);
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    CHECK(release.asked == std::vector<std::string>{ ItemFiles::META_FILE, published["patches"][1]["file"].get<std::string>() });
    isAfter();
}

TEST_CASE("when the patches don't lead to the release's pack, the whole pack comes") {
    Side from = before(), mid = between(), to = after();
    fs::path items = installed(from);
    Release release = releasedSide(to);
    json published = to.meta;
    SECTION("none from the installed pack") {
        addPatch(release, published, mid, to);
    }
    SECTION("a gap in the chain") {
        addPatch(release, published, from, mid);
        published["patches"][0]["to"] = std::string(64, 'a');
        addPatch(release, published, mid, to);
    }
    SECTION("a patch that makes another pack than it says") {
        addPatch(release, published, from, mid);
        published["patches"][0]["to"] = to.meta["sha256"];
        release.files[ItemFiles::META_FILE] = published.dump();
    }
    SECTION("a patch that didn't come") {
        addPatch(release, published, from, to);
        release.files.erase(published["patches"][0]["file"].get<std::string>());
    }
    SECTION("a patch bigger than its items.json says") {
        addPatch(release, published, from, to);
        published["patches"][0]["size"] = 10;
        release.files[ItemFiles::META_FILE] = published.dump();
    }
    SECTION("patches that come to more than half the pack") {
        addPatch(release, published, from, to);
        published["patches"][0]["size"] = to.packed.size() / 2 + 1;
        release.files[ItemFiles::META_FILE] = published.dump();
    }
    SECTION("a patch with a name the builder doesn't make") {
        addPatch(release, published, from, to);
        published["patches"][0]["file"] = "../items.pack.gz";
        release.files[ItemFiles::META_FILE] = published.dump();
    }
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    CHECK(run.status.reason == Reason::None);
    REQUIRE_FALSE(release.asked.empty());
    CHECK(release.asked.back() == to.meta["file"].get<std::string>());
    isAfter();
    installedIs(items, { to.raw, to.packed, published });
}

TEST_CASE("a pack name in items.json that the builder doesn't make is taken as items.pack.gz") {
    fs::path items = noItems();
    Release release = released();
    json wrong = meta();
    wrong["file"] = "../../evil.gz";
    release.files[ItemFiles::META_FILE] = wrong.dump();
    Run run = update(items, release);
    CHECK(run.status.state == State::Ready);
    CHECK(release.asked.back() == ItemFiles::PACK_FILE);
}

TEST_CASE("the release's names for packs and patches are by sha256") {
    std::string a = "815f9c5bbb30a6f6a7b372bf8488c9295c554772c0f7b9ba394c982517197974";
    std::string b = "ac538e57928e3760420081450c971d8b1c474d279d5eed89344a0162393f2434";
    CHECK(ItemFiles::packName(a) == "items-815f9c5bbb30.pack.gz");
    CHECK(ItemFiles::patchName(a, b) == "items-815f9c5bbb30-ac538e57928e.patch.gz");
}

TEST_CASE("a check nobody asked for shows only a new list, and when the next one runs") {
    ItemFiles::Status shown;
    shown.state = State::Ready;
    shown.started = true;
    SECTION("a failed check leaves what's shown") {
        for (Reason reason : { Reason::NoNetwork, Reason::BrokenPack, Reason::NewerFormat, Reason::CantSave }) {
            ItemFiles::Status result;
            result.state = State::Old;
            result.reason = reason;
            ItemFiles::Status now = ItemFiles::afterQuietCheck(shown, result);
            CHECK(now.state == State::Ready);
            CHECK(now.reason == Reason::None);
        }
        CHECK(ItemFiles::nextCheck(shown) == ItemFiles::HOURLY);
    }
    SECTION("an old list with no internet gets the new one once it's back") {
        shown.state = State::Old;
        shown.reason = Reason::NoNetwork;
        shown.built = "2026-10-26";
        CHECK(ItemFiles::nextCheck(shown) == ItemFiles::RETRY);
        ItemFiles::Status result;
        result.state = State::Ready;
        result.built = "2026-11-14";
        ItemFiles::Status now = ItemFiles::afterQuietCheck(shown, result);
        CHECK(now.state == State::Ready);
        CHECK(now.reason == Reason::None);
        CHECK(now.built == "2026-11-14");
        CHECK(now.started);
        CHECK(ItemFiles::nextCheck(now) == ItemFiles::HOURLY);
    }
    SECTION("the player's check that's waiting stays shown as checking") {
        shown.checking = true;
        ItemFiles::Status result;
        result.state = State::Ready;
        CHECK(ItemFiles::afterQuietCheck(shown, result).checking);
    }
}
