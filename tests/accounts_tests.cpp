#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include "accounts.hpp"
#include "credentials.hpp"
#include "extensions.hpp"
#include "files.hpp"
#include "settings.hpp"
#include "test_extension.hpp"

namespace fs = std::filesystem;
using Accounts::Site;
using Accounts::State;
using Permission = Extensions::ExtensionPermissionTypes;

namespace {
    // the tests' own store entries and settings, both sites logged out
    struct Clean {
        std::wstring prefix = Credentials::prefix;
        Clean() {
            Credentials::prefix = L"genshin-overlay-tests/";
            fs::remove(Settings::file);
            Accounts::LogOut(Site::HoyolabMap);
            Accounts::LogOut(Site::Appsample);
        }
        ~Clean() {
            Accounts::LogOut(Site::HoyolabMap);
            Accounts::LogOut(Site::Appsample);
            Credentials::prefix = prefix;
        }
    };

    Net::Response saved(const std::string& file, int status) {
        Net::Response response;
        response.status = status;
        response.body = Files::readText(fs::path(__FILE__).parent_path() / "data" / "accounts" / file);
        return response;
    }
}

TEST_CASE("a refresh gives the new tokens and how long they last") {
    Accounts::Refresh refresh = Accounts::ReadRefresh(saved("securetoken_ok.json", 200));
    CHECK(refresh.ok);
    CHECK_FALSE(refresh.refused);
    CHECK(refresh.idToken == "redacted-id_token");
    CHECK(refresh.refreshToken == "redacted-refresh_token");
    CHECK(refresh.expiresIn == 3600);

    // the fields as other types
    Net::Response number;
    number.status = 200;
    number.body = R"({ "id_token": "a", "refresh_token": "b", "expires_in": 3600 })";
    CHECK(Accounts::ReadRefresh(number).expiresIn == 3600);
    Net::Response empty;
    empty.status = 200;
    empty.body = R"({ "id_token": null, "refresh_token": ["b"], "expires_in": {} })";
    CHECK_FALSE(Accounts::ReadRefresh(empty).ok);
}

TEST_CASE("the Firebase user saved by appsample's page gives the login") {
    std::string saved = Files::readText(fs::path(__FILE__).parent_path() / "data" / "accounts" / "firebase_user.json");
    std::optional<Accounts::FirebaseUser> user = Accounts::ReadFirebaseUser(saved);
    REQUIRE(user);
    CHECK(user->uid == "redacted-uid");
    CHECK(user->email == "redacted-email");
    CHECK(user->refreshToken == "redacted-refreshToken");
    CHECK(user->apiKey == "redacted-apiKey");

    nlohmann::json noToken = nlohmann::json::parse(saved);
    noToken["stsTokenManager"].erase("refreshToken");
    CHECK_FALSE(Accounts::ReadFirebaseUser(noToken.dump()));
    nlohmann::json noUid = nlohmann::json::parse(saved);
    noUid.erase("uid");
    CHECK_FALSE(Accounts::ReadFirebaseUser(noUid.dump()));
    CHECK_FALSE(Accounts::ReadFirebaseUser("not json"));
}

TEST_CASE("a refused refresh token is told apart from a network or server problem") {
    Accounts::Refresh refused = Accounts::ReadRefresh(saved("securetoken_refused.json", 400));
    CHECK(refused.refused);
    CHECK_FALSE(refused.ok);
    CHECK(refused.why == "INVALID_REFRESH_TOKEN");

    for (int status : { 0, 429, 503 }) {
        Net::Response response;
        response.status = status;
        Accounts::Refresh refresh = Accounts::ReadRefresh(response);
        CHECK_FALSE(refresh.refused);
        CHECK_FALSE(refresh.ok);
        CHECK_FALSE(refresh.why.empty());
    }
}

TEST_CASE("the login window opens only its site and the sites it logs in with") {
    CHECK(Accounts::AllowedUrl(Site::HoyolabMap, L"https://account.hoyolab.com/login"));
    CHECK(Accounts::AllowedUrl(Site::HoyolabMap, L"https://WWW.HoYoLAB.com"));
    CHECK(Accounts::AllowedUrl(Site::HoyolabMap, L"https://accounts.google.com/o/oauth2?x=1"));
    CHECK(Accounts::AllowedUrl(Site::HoyolabMap, L"about:blank"));
    CHECK(Accounts::AllowedUrl(Site::Appsample, L"https://genshin-impact-map.appsample.com/"));
    CHECK(Accounts::AllowedUrl(Site::Appsample, L"https://genshin-impact.firebaseapp.com/__/auth/handler"));
    CHECK(Accounts::AllowedUrl(Site::Appsample, L"https://accounts.google.com/signin"));
    CHECK(Accounts::AllowedUrl(Site::Appsample, L"https://accounts.google.com.sa/accounts/SetSID"));
    CHECK(Accounts::AllowedUrl(Site::HoyolabMap, L"https://accounts.google.co.uk/accounts/SetSID"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://www.google.de/"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"https://docs.google.com/forms/d/x"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://sites.google.com/view/x"));
    CHECK(Accounts::AllowedUrl(Site::Appsample, L"https://accounts.youtube.com/accounts/SetSID"));

    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"http://www.hoyolab.com"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"https://hoyolab.com.example.com"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"https://myhoyolab.com"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://notgoogle.de/"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://google.example.com/"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://google.de.example.com/"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://www.youtube.com/"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"https://hoyolab.com@example.com"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"https://example.com\\.hoyolab.com"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"javascript:alert(1)"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::HoyolabMap, L"file:///C:/"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://www.hoyolab.com"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://other.firebaseapp.com"));
    CHECK_FALSE(Accounts::AllowedUrl(Site::Appsample, L"https://facebook.com"));
}

TEST_CASE("a HoYoLAB Map login goes through each state") {
    Clean clean;
    int changes = Accounts::Changes();
    REQUIRE(Accounts::Get(Site::HoyolabMap).state == State::LoggedOut);

    Accounts::WindowOpened(Site::HoyolabMap);
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::WindowOpen);
    CHECK(Accounts::FoundHoyolabMap("123456789", "v2_token"));
    // not used before the frame has compared it with the account seen last
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::WindowOpen);
    CHECK(Accounts::Get(Site::HoyolabMap).account.empty());
    CHECK(Accounts::HoyolabMapCookie().empty());
    Accounts::Frame();
    Accounts::Status status = Accounts::Get(Site::HoyolabMap);
    CHECK(status.state == State::LoggedIn);
    CHECK(status.account == "123456789");
    CHECK(Accounts::HoyolabMapCookie() == "ltuid_v2=123456789; ltoken_v2=v2_token");
    auto kept = Credentials::Read("hoyolab_map");
    REQUIRE(kept);
    CHECK(kept->account == "123456789");
    CHECK(kept->secret == "v2_token");

    Accounts::CaptchaAsked(Site::HoyolabMap);
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::Captcha);
    Accounts::Working(Site::HoyolabMap);
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::LoggedIn);

    // a refusal of an older token: the login made since stays
    Accounts::Refused(Site::HoyolabMap, "older_token");
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::LoggedIn);
    CHECK(Credentials::Read("hoyolab_map"));

    // turned down: forgotten, the account still shown
    Accounts::Refused(Site::HoyolabMap, "v2_token");
    status = Accounts::Get(Site::HoyolabMap);
    CHECK(status.state == State::NeedsLogin);
    CHECK(status.account == "123456789");
    CHECK(Accounts::HoyolabMapCookie().empty());
    CHECK_FALSE(Credentials::Read("hoyolab_map"));

    // the window closed without a login goes back
    Accounts::WindowOpened(Site::HoyolabMap);
    Accounts::WindowClosed(Site::HoyolabMap);
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::NeedsLogin);

    Accounts::LogOut(Site::HoyolabMap);
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::LoggedOut);
    CHECK(Accounts::Get(Site::HoyolabMap).account.empty());
    CHECK(Accounts::Changes() > changes + 6);
}

TEST_CASE("a HoYoLAB Map value that could break a header isn't kept") {
    Clean clean;
    Accounts::WindowOpened(Site::HoyolabMap);
    CHECK_FALSE(Accounts::FoundHoyolabMap("123", "a; b"));
    Accounts::Status status = Accounts::Get(Site::HoyolabMap);
    CHECK(status.state == State::LoggedOut);
    CHECK_FALSE(status.problem.empty());
    CHECK_FALSE(Credentials::Read("hoyolab_map"));
}

TEST_CASE("the logins are read back at the start, one that doesn't read is deleted") {
    Clean clean;
    Accounts::FirebaseUser user{ "uid-1", "you@example.com", "refresh-1", "" };
    REQUIRE(Accounts::FoundAppsample(user));
    Accounts::Frame();
    // the old login window kept the id and token together like this
    REQUIRE(Credentials::Write("hoyolab_map", { "123", "123\ntoken" }, L"test"));
    Accounts::Shutdown();

    Accounts::Init();
    Accounts::Frame();
    Accounts::Status appsample = Accounts::Get(Site::Appsample);
    CHECK(appsample.state == State::LoggedIn);
    CHECK(appsample.account == "you@example.com");
    CHECK_FALSE(Credentials::Read("hoyolab_map"));
    CHECK(Accounts::Get(Site::HoyolabMap).state == State::LoggedOut);
}

TEST_CASE("logging in as another account switches its permission off for every extension") {
    Clean clean;
    Extensions::Extension* ext = TestExtension::load("syncs", "function Init() end");
    REQUIRE(ext != nullptr);
    Extensions::switchPermission(*ext, Permission::HoyolabMapAccount, true);
    Extensions::saveExtensionPermissions(*ext);
    // one that isn't loaded, only saved
    Settings::Grants other;
    other.saved = true;
    other.hash = "h";
    other.names = { "hoyolab_map_account", "network" };
    Settings::SaveGrants("not_loaded", other);

    REQUIRE(Accounts::FoundHoyolabMap("111", "token-a"));
    Accounts::Frame();
    Accounts::LogOut(Site::HoyolabMap);
    REQUIRE(Accounts::FoundHoyolabMap("111", "token-b"));
    Accounts::Frame();
    CHECK(Extensions::permissionState(*ext, Permission::HoyolabMapAccount) == "allowed");

    // one waiting for "Allow the same" after its files changed
    Extensions::Extension* waiting = TestExtension::load("waits", "function Init() end");
    REQUIRE(waiting != nullptr);
    waiting->grantsBeforeChange = { "hoyolab_map_account", "network" };

    REQUIRE(Accounts::FoundHoyolabMap("222", "token-c"));
    Accounts::Frame();
    CHECK(Extensions::permissionState(*ext, Permission::HoyolabMapAccount) == "off");
    Settings::Grants after = Settings::LoadGrants("not_loaded", "h");
    CHECK(after.names == std::vector<std::string>{ "network" });
    CHECK(after.off == std::vector<std::string>{ "hoyolab_map_account" });
    CHECK(waiting->grantsBeforeChange == std::vector<std::string>{ "network" });
    CHECK(waiting->switchedOff.count("hoyolab_map_account"));
    TestExtension::unloadAll();
}
