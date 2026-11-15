#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <catch2/catch_test_macros.hpp>
#include "credentials.hpp"

namespace {
    struct TestPrefix {
        std::wstring before = Credentials::prefix;
        TestPrefix() { Credentials::prefix = L"genshin-overlay-tests/"; }
        ~TestPrefix() {
            Credentials::Delete("hoyolab_map");
            Credentials::Delete("appsample");
            Credentials::prefix = before;
        }
    };

    // files under folder changed since `since` that hold text
    bool fileHolds(const std::filesystem::path& folder, std::filesystem::file_time_type since, const std::string& text) {
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
            it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (error || !it->is_regular_file(error) || it->last_write_time(error) < since) continue;
            std::ifstream file(it->path(), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (bytes.find(text) != std::string::npos) return true;
        }
        return false;
    }
}

TEST_CASE("a login is written read back and deleted per site") {
    TestPrefix testPrefix;
    REQUIRE(Credentials::Write("hoyolab_map", { "123456789", "v2_token_one" }, L"test"));
    REQUIRE(Credentials::Write("appsample", { "uid-abc", "refresh-two" }, L"test"));

    auto hoyolab = Credentials::Read("hoyolab_map");
    REQUIRE(hoyolab);
    CHECK(hoyolab->account == "123456789");
    CHECK(hoyolab->secret == "v2_token_one");
    auto appsample = Credentials::Read("appsample");
    REQUIRE(appsample);
    CHECK(appsample->account == "uid-abc");
    CHECK(appsample->secret == "refresh-two");

    CHECK(Credentials::Delete("hoyolab_map"));
    CHECK_FALSE(Credentials::Read("hoyolab_map"));
    CHECK(Credentials::Read("appsample"));
    // nothing there is fine too
    CHECK(Credentials::Delete("hoyolab_map"));
}

TEST_CASE("writing a login again replaces it") {
    TestPrefix testPrefix;
    REQUIRE(Credentials::Write("hoyolab_map", { "1", "old" }, L"test"));
    REQUIRE(Credentials::Write("hoyolab_map", { "2", "new" }, L"test"));
    auto saved = Credentials::Read("hoyolab_map");
    REQUIRE(saved);
    CHECK(saved->account == "2");
    CHECK(saved->secret == "new");
}

TEST_CASE("the store refuses an empty or too long secret") {
    TestPrefix testPrefix;
    CHECK_FALSE(Credentials::Write("hoyolab_map", { "1", "" }, L"test"));
    CHECK_FALSE(Credentials::Write("hoyolab_map", { "1", std::string(2561, 'a') }, L"test"));
    CHECK(Credentials::Write("hoyolab_map", { "1", std::string(2560, 'a') }, L"test"));
    CHECK_FALSE(Credentials::Write("", { "1", "x" }, L"test"));
}

TEST_CASE("a login never lands in a file") {
    TestPrefix testPrefix;
    auto since = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(2);
    // made at run time, tests.exe itself never holds it
    const std::string secret = "never-in-a-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(Credentials::Write("appsample", { "uid", secret }, L"test"));
    REQUIRE(Credentials::Read("appsample"));
    CHECK_FALSE(fileHolds(std::filesystem::current_path(), since, secret));
}

TEST_CASE("Wipe clears a secret") {
    std::string secret = "abc";
    Credentials::Wipe(secret);
    CHECK(secret.empty());
}
