#include "update_check.hpp"
#include "helpers.hpp"
#include "net.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <thread>
#include <vector>

namespace UpdateCheck {
    static const std::string RELEASES = "https://api.github.com/repos/PocketEsoptron/PocketEsoptron/releases?per_page=30";

    static std::thread worker;
    static std::mutex lock;
    static std::condition_variable wake;
    static bool stopping = false;
    static std::unique_ptr<Net::Stopper> stopper;
    static Release found;

    struct Version {
        int numbers[3] = {};
        std::vector<std::string> pre;
    };

    static std::optional<Version> parse(const std::string& text) {
        static const std::regex shape(R"(v(\d{1,6})\.(\d{1,6})\.(\d{1,6})(?:-([0-9A-Za-z.-]+))?)");
        std::smatch match;
        if (!std::regex_match(text, match, shape)) return std::nullopt;
        Version version;
        for (int i = 0; i < 3; i++) version.numbers[i] = std::stoi(match[i + 1]);
        std::string pre = match[4];
        size_t start = 0;
        while (!pre.empty()) {
            size_t dot = pre.find('.', start);
            version.pre.push_back(pre.substr(start, dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return version;
    }

    static bool allDigits(const std::string& part) {
        return !part.empty() && part.find_first_not_of("0123456789") == std::string::npos;
    }

    // -1, 0 or 1; numbers below words, numbers by value
    static int comparePart(const std::string& a, const std::string& b) {
        bool aNumber = allDigits(a), bNumber = allDigits(b);
        if (aNumber && bNumber) {
            if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
            return a == b ? 0 : (a < b ? -1 : 1);
        }
        if (aNumber != bNumber) return aNumber ? -1 : 1;
        return a == b ? 0 : (a < b ? -1 : 1);
    }

    bool Later(const std::string& a, const std::string& b) {
        std::optional<Version> first = parse(a), second = parse(b);
        if (!first || !second) return false;
        for (int i = 0; i < 3; i++) {
            if (first->numbers[i] != second->numbers[i]) return first->numbers[i] > second->numbers[i];
        }
        // a release is later than its own pre-releases
        if (first->pre.empty() || second->pre.empty()) return first->pre.empty() && !second->pre.empty();
        size_t shared = std::min(first->pre.size(), second->pre.size());
        for (size_t i = 0; i < shared; i++) {
            int order = comparePart(first->pre[i], second->pre[i]);
            if (order != 0) return order > 0;
        }
        return first->pre.size() > second->pre.size();
    }

    Release Pick(const std::string& releases, const std::string& current) {
        Release best;
        nlohmann::json list = nlohmann::json::parse(releases, nullptr, false);
        if (!list.is_array()) return best;
        bool currentPre = current.find('-') != std::string::npos;
        for (const nlohmann::json& release : list) {
            if (!release.is_object() || release.value("draft", false)) continue;
            if (release.value("prerelease", false) && !currentPre) continue;
            auto tag = release.find("tag_name");
            auto page = release.find("html_url");
            if (tag == release.end() || !tag->is_string() || page == release.end() || !page->is_string()) continue;
            std::string version = tag->get<std::string>();
            // only release pages on github.com
            if (page->get<std::string>().rfind("https://github.com/", 0) != 0) continue;
            if (Later(version, best.version.empty() ? current : best.version)) best = { version, page->get<std::string>() };
        }
        return best;
    }

    static void ask() {
        Net::Response response;
        Net::Limits limits;
        limits.maxBody = 2 * 1024 * 1024;
        limits.deadlineMs = 30000;
        if (!Net::get(RELEASES, response, false, nullptr, stopper.get(), limits) || response.status != 200) {
            if (!stopper->stopped()) std::cerr << "Update check: couldn't reach GitHub (" << response.status << ")" << std::endl;
            return;
        }
        Release newer = Pick(response.body, Helpers::appVersion());
        if (!newer.version.empty()) std::cout << "Update check: " << newer.version << " is out" << std::endl;
        std::lock_guard<std::mutex> hold(lock);
        found = newer;
    }

    void Start() {
        if (Helpers::appVersion() == "dev" || worker.joinable()) return;
        stopping = false;
        stopper = std::make_unique<Net::Stopper>();
        worker = std::thread([]() {
            while (true) {
                ask();
                std::unique_lock<std::mutex> hold(lock);
                if (wake.wait_for(hold, std::chrono::hours(24), []() { return stopping; })) return;
            }
        });
    }

    void Stop() {
        if (!worker.joinable()) return;
        {
            std::lock_guard<std::mutex> hold(lock);
            stopping = true;
            found = {};
        }
        stopper->stop();
        wake.notify_all();
        worker.join();
    }

    Release Newer() {
        std::lock_guard<std::mutex> hold(lock);
        return found;
    }
}
