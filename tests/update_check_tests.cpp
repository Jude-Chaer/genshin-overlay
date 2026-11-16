#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include "update_check.hpp"
#include "files.hpp"

namespace fs = std::filesystem;

namespace {
    // GitHub's answer for the app's releases on 2026-11-16: v0.2.0-beta.1
    // (pre-release), v0.1.0, and map-index (pre-release, the map data)
    std::string releases() {
        return Files::readText(fs::path(__FILE__).parent_path() / "data" / "update_check" / "releases.json");
    }
}

TEST_CASE("a later version is told apart by its numbers, then its pre-release", "[update_check]") {
    CHECK(UpdateCheck::Later("v0.2.0", "v0.1.0"));
    CHECK(UpdateCheck::Later("v0.10.0", "v0.9.9"));
    CHECK(UpdateCheck::Later("v1.0.0", "v0.99.99"));
    CHECK(UpdateCheck::Later("v0.2.0", "v0.2.0-beta.2"));
    CHECK(UpdateCheck::Later("v0.2.0-beta.2", "v0.2.0-beta.1"));
    CHECK(UpdateCheck::Later("v0.2.0-beta.10", "v0.2.0-beta.2"));
    CHECK(UpdateCheck::Later("v0.2.0-beta", "v0.2.0-alpha.3"));
    CHECK(UpdateCheck::Later("v0.2.0-beta.1.1", "v0.2.0-beta.1"));

    CHECK_FALSE(UpdateCheck::Later("v0.1.0", "v0.1.0"));
    CHECK_FALSE(UpdateCheck::Later("v0.2.0-beta.1", "v0.2.0"));
    CHECK_FALSE(UpdateCheck::Later("v0.1.0", "v0.2.0-beta.1"));
}

TEST_CASE("a version not shaped like v1.2.3 is never later or earlier", "[update_check]") {
    CHECK_FALSE(UpdateCheck::Later("map-index", "v0.1.0"));
    CHECK_FALSE(UpdateCheck::Later("v0.2.0", "dev"));
    CHECK_FALSE(UpdateCheck::Later("0.2.0", "v0.1.0"));
    CHECK_FALSE(UpdateCheck::Later("v0.2", "v0.1.0"));
    CHECK_FALSE(UpdateCheck::Later("v0.2.0 ", "v0.1.0"));
}

TEST_CASE("a release build hears only of newer releases, not pre-releases or the map data", "[update_check]") {
    UpdateCheck::Release newer = UpdateCheck::Pick(releases(), "v0.0.9");
    CHECK(newer.version == "v0.1.0");
    CHECK(newer.page == "https://github.com/PocketEsoptron/PocketEsoptron/releases/tag/v0.1.0");

    CHECK(UpdateCheck::Pick(releases(), "v0.1.0").version.empty());
    CHECK(UpdateCheck::Pick(releases(), "v0.3.0").version.empty());
}

TEST_CASE("a pre-release build hears of newer pre-releases too", "[update_check]") {
    UpdateCheck::Release newer = UpdateCheck::Pick(releases(), "v0.0.9-beta.1");
    CHECK(newer.version == "v0.2.0-beta.1");
    CHECK(newer.page == "https://github.com/PocketEsoptron/PocketEsoptron/releases/tag/v0.2.0-beta.1");

    CHECK(UpdateCheck::Pick(releases(), "v0.2.0-beta.1").version.empty());
}

TEST_CASE("a local build hears of nothing", "[update_check]") {
    CHECK(UpdateCheck::Pick(releases(), "dev").version.empty());
}

TEST_CASE("drafts, pages off GitHub and answers that aren't a list are left out", "[update_check]") {
    CHECK(UpdateCheck::Pick(R"([{"tag_name":"v0.5.0","html_url":"https://github.com/a/b/releases/tag/v0.5.0","draft":true}])", "v0.1.0").version.empty());
    CHECK(UpdateCheck::Pick(R"([{"tag_name":"v0.5.0","html_url":"https://example.com/v0.5.0"}])", "v0.1.0").version.empty());
    CHECK(UpdateCheck::Pick(R"({"message":"API rate limit exceeded"})", "v0.1.0").version.empty());
    CHECK(UpdateCheck::Pick("not json", "v0.1.0").version.empty());
    CHECK(UpdateCheck::Pick(R"([{"tag_name":5,"html_url":"https://github.com/a/b"}])", "v0.1.0").version.empty());
}
