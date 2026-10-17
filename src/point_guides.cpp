#include "point_guides.hpp"
#include "helpers.hpp"
#include "net.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace PointGuides {
    static const std::string INFO = "https://sg-public-api-static.hoyolab.com/common/map_user/ys_obc/v1/map/point/info";
    static const std::string COMMENTS = "https://cache-v2.lemonapi.com/comments/v2";
    static const std::string COMMENT_PICTURES = "https://game-cdn.appsample.com";
    static const std::wstring APPSAMPLE_HEADERS =
        L"Origin: https://genshin-impact-map.appsample.com\r\nReferer: https://genshin-impact-map.appsample.com/\r\n";
    static constexpr int MAX_COMMENTS = 30;
    static constexpr size_t KEPT = 64;
    // a guide that failed is asked again after this long
    static constexpr auto RETRY_AFTER = std::chrono::seconds(10);

    typedef std::pair<int, int64_t> Key;  // map, point

    struct Job {
        Key key;
        int typeId;
    };

    static std::mutex mutex;
    static std::condition_variable wake;
    static std::deque<Job> queue;
    static std::set<Key> pending;
    static std::map<Key, Guide> guides;
    static std::map<Key, std::chrono::steady_clock::time_point> failedAt;
    static std::deque<Key> order;
    static bool stopping = false;
    static std::thread worker;

    // worker only
    static std::map<int64_t, int64_t> pairs;
    static bool pairsLoaded = false;

    // the item files can arrive after start, so it's read on the first tap that finds it
    static void loadPairs() {
        if (pairsLoaded) return;
        std::ifstream file(Helpers::exeDirectory() / "items" / "appsample.json", std::ios::binary);
        if (!file) return;
        json data = json::parse(file, nullptr, false);
        if (!data.is_object()) return;
        for (const auto& [ours, theirs] : data.items()) {
            if (theirs.is_number_integer()) pairs[std::stoll(ours)] = theirs.get<int64_t>();
        }
        pairsLoaded = true;
    }

    static bool fetchHoyolab(const Job& job, Guide& guide) {
        Net::Response response;
        std::string url = INFO + "?map_id=" + std::to_string(job.key.first) + "&point_id=" + std::to_string(job.key.second) + "&app_sn=ys_obc&lang=en-us";
        if (!Net::get(url, response, true) || response.status != 200) return false;
        json body = json::parse(response.body, nullptr, false);
        if (body.is_discarded() || body.value("retcode", -1) != 0) return false;
        const json& info = body["data"]["info"];
        if (!info.is_object()) return true;
        guide.text = info.value("content", "");
        guide.picture = info.value("img", "");
        return true;
    }

    static bool fetchComments(const Job& job, Guide& guide) {
        loadPairs();
        auto it = pairs.find(job.key.second);
        if (it == pairs.end()) return true;

        // appsample's types are HoYoLAB's with an o in front
        std::string url = COMMENTS + "?app=gim&ttl=7000&collection=o" + std::to_string(job.typeId) + "&docId=" + std::to_string(it->second)
            + "&sort=&page=1&pageSize=" + std::to_string(MAX_COMMENTS);
        Net::Response response;
        if (!Net::getWithHeaders(url, response, APPSAMPLE_HEADERS) || response.status != 200) return false;
        json body = json::parse(response.body, nullptr, false);
        if (body.is_discarded() || !body.contains("data") || !body["data"].contains("comments") || !body["data"]["comments"].is_array()) return false;

        for (const json& entry : body["data"]["comments"]) {
            if (!entry.is_object()) continue;
            Comment comment;
            comment.author = entry.value("aname", "");
            comment.date = entry.value("time", "").substr(0, 10);
            comment.text = entry.value("content", "");
            // written in another language, the English one if there is one
            if (entry.value("lang", "en") != "en" && entry.contains("trans") && entry["trans"].is_object()) {
                comment.text = entry["trans"].value("en", comment.text);
            }
            std::string picture = entry.value("image", "");
            if (!picture.empty()) comment.picture = COMMENT_PICTURES + picture;
            comment.likes = entry.value("vote", 0);
            if (!comment.text.empty() || !comment.picture.empty()) guide.comments.push_back(std::move(comment));
        }
        return true;
    }

    static void work() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [] { return stopping || !queue.empty(); });
                if (stopping) return;
                job = queue.front();
                queue.pop_front();
            }

            // a field that comes back null makes json throw
            Guide guide;
            bool hoyolab = false, comments = false;
            try { hoyolab = fetchHoyolab(job, guide); }
            catch (const json::exception&) {}
            try { comments = fetchComments(job, guide); }
            catch (const json::exception&) {}
            guide.failed = !hoyolab && !comments;

            std::lock_guard<std::mutex> lock(mutex);
            pending.erase(job.key);
            if (guide.failed) failedAt[job.key] = std::chrono::steady_clock::now();
            guides[job.key] = std::move(guide);
            order.push_back(job.key);
            while (order.size() > KEPT) {
                guides.erase(order.front());
                failedAt.erase(order.front());
                order.pop_front();
            }
        }
    }

    void Init() {
        stopping = false;
        worker = std::thread(work);
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    bool Get(int mapId, int64_t pointId, int typeId, Guide& out) {
        Key key(mapId, pointId);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = guides.find(key);
            auto failed = failedAt.find(key);
            if (it != guides.end() && failed != failedAt.end() && std::chrono::steady_clock::now() - failed->second > RETRY_AFTER) {
                guides.erase(it);
                failedAt.erase(failed);
                it = guides.end();
            }
            if (it != guides.end()) {
                out = it->second;
                return true;
            }
            if (!pending.insert(key).second) return false;
            queue.push_back({ key, typeId });
        }
        wake.notify_one();
        return false;
    }
}
