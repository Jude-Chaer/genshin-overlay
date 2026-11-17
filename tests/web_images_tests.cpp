#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "web_images.hpp"

// The web image store, with no GL: textures are numbers handed out by a
// stand in, downloads a real picture (tests/data/map_screen/custom_pin.png,
// 640 x 955) or a failure, and the clock moves only when a test moves it.

namespace fs = std::filesystem;

namespace {
    std::string picture() {
        std::ifstream in(fs::path(__FILE__).parent_path() / "data" / "map_screen" / "custom_pin.png", std::ios::binary);
        std::stringstream bytes;
        bytes << in.rdbuf();
        return bytes.str();
    }

    struct Store {
        GLuint next = 100;
        // what make gives, 0 for the next number
        GLuint reuse = 0;
        std::vector<GLuint> freed;
        std::atomic<int> fetched{ 0 };
        std::atomic<bool> fails{ false };
        std::string bytes = picture();
        decltype(WebImages::fetch) realFetch = WebImages::fetch;
        decltype(WebImages::make) realMake = WebImages::make;
        decltype(WebImages::release) realRelease = WebImages::release;
        decltype(WebImages::now) realNow = WebImages::now;
        std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::now();

        explicit Store(bool fake = true) {
            WebImages::clear();
            if (fake) {
                WebImages::fetch = [this](const std::string&, Net::Response& out, Net::Stopper*, const Net::Limits&) {
                    fetched++;
                    if (fails) return false;
                    out.status = 200;
                    out.body = bytes;
                    return true;
                };
            }
            WebImages::make = [this](const Helpers::Pixels& pixels) { return pixels.rgba.empty() ? 0 : (reuse ? reuse : next++); };
            WebImages::release = [this](GLuint texture) { freed.push_back(texture); };
            WebImages::now = [this] { return clock; };
        }
        // one frame after time went by
        void after(std::chrono::seconds time) {
            clock += time;
            WebImages::endFrame();
        }
        ~Store() {
            WebImages::clear();
            WebImages::fetch = realFetch;
            WebImages::make = realMake;
            WebImages::release = realRelease;
            WebImages::now = realNow;
        }
    };

    const int owner = 0, other = 0;

    // frames, asking each one, until it isn't loading or 5 s
    WebImages::Image askUntilDone(const void* who, const std::string& url, const Net::Limits& limits = {},
        const std::function<void(const WebImages::Done&)>& done = nullptr) {
        auto started = std::chrono::steady_clock::now();
        WebImages::Image image;
        while (std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            image = WebImages::get(who, url, limits);
            if (image.state != WebImages::State::Loading) return image;
            WebImages::endFrame(done);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return image;
    }
}

TEST_CASE("a web picture is shrunk to 640 and given as a handle that is never a GL id") {
    Store store;
    const std::string url = "https://example.com/pin.png";
    CHECK(WebImages::get(&owner, url, {}).state == WebImages::State::Loading);
    WebImages::Image image = askUntilDone(&owner, url);
    REQUIRE(image.state == WebImages::State::Ready);
    // 640 x 955 times 640 / 955
    CHECK(image.width == 428);
    CHECK(image.height == 640);
    CHECK(WebImages::isHandle(image.handle));
    CHECK_FALSE(WebImages::isHandle(4294967295.0));
    CHECK(WebImages::texture(&owner, image.handle) == 100);
    // another extension's handle draws nothing
    CHECK(WebImages::texture(&other, image.handle) == 0);
    CHECK(store.fetched == 1);
}

TEST_CASE("a freed picture's handle draws nothing, even when GL gives its id to the next one") {
    Store store;
    const std::string url = "https://example.com/pin.png";
    WebImages::Image first = askUntilDone(&owner, url);
    REQUIRE(first.state == WebImages::State::Ready);

    // drawing counts as asking
    store.after(std::chrono::seconds(20));
    CHECK(WebImages::texture(&owner, first.handle) == 100);
    store.after(std::chrono::seconds(20));
    CHECK(WebImages::texture(&owner, first.handle) == 100);
    CHECK(store.freed.empty());

    store.after(WebImages::FreeAfter + std::chrono::seconds(1));
    CHECK(WebImages::texture(&owner, first.handle) == 0);
    REQUIRE(store.freed.size() == 1);
    CHECK(store.freed[0] == 100);
    CHECK(WebImages::kept(&owner) == 0);

    store.reuse = 100;
    WebImages::Image second = askUntilDone(&owner, url);
    REQUIRE(second.state == WebImages::State::Ready);
    CHECK(second.handle != first.handle);
    CHECK(WebImages::texture(&owner, first.handle) == 0);
    CHECK(WebImages::texture(&owner, second.handle) == 100);
    CHECK(store.fetched == 2);
}

TEST_CASE("a picture that failed is tried again only once it's freed") {
    Store store;
    store.fails = true;
    const std::string url = "https://example.com/pin.png";
    CHECK(askUntilDone(&owner, url).state == WebImages::State::Failed);
    for (int i = 0; i < 400; i++) {
        CHECK(WebImages::get(&owner, url, {}).state == WebImages::State::Failed);
        WebImages::endFrame();
    }
    CHECK(store.fetched == 1);

    store.fails = false;
    store.after(WebImages::FreeAfter + std::chrono::seconds(1));
    CHECK(askUntilDone(&owner, url).state == WebImages::State::Ready);
    CHECK(store.fetched == 2);
}

TEST_CASE("a picture past the downloads at once is loading, not failed") {
    Store store;
    for (int i = 0; i < WebImages::MaxWaiting; i++) WebImages::get(&owner, "https://example.com/" + std::to_string(i) + ".png", {});
    CHECK(WebImages::get(&owner, "https://example.com/last.png", {}).state == WebImages::State::Loading);
    CHECK(askUntilDone(&owner, "https://example.com/last.png").state == WebImages::State::Ready);
}

TEST_CASE("forgetting an extension frees its pictures and keeps the others'") {
    Store store;
    REQUIRE(askUntilDone(&owner, "https://example.com/a.png").state == WebImages::State::Ready);
    WebImages::Image kept = askUntilDone(&other, "https://example.com/a.png");
    REQUIRE(kept.state == WebImages::State::Ready);
    // each extension downloads its own
    CHECK(store.fetched == 2);
    WebImages::forget(&owner);
    CHECK(WebImages::kept(&owner) == 0);
    CHECK(store.freed.size() == 1);
    CHECK(WebImages::texture(&other, kept.handle) != 0);
}

TEST_CASE("a picture on a host off the extension's sites is refused before anything is sent") {
    Store store(false);
    Net::Limits limits;
    limits.publicOnly = true;
    limits.sites = Net::Sites{ true, { "hoyoverse.com" } };
    bool told = false;
    WebImages::Image image = askUntilDone(&owner, "https://example.com/pin.png", limits, [&](const WebImages::Done& done) {
        told = true;
        CHECK_FALSE(done.ok);
        CHECK(done.response.notListed);
        CHECK(done.response.refused == "example.com");
    });
    CHECK(image.state == WebImages::State::Failed);
    CHECK(told);
}
