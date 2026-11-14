#include "point_guides.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace PointGuides {
    static const std::string INFO = "https://sg-public-api-static.hoyolab.com/common/map_user/ys_obc/v1/map/point/info";
    static const std::string COMMENTS = "https://cache-v2.lemonapi.com/comments/v2";
    static const std::string COMMENT_PICTURES = "https://game-cdn.appsample.com";
    static const std::string GOOGLE = "https://translate.googleapis.com/translate_a/t";
    static const std::string MYMEMORY = "https://api.mymemory.translated.net/get";
    // HoYoLAB says a point has no guide with this
    static constexpr int NO_CONTENT = -2010;
    static constexpr size_t MAX_BODY = 2 * 1024 * 1024;
    static constexpr int DEADLINE_MS = 15000;

    const std::vector<std::string>& sites() {
        static const std::vector<std::string> names = { "sg-public-api-static.hoyolab.com", "cache-v2.lemonapi.com", "translate.googleapis.com",
            "api.mymemory.translated.net" };
        return names;
    }

    static Net::Limits limits() {
        Net::Limits limits;
        limits.publicOnly = true;
        limits.sites.only = true;
        limits.sites.names = sites();
        limits.maxBody = MAX_BODY;
        limits.deadlineMs = DEADLINE_MS;
        return limits;
    }

    std::function<bool(const std::string&, bool, Net::Response&, Net::Stopper*)> fetch =
        [](const std::string& url, bool hoyolab, Net::Response& out, Net::Stopper* stopper) {
            return Net::get(url, out, hoyolab, nullptr, stopper, limits());
        };
    std::function<std::chrono::steady_clock::time_point()> now = []() { return std::chrono::steady_clock::now(); };

    // "pt-BR" and "pt" are the same language here
    static std::string base(const std::string& code) {
        std::string out = code.substr(0, code.find('-'));
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return out;
    }

    static std::string encode(const std::string& text) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
            else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
        return out;
    }

    // HoYoLAB wants a language with its country
    static std::string hoyolabLanguage(const std::string& language) {
        static const std::map<std::string, std::string> names = {
            { "en", "en-us" }, { "zh", "zh-cn" }, { "ja", "ja-jp" }, { "ko", "ko-kr" }, { "es", "es-es" }, { "pt", "pt-pt" },
            { "fr", "fr-fr" }, { "de", "de-de" }, { "ru", "ru-ru" }, { "id", "id-id" }, { "th", "th-th" }, { "vi", "vi-vn" },
            { "it", "it-it" }, { "tr", "tr-tr" },
        };
        auto found = names.find(base(language));
        return found == names.end() ? "en-us" : found->second;
    }

    std::string languageName(const std::string& code) {
        static const std::map<std::string, std::string> names = {
            { "ar", "Arabic" }, { "cs", "Czech" }, { "da", "Danish" }, { "de", "German" }, { "el", "Greek" }, { "en", "English" },
            { "es", "Spanish" }, { "fi", "Finnish" }, { "fil", "Filipino" }, { "fr", "French" }, { "hu", "Hungarian" },
            { "id", "Indonesian" }, { "it", "Italian" }, { "ja", "Japanese" }, { "ko", "Korean" }, { "ms", "Malay" },
            { "nb", "Norwegian" }, { "nl", "Dutch" }, { "no", "Norwegian" }, { "pl", "Polish" }, { "pt", "Portuguese" },
            { "ro", "Romanian" }, { "ru", "Russian" }, { "sv", "Swedish" }, { "th", "Thai" }, { "tl", "Filipino" },
            { "tr", "Turkish" }, { "uk", "Ukrainian" }, { "vi", "Vietnamese" }, { "zh", "Chinese" },
        };
        auto found = names.find(base(code));
        return found == names.end() ? code : found->second;
    }

    std::string infoUrl(const Point& point, const std::string& language) {
        return INFO + "?map_id=" + std::to_string(point.map) + "&point_id=" + std::to_string(point.id) + "&app_sn=ys_obc&lang=" + hoyolabLanguage(language);
    }

    // appsample's types are HoYoLAB's with an o in front
    std::string commentsUrl(const Point& point) {
        return COMMENTS + "?app=gim&ttl=7000&collection=o" + std::to_string(point.type) + "&docId=" + std::to_string(point.appsample)
            + "&sort=&page=1&pageSize=" + std::to_string(MaxComments);
    }

    std::string googleUrl(const std::vector<std::string>& texts, const std::string& language) {
        std::string url = GOOGLE + "?client=gtx&sl=auto&tl=" + encode(base(language));
        for (const std::string& text : texts) url += "&q=" + encode(text);
        return url;
    }

    std::string myMemoryUrl(const std::string& text, const std::string& from, const std::string& language) {
        return MYMEMORY + "?q=" + encode(text) + "&langpair=" + encode(base(from)) + "%7C" + encode(base(language));
    }

    // a text field, empty when it's missing or null
    static std::string text(const json& object, const char* key) {
        if (!object.is_object()) return "";
        auto found = object.find(key);
        return found != object.end() && found->is_string() ? found->get<std::string>() : "";
    }

    bool readInfo(const std::string& body, Guide& out) {
        json answer = json::parse(body, nullptr, false);
        if (!answer.is_object() || !answer.contains("retcode") || !answer["retcode"].is_number_integer()) return false;
        int code = answer["retcode"].get<int>();
        if (code == NO_CONTENT) return true;
        if (code != 0 || !answer.contains("data") || !answer["data"].is_object()) return false;
        const json& data = answer["data"];
        if (!data.contains("info")) return true;
        out.text = text(data["info"], "content");
        out.picture = text(data["info"], "img");
        return true;
    }

    bool readComments(const std::string& body, const std::string& language, Guide& out) {
        json answer = json::parse(body, nullptr, false);
        if (!answer.is_object() || !answer.contains("data") || !answer["data"].is_object()) return false;
        const json& data = answer["data"];
        if (!data.contains("comments") || !data["comments"].is_array()) return false;
        out.summary = data.contains("summary") ? text(data["summary"], "content") : "";

        std::vector<Comment> comments;
        for (const json& entry : data["comments"]) {
            if (!entry.is_object()) continue;
            Comment comment;
            comment.author = text(entry, "aname");
            comment.date = text(entry, "time").substr(0, 10);
            comment.original = text(entry, "content");
            comment.text = comment.original;
            std::string picture = text(entry, "image");
            if (!picture.empty()) comment.picture = picture[0] == '/' ? COMMENT_PICTURES + picture : picture;
            if (entry.contains("vote") && entry["vote"].is_number_integer()) comment.likes = entry["vote"].get<int>();
            if (comment.original.empty() && comment.picture.empty()) continue;

            comment.language = text(entry, "lang");
            std::string translation = entry.contains("trans") ? text(entry["trans"], base(language).c_str()) : "";
            if (comment.original.empty() || base(comment.language) == base(language)) {}
            else if (!translation.empty()) {
                comment.text = translation;
                comment.from = languageName(comment.language);
            }
            // with no language said, Google tells
            else comment.translating = comment.original.size() <= MaxTranslated;
            comments.push_back(std::move(comment));
        }
        std::stable_sort(comments.begin(), comments.end(), [](const Comment& a, const Comment& b) { return a.likes > b.likes; });
        out.comments = std::move(comments);
        return true;
    }

    // [["text", "es"], ...], one for each text asked, in order
    bool readGoogle(const std::string& body, size_t count, std::vector<Translated>& out) {
        json answer = json::parse(body, nullptr, false);
        if (!answer.is_array() || answer.size() != count) return false;
        std::vector<Translated> read;
        for (const json& each : answer) {
            if (!each.is_array() || each.size() < 2 || !each[0].is_string() || !each[1].is_string()) return false;
            read.push_back({ each[0].get<std::string>(), each[1].get<std::string>() });
        }
        out = std::move(read);
        return true;
    }

    bool readMyMemory(const std::string& body, std::string& out) {
        json answer = json::parse(body, nullptr, false);
        if (!answer.is_object() || !answer.contains("responseStatus")) return false;
        const json& status = answer["responseStatus"];
        bool ok = (status.is_number_integer() && status.get<int>() == 200) || (status.is_string() && status.get<std::string>() == "200");
        if (!ok || (answer.contains("quotaFinished") && answer["quotaFinished"] == true)) return false;
        std::string translated = answer.contains("responseData") ? text(answer["responseData"], "translatedText") : "";
        // its errors come back as the translation
        if (translated.empty() || translated.rfind("MYMEMORY WARNING", 0) == 0) return false;
        out = translated;
        return true;
    }

    typedef std::pair<int64_t, std::string> Key;  // point, language

    struct Entry {
        Point point;
        Guide guide;
        bool have = false;      // an answer is in
        bool loading = false;
        bool complete = false;  // both sites answered
        std::chrono::steady_clock::time_point fetched, asked;
        // which answer the translations go into
        uint64_t serial = 0;
    };

    struct Job {
        Key key;
        bool translate = false;
        uint64_t serial = 0;
        std::vector<size_t> comments;  // to translate, by place in the guide
    };

    static std::mutex mutex;
    static std::condition_variable wake;
    static std::map<Key, Entry> entries;
    static std::deque<Job> jobs;
    static std::thread worker;
    static std::unique_ptr<Net::Stopper> stopper;
    static bool stopping = false;
    static uint64_t nextSerial = 1;

    static void keepTranslations(const std::vector<Comment>& before, std::vector<Comment>& comments) {
        std::map<std::string, const Comment*> done;
        for (const Comment& comment : before) {
            if (!comment.translating) done[comment.original] = &comment;
        }
        for (Comment& comment : comments) {
            auto found = done.find(comment.original);
            if (!comment.translating || found == done.end()) continue;
            comment.text = found->second->text;
            comment.from = found->second->from;
            comment.translating = false;
        }
    }

    static bool ask(const std::string& url, bool hoyolab, std::string& body) {
        Net::Response response;
        if (!fetch(url, hoyolab, response, stopper.get()) || response.status != 200) return false;
        body = std::move(response.body);
        return true;
    }

    static void fetchGuide(const Job& job) {
        Point point;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = entries.find(job.key);
            if (found == entries.end()) return;
            point = found->second.point;
        }
        const std::string& language = job.key.second;
        Guide guide;
        std::string body;
        bool info = ask(infoUrl(point, language), true, body) && readInfo(body, guide);
        bool paired = point.appsample != 0;
        bool comments = paired && ask(commentsUrl(point), false, body) && readComments(body, language, guide);
        // nothing came back from any site asked
        guide.failed = !info && !comments;

        std::lock_guard<std::mutex> lock(mutex);
        auto found = entries.find(job.key);
        if (found == entries.end()) return;
        Entry& entry = found->second;
        entry.loading = false;
        entry.complete = info && (comments || !paired);
        entry.fetched = now();
        // a part that didn't come this time is kept from before, and comments
        // that came again keep their translation
        bool keepComments = entry.have && paired && !comments;
        if (entry.have) {
            if (!info) {
                guide.text = entry.guide.text;
                guide.picture = entry.guide.picture;
            }
            if (keepComments) {
                guide.comments = std::move(entry.guide.comments);
                guide.summary = entry.guide.summary;
            }
            else keepTranslations(entry.guide.comments, guide.comments);
            guide.failed = guide.failed && entry.guide.failed;
        }
        entry.guide = std::move(guide);
        entry.have = true;
        // translations on their way still go into the kept comments
        if (keepComments) return;
        entry.serial = nextSerial++;
        // as few Google requests as fit
        Job batch{ job.key, true, entry.serial };
        size_t length = 0;
        for (size_t i = 0; i < entry.guide.comments.size(); i++) {
            const Comment& comment = entry.guide.comments[i];
            if (!comment.translating) continue;
            if (!batch.comments.empty() && length + comment.original.size() > MaxPerRequest) {
                jobs.push_back(batch);
                batch.comments.clear();
                length = 0;
            }
            batch.comments.push_back(i);
            length += comment.original.size();
        }
        if (!batch.comments.empty()) jobs.push_back(batch);
    }

    // Google for all of them, else MyMemory one by one where appsample said the
    // language; one left as it was written gets an empty from
    static void translate(const std::vector<std::string>& originals, const std::vector<std::string>& written, const std::string& language,
        std::vector<Translated>& out) {
        out.assign(originals.size(), {});
        for (size_t i = 0; i < originals.size(); i++) out[i].text = originals[i];
        std::string body;
        std::vector<Translated> google;
        if (ask(googleUrl(originals, language), false, body) && readGoogle(body, originals.size(), google)) {
            for (size_t i = 0; i < originals.size(); i++) {
                if (base(google[i].from) == base(language) || google[i].text.empty()) continue;
                out[i] = { google[i].text, languageName(google[i].from) };
            }
            return;
        }
        for (size_t i = 0; i < originals.size(); i++) {
            if (written[i].empty() || base(written[i]) == base(language)) continue;
            std::string text;
            if (ask(myMemoryUrl(originals[i], written[i], language), false, body) && readMyMemory(body, text)) out[i] = { text, languageName(written[i]) };
        }
    }

    static void translateComments(const Job& job) {
        std::vector<std::string> originals, written;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = entries.find(job.key);
            if (found == entries.end() || found->second.serial != job.serial) return;
            for (size_t i : job.comments) {
                originals.push_back(found->second.guide.comments[i].original);
                written.push_back(found->second.guide.comments[i].language);
            }
        }
        std::vector<Translated> translated;
        translate(originals, written, job.key.second, translated);

        std::lock_guard<std::mutex> lock(mutex);
        auto found = entries.find(job.key);
        if (found == entries.end() || found->second.serial != job.serial) return;
        for (size_t n = 0; n < job.comments.size(); n++) {
            Comment& comment = found->second.guide.comments[job.comments[n]];
            comment.translating = false;
            comment.text = translated[n].text;
            comment.from = translated[n].from;
        }
    }

    static void work() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [] { return stopping || !jobs.empty(); });
                if (stopping) return;
                // guides before translations
                auto first = std::find_if(jobs.begin(), jobs.end(), [](const Job& each) { return !each.translate; });
                if (first == jobs.end()) first = jobs.begin();
                job = *first;
                jobs.erase(first);
            }
            if (job.translate) translateComments(job);
            else fetchGuide(job);
        }
    }

    // the one asked longest ago goes, never the one just asked
    static void keepFew(const Key& keep) {
        while (entries.size() > Kept) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->first != keep && (oldest == entries.end() || it->second.asked < oldest->second.asked)) oldest = it;
            }
            if (oldest == entries.end()) return;
            Key gone = oldest->first;
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const Job& job) { return job.key == gone; }), jobs.end());
            entries.erase(oldest);
        }
    }

    bool get(const Point& point, const std::string& language, Guide& out) {
        if (point.id == 0) return false;
        Key key(point.id, language);
        bool have = false, refresh = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return false;
            if (!worker.joinable()) {
                stopper = std::make_unique<Net::Stopper>();
                worker = std::thread(work);
            }
            auto now = PointGuides::now();
            auto found = entries.find(key);
            if (found == entries.end()) {
                Entry& entry = entries[key];
                entry.point = point;
                entry.loading = true;
                entry.asked = now;
                jobs.push_back({ key });
                keepFew(key);
            }
            else {
                Entry& entry = found->second;
                entry.asked = now;
                entry.point = point;
                if (entry.have && !entry.loading) {
                    auto age = now - entry.fetched;
                    if (age >= FreshFor || (!entry.complete && age >= RetryAfter)) {
                        entry.loading = true;
                        jobs.push_back({ key });
                        refresh = true;
                    }
                }
                have = entry.have;
                if (have) out = entry.guide;
                if (!refresh) return have;
            }
        }
        wake.notify_one();
        return have;
    }

    size_t queued() {
        std::lock_guard<std::mutex> lock(mutex);
        return jobs.size();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            if (stopper) stopper->stop();
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
        jobs.clear();
        stopper.reset();
        stopping = false;
    }
}
