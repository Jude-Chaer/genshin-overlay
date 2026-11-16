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

namespace {
    fs::path data(const char* name) {
        return fs::path(__FILE__).parent_path() / "data" / "update_check" / name;
    }

    // a release the way release.yml publishes it
    std::string publishedRelease(const std::string& downloads) {
        return R"([{"tag_name":"v0.3.0","html_url":"https://github.com/PocketEsoptron/PocketEsoptron/releases/tag/v0.3.0","assets":[)"
            R"({"name":"PocketEsoptron-Setup-v0.3.0.exe","browser_download_url":")" + downloads + R"(PocketEsoptron-Setup-v0.3.0.exe"},)"
            R"({"name":"PocketEsoptron-v0.3.0-portable.zip","browser_download_url":")" + downloads + R"(PocketEsoptron-v0.3.0-portable.zip"},)"
            R"({"name":"SHA256SUMS.txt","browser_download_url":")" + downloads + R"(SHA256SUMS.txt"}]}])";
    }
}

TEST_CASE("a newer release comes with its installer, portable zip and sums", "[update_check]") {
    UpdateCheck::Release newer = UpdateCheck::Pick(publishedRelease("https://github.com/PocketEsoptron/PocketEsoptron/releases/download/v0.3.0/"), "v0.1.0");
    CHECK(newer.version == "v0.3.0");
    CHECK(newer.installer == "https://github.com/PocketEsoptron/PocketEsoptron/releases/download/v0.3.0/PocketEsoptron-Setup-v0.3.0.exe");
    CHECK(newer.portable == "https://github.com/PocketEsoptron/PocketEsoptron/releases/download/v0.3.0/PocketEsoptron-v0.3.0-portable.zip");
    CHECK(newer.sums == "https://github.com/PocketEsoptron/PocketEsoptron/releases/download/v0.3.0/SHA256SUMS.txt");
}

TEST_CASE("downloads from anywhere but the release itself are left out", "[update_check]") {
    UpdateCheck::Release elsewhere = UpdateCheck::Pick(publishedRelease("https://example.com/v0.3.0/"), "v0.1.0");
    CHECK(elsewhere.version == "v0.3.0");
    CHECK(elsewhere.installer.empty());
    CHECK(elsewhere.portable.empty());
    CHECK(elsewhere.sums.empty());

    UpdateCheck::Release otherTag = UpdateCheck::Pick(publishedRelease("https://github.com/PocketEsoptron/PocketEsoptron/releases/download/v0.2.0/"), "v0.1.0");
    CHECK(otherTag.installer.empty());
}

TEST_CASE("the releases before the installer have nothing to download", "[update_check]") {
    UpdateCheck::Release newer = UpdateCheck::Pick(releases(), "v0.0.9");
    CHECK(newer.version == "v0.1.0");
    CHECK(newer.installer.empty());
    CHECK(newer.portable.empty());
    CHECK(newer.sums.empty());
}

TEST_CASE("SHA256SUMS.txt gives each file's sum", "[update_check]") {
    std::string sums = Files::readText(data("SHA256SUMS.txt"));
    CHECK(UpdateCheck::SumFor(sums, "PocketEsoptron-Setup-v9.9.9.exe") == "4b00b58dc38c0df822ec4b3392e1f58fd9d71de961e23c97510b09857478f061");
    CHECK(UpdateCheck::SumFor(sums, "PocketEsoptron-v9.9.9-portable.zip") == Files::sha256(Files::readText(data("PocketEsoptron-v9.9.9-portable.zip"))));
    CHECK_FALSE(UpdateCheck::SumFor(sums, "PocketEsoptron-v9.9.9.zip"));
    CHECK_FALSE(UpdateCheck::SumFor(sums, "v9.9.9-portable.zip"));
    CHECK_FALSE(UpdateCheck::SumFor("", "PocketEsoptron-v9.9.9-portable.zip"));
    // sha256sum's binary mark, and a BOM
    CHECK(UpdateCheck::SumFor("\xEF\xBB\xBF" "874BEFCF94CAF35D975E41C52F358A98025FE47C397230633A7F7FF7EEF6C3BC *a.zip\n", "a.zip")
        == "874befcf94caf35d975e41c52f358a98025fe47c397230633a7f7ff7eef6c3bc");
    CHECK_FALSE(UpdateCheck::SumFor("874befcf94caf35d975e41c52f358a98025fe47c397230633a7f7ff7eef6c3bz  a.zip", "a.zip"));
}

TEST_CASE("a copy with the uninstaller next to it is the installed one", "[update_check]") {
    fs::path folder = fs::temp_directory_path() / "genshin-overlay-tests" / "installed";
    fs::remove_all(folder);
    fs::create_directories(folder);
    CHECK_FALSE(UpdateCheck::Installed(folder));
    Files::writeFile(folder / "unins000.exe", "x");
    CHECK(UpdateCheck::Installed(folder));
    fs::remove_all(folder);
}

TEST_CASE("a portable update puts the new files over the old, moves the exe aside and keeps the folder's own files", "[update_check]") {
    fs::path root = fs::temp_directory_path() / "genshin-overlay-tests" / "portable-update";
    fs::remove_all(root);
    fs::path app = root / "app";
    fs::create_directories(app / "extensions" / "markers");
    Files::writeFile(app / "PocketEsoptron.exe", "old exe");
    Files::writeFile(app / "PocketEsoptron.exe.old", "older exe");
    Files::writeFile(app / "extensions" / "markers" / "main.lua", "-- old markers");
    Files::writeFile(app / "extensions" / "markers" / "saved.json", "{}");

    fs::path unpacked = UpdateCheck::Unpack(data("PocketEsoptron-v9.9.9-portable.zip"), root / "unpacked");
    REQUIRE(unpacked == root / "unpacked" / "PocketEsoptron");
    CHECK(fs::exists(unpacked / "portable"));

    REQUIRE(UpdateCheck::PutInPlace(unpacked, app));
    CHECK(Files::readText(app / "PocketEsoptron.exe") == "new exe");
    CHECK(Files::readText(app / "PocketEsoptron.exe.old") == "old exe");
    CHECK(Files::readText(app / "extensions" / "markers" / "main.lua") == "-- new markers");
    CHECK(Files::readText(app / "extensions" / "markers" / "saved.json") == "{}");
    // a copy without it keeps its data where it was
    CHECK_FALSE(fs::exists(app / "portable"));
    fs::remove_all(root);
}

TEST_CASE("a zip that isn't an update unpacks to nothing", "[update_check]") {
    fs::path root = fs::temp_directory_path() / "genshin-overlay-tests" / "not-an-update";
    fs::remove_all(root);
    CHECK(UpdateCheck::Unpack(data("SHA256SUMS.txt"), root).empty());
    CHECK(UpdateCheck::Unpack(data("missing.zip"), root).empty());
    fs::remove_all(root);
}
