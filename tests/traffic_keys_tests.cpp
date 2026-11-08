#include <catch2/catch_test_macros.hpp>
#include <windows.h>
#include <filesystem>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
#include "files.hpp"
#include "packets/session_key.hpp"
#include "packets/traffic_keys.hpp"

// tests/data/traffic-keys/traffic-keys.json is what .github/scripts/traffic_keys.py
// made on 2026-11-08 from Irminsul's keys/gi.json and auto-artifactarium's
// keys/private_key_4.pem and _5.pem, the same keys as packets/.

namespace {
    const std::filesystem::path TESTS = std::filesystem::path(__FILE__).parent_path();
    const std::filesystem::path RELEASED = TESTS / "data" / "traffic-keys" / "traffic-keys.json";
    const std::filesystem::path SHIPPED = TESTS.parent_path() / "packets";

    std::string released() {
        std::string body = Files::readText(RELEASED);
        REQUIRE_FALSE(body.empty());
        return body;
    }

    SessionKey::Keys shipped() {
        SessionKey::Keys out;
        REQUIRE(SessionKey::LoadKeys(SHIPPED, out));
        return out;
    }

    std::string changed(const std::function<void(nlohmann::json&)>& change) {
        nlohmann::json keys = nlohmann::json::parse(released());
        change(keys);
        return keys.dump();
    }

    bool reads(const std::string& body) {
        SessionKey::Keys out;
        std::string problem;
        return TrafficKeys::Read(body, out, problem);
    }

    // packets/ copied to a folder of its own, a stand in for the one next to the exe
    struct Folder {
        std::filesystem::path path;
        Folder() {
            path = std::filesystem::temp_directory_path() / ("traffic-keys-test-" + std::to_string(GetCurrentProcessId()));
            std::filesystem::remove_all(path);
            std::filesystem::copy(SHIPPED, path);
        }
        ~Folder() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };
}

TEST_CASE("The released keys file reads and has packets/'s keys") {
    SessionKey::Keys got;
    std::string problem;
    REQUIRE(TrafficKeys::Read(released(), got, problem));
    SessionKey::Keys mine = shipped();
    CHECK(got.dispatch == mine.dispatch);
    REQUIRE(got.rsaPem.size() == 2);
    CHECK(SessionKey::Rsa(got.rsaPem).AllLoaded());
}

TEST_CASE("A keys file that isn't right is refused whole") {
    CHECK_FALSE(reads(""));
    CHECK_FALSE(reads("not json"));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys["format"] = 2; })));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys.erase("format"); })));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys["dispatch"] = nlohmann::json::object(); })));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys["rsa"] = nlohmann::json::object(); })));
    // one key cut short, the others fine
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) {
        std::string& key = keys["dispatch"]["13980"].get_ref<std::string&>();
        key.resize(key.size() - 8);
    })));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys["dispatch"]["013980"] = keys["dispatch"]["13980"]; })));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys["dispatch"]["70000"] = keys["dispatch"]["13980"]; })));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys["rsa"]["4"] = "-----BEGIN RSA PRIVATE KEY-----\nnot a key\n-----END RSA PRIVATE KEY-----\n"; })));
    CHECK_FALSE(reads(changed([](nlohmann::json& keys) { keys["rsa"]["4"] = 4; })));
}

TEST_CASE("Merging keeps every key, the newer one wins for an id") {
    SessionKey::Keys into = shipped();
    SessionKey::Keys from;
    std::string problem;
    REQUIRE(TrafficKeys::Read(released(), from, problem));
    // a key the release has and packets/ doesn't, and one it changed
    from.dispatch[7] = from.dispatch.begin()->second;
    std::string changedKey = std::next(from.dispatch.begin(), 1)->second;
    uint16_t lastId = from.dispatch.rbegin()->first;
    from.dispatch[lastId] = changedKey;
    TrafficKeys::Merge(into, from);
    CHECK(into.dispatch.size() == shipped().dispatch.size() + 1);
    CHECK(into.dispatch[7] == from.dispatch[7]);
    CHECK(into.dispatch[lastId] == changedKey);
    // the same login keys with \r\n aren't taken twice
    CHECK(into.rsaPem.size() == 2);
}

TEST_CASE("A new keys file is saved and used next to packets/, the same one isn't again") {
    Folder folder;
    std::string body = released();
    SessionKey::Keys out;
    CHECK(TrafficKeys::Take(folder.path, 200, body, out) == TrafficKeys::Got::New);
    CHECK(Files::readText(folder.path / TrafficKeys::FILE) == body);
    CHECK(out.dispatch == shipped().dispatch);
    CHECK(SessionKey::Rsa(out.rsaPem).AllLoaded());
    CHECK(TrafficKeys::Take(folder.path, 200, body, out) == TrafficKeys::Got::Same);

    // a newer file with a key packets/ doesn't have
    std::string newer = changed([](nlohmann::json& keys) { keys["dispatch"]["7"] = keys["dispatch"]["13980"]; });
    SessionKey::Keys got;
    CHECK(TrafficKeys::Take(folder.path, 200, newer, got) == TrafficKeys::Got::New);
    CHECK(got.dispatch.count(7) == 1);
    SessionKey::Keys loaded;
    REQUIRE(TrafficKeys::Load(folder.path, loaded));
    CHECK(loaded.dispatch == got.dispatch);
}

TEST_CASE("No keys file or a broken one leaves the saved one alone") {
    Folder folder;
    std::string body = released();
    SessionKey::Keys out;
    REQUIRE(TrafficKeys::Take(folder.path, 200, body, out) == TrafficKeys::Got::New);
    CHECK(TrafficKeys::Take(folder.path, 404, "Not Found", out) == TrafficKeys::Got::Missing);
    CHECK(TrafficKeys::Take(folder.path, 503, "", out) == TrafficKeys::Got::Unreached);
    CHECK(TrafficKeys::Take(folder.path, 0, "", out) == TrafficKeys::Got::Unreached);
    CHECK(TrafficKeys::Take(folder.path, 200, body.substr(0, body.size() / 2), out) == TrafficKeys::Got::Broken);
    CHECK(Files::readText(folder.path / TrafficKeys::FILE) == body);
}

TEST_CASE("A saved keys file that doesn't read is left out, packets/ still loads") {
    Folder folder;
    REQUIRE(Files::writeFile(folder.path / TrafficKeys::FILE, "{\"format\": 1}"));
    SessionKey::Keys loaded;
    REQUIRE(TrafficKeys::Load(folder.path, loaded));
    CHECK(loaded.dispatch == shipped().dispatch);
}

TEST_CASE("The release's keys file downloads", "[.][live]") {
    Folder folder;
    SessionKey::Keys out;
    TrafficKeys::Got got = TrafficKeys::Download(folder.path, out);
    INFO(TrafficKeys::GotCode(got));
    CHECK(got == TrafficKeys::Got::New);
}
