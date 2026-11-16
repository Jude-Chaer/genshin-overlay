#include "marks.hpp"
#include "asks.hpp"
#include "credentials.hpp"
#include "files.hpp"
#include "helpers.hpp"
#include "item_catalog.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace Marks {
    static const std::string HOYOLAB = "https://sg-public-api.hoyolab.com/common/map_user/ys_obc/v1/map/point/";
    static const std::string HOYOLAB_QUERY = "app_sn=ys_obc&lang=en-us";
    static const std::string HOYOLAB_HEADERS = "Origin: https://act.hoyolab.com\r\nReferer: https://act.hoyolab.com/\r\nx-rpc-map_version: 4.5\r\n";
    static const std::string NAME_URL = "https://bbs-api-os.hoyolab.com/community/user/wapi/getUserFullInfo?uid=";
    static const std::string NAME_HEADERS = "Origin: https://www.hoyolab.com\r\nReferer: https://www.hoyolab.com/\r\n";
    static const std::string FIRESTORE = "https://firestore.googleapis.com/v1/";
    static const std::string DOCUMENTS = "projects/genshin-impact/databases/(default)/documents";
    static const std::string USERS = DOCUMENTS + "/users/";
    // how many ids go in one write, and the wait between two
    static constexpr size_t BATCH = 50;
    static constexpr auto BATCH_GAP = std::chrono::milliseconds(300);
    // appsample takes a found mark only from this level up
    static constexpr int APPSAMPLE_LEVEL = 5;

    // ---- reading and writing the sites' answers ----

    HoyolabAnswer ReadHoyolab(const Net::Response& response) {
        HoyolabAnswer answer;
        json parsed = json::parse(response.body, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("retcode") || !parsed["retcode"].is_number_integer()) return answer;
        answer.read = true;
        answer.retcode = parsed["retcode"].get<int>();
        if (parsed.contains("message") && parsed["message"].is_string()) answer.message = parsed["message"].get<std::string>();
        return answer;
    }

    static bool loggedOut(int retcode) {
        return retcode == -100 || retcode == -2004 || retcode == 10001;
    }

    bool ReadHoyolabList(const Net::Response& response, std::vector<int64_t>& out) {
        json parsed = json::parse(response.body, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("retcode") || parsed["retcode"] != 0) return false;
        auto data = parsed.find("data");
        if (data == parsed.end() || !data->is_object()) return false;
        auto list = data->find("list");
        // a map with nothing marked can come back with no list
        if (list == data->end() || list->is_null()) return true;
        if (!list->is_array()) return false;
        for (const json& mark : *list) {
            if (mark.is_object() && mark.contains("point_id") && mark["point_id"].is_number_integer())
                out.push_back(mark["point_id"].get<int64_t>());
        }
        return true;
    }

    std::string HoyolabBatch(int map, const std::vector<int64_t>& ids, bool unmark) {
        json list = json::array();
        for (int64_t id : ids) list.push_back({ { "point_id", id }, { "is_delete", unmark } });
        return json{ { "map_id", map }, { "list", list } }.dump();
    }

    std::string ReadHoyolabName(const Net::Response& response) {
        json parsed = json::parse(response.body, nullptr, false);
        if (!parsed.is_object() || parsed.value("retcode", -1) != 0) return "";
        const json* at = &parsed;
        for (const char* key : { "data", "user_info", "nickname" }) {
            if (!at->is_object() || !at->contains(key)) return "";
            at = &(*at)[key];
        }
        if (!at->is_string()) return "";
        std::string name = at->get<std::string>();
        // a line on the Accounts page, nothing that breaks it
        name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return (unsigned char)c < 0x20; }), name.end());
        if (name.size() <= 64) return name;
        size_t end = 64;
        while (end > 0 && ((unsigned char)name[end] & 0xC0) == 0x80) end--;
        return name.substr(0, end);
    }

    // a whole number from a Firestore value, which keeps ids as strings
    static bool firestoreId(const json& value, int64_t& out) {
        if (!value.is_object()) return false;
        std::string text;
        for (const char* key : { "stringValue", "integerValue" }) {
            if (value.contains(key) && value[key].is_string()) text = value[key].get<std::string>();
        }
        if (text.empty() || text.size() > 18 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        out = std::stoll(text);
        return true;
    }

    bool ReadAppsampleUser(const Net::Response& response, std::vector<int64_t>& out) {
        if (response.status == 404) return true;
        if (response.status != 200) return false;
        json parsed = json::parse(response.body, nullptr, false);
        if (!parsed.is_object()) return false;
        auto fields = parsed.find("fields");
        if (fields == parsed.end()) return true;
        if (!fields->is_object()) return false;
        auto ids = fields->find("markerIds");
        if (ids == fields->end()) return true;
        if (!ids->is_object() || !ids->contains("arrayValue")) return false;
        const json& array = (*ids)["arrayValue"];
        if (!array.is_object()) return false;
        // an empty array has no values
        if (!array.contains("values")) return true;
        if (!array["values"].is_array()) return false;
        for (const json& value : array["values"]) {
            int64_t id = 0;
            if (firestoreId(value, id)) out.push_back(id);
        }
        return true;
    }

    // Firebase uids are letters and digits; anything else isn't put in a path
    static bool goodUid(const std::string& uid) {
        return !uid.empty() && uid.size() <= 128
            && std::all_of(uid.begin(), uid.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
    }

    std::string AppsampleCommit(const std::string& uid, const std::vector<int64_t>& ids, bool remove) {
        json values = json::array();
        for (int64_t id : ids) {
            values.push_back({ { "stringValue", std::to_string(id) } });
            // an id kept as a number goes too
            if (remove) values.push_back({ { "integerValue", std::to_string(id) } });
        }
        json transform = { { "fieldPath", "markerIds" }, { remove ? "removeAllFromArray" : "appendMissingElements", { { "values", values } } } };
        // an update with no fields and an empty mask touches only the
        // transform, and makes the document when it's not there yet
        json write = {
            { "update", { { "name", USERS + uid }, { "fields", json::object() } } },
            { "updateMask", { { "fieldPaths", json::array() } } },
            { "updateTransforms", json::array({ transform }) },
        };
        return json{ { "writes", json::array({ write }) } }.dump();
    }

    Plan PlanChange(const std::vector<int64_t>& marked, const std::vector<int64_t>& add, const std::vector<int64_t>& remove) {
        std::set<int64_t> there(marked.begin(), marked.end());
        std::set<int64_t> adding, removing;
        Plan plan;
        for (int64_t id : add) {
            if (!there.count(id) && adding.insert(id).second) plan.add.push_back(id);
        }
        // a point both added and removed stays marked
        std::set<int64_t> added(add.begin(), add.end());
        for (int64_t id : remove) {
            if (there.count(id) && !added.count(id) && removing.insert(id).second) plan.remove.push_back(id);
        }
        return plan;
    }

    // ---- backups ----

    std::filesystem::path BackupFolder() {
        return Helpers::exeDirectory() / "backups";
    }

    std::string AccountHash(Site site, const std::string& account) {
        return Files::sha256(std::string(Accounts::SiteName(site)) + ":" + account);
    }

    static std::string now() {
        std::time_t time = std::time(nullptr);
        std::tm local;
        localtime_s(&local, &time);
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &local);
        return text;
    }

    std::string SaveBackup(const std::filesystem::path& folder, Site site, const std::string& account, const std::vector<int64_t>& marks) {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        std::string first = std::string(Accounts::SiteName(site)) + "-" + now();
        std::string name = first;
        // a second one in the same second ends in _2, then _3
        for (int n = 2; std::filesystem::exists(folder / (name + ".json"), error); n++) name = first + "_" + std::to_string(n);
        std::vector<int64_t> sorted = marks;
        std::sort(sorted.begin(), sorted.end());
        json data = { { "site", Accounts::SiteName(site) }, { "account", AccountHash(site, account) }, { "count", sorted.size() }, { "marks", sorted } };
        std::ofstream file(folder / (name + ".json"), std::ios::binary | std::ios::trunc);
        if (!file) return "";
        file << data.dump(1);
        file.close();
        if (!file) return "";
        size_t kept = 0;
        for (const Backup& each : Backups(folder)) {
            if (each.site != site || each.account != data["account"].get<std::string>() || ++kept <= KeepBackups) continue;
            std::filesystem::remove(folder / (each.name + ".json"), error);
        }
        return name;
    }

    static bool readBackupFile(const std::filesystem::path& path, Backup& backup, std::vector<int64_t>* marks) {
        std::ifstream file(path, std::ios::binary);
        json data = json::parse(file, nullptr, false);
        if (!data.is_object() || !data["site"].is_string() || !data["account"].is_string() || !data["marks"].is_array()) return false;
        std::optional<Site> site = Accounts::SiteNamed(data["site"].get<std::string>());
        if (!site) return false;
        backup.name = path.stem().string();
        backup.site = *site;
        backup.account = data["account"].get<std::string>();
        backup.count = 0;
        for (const json& id : data["marks"]) {
            if (!id.is_number_integer()) continue;
            backup.count++;
            if (marks) marks->push_back(id.get<int64_t>());
        }
        return true;
    }

    std::vector<Backup> Backups(const std::filesystem::path& folder) {
        std::vector<Backup> found;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            Backup backup;
            if (entry.path().extension() == ".json" && readBackupFile(entry.path(), backup, nullptr)) found.push_back(backup);
        }
        // the name ends in the date and, for a second one in the same second,
        // _2, _3 ...: by date, then that number
        auto when = [](const Backup& b) {
            std::string date = b.name.substr(b.name.find('-') + 1);
            size_t cut = date.find('_', 11);
            int n = cut == std::string::npos ? 1 : std::atoi(date.c_str() + cut + 1);
            return std::make_pair(date.substr(0, cut), n);
        };
        std::sort(found.begin(), found.end(), [&](const Backup& a, const Backup& b) {
            auto aWhen = when(a), bWhen = when(b);
            return aWhen != bWhen ? aWhen > bWhen : a.name > b.name;
        });
        return found;
    }

    bool ReadBackup(const std::filesystem::path& folder, const std::string& name, Site site, const std::string& account,
        std::vector<int64_t>& out) {
        // only a name SaveBackup gives, never a path or a device like CON
        static const std::regex given("(hoyolab_map|appsample)-[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}(_[0-9]+)?");
        if (!std::regex_match(name, given)) return false;
        Backup backup;
        std::vector<int64_t> marks;
        if (!readBackupFile(folder / (name + ".json"), backup, &marks)) return false;
        if (backup.site != site || backup.account != AccountHash(site, account)) return false;
        out = std::move(marks);
        return true;
    }

    // ---- the jobs ----

    enum class Choice { None, Unmark, Keep, Cancel };

    struct Job {
        int id = 0;
        Request request;
        // each site's account when it was started; a job for an account
        // that's gone isn't run for the next one
        std::map<Site, std::string> accounts;
        // the overlay's own nickname read, not a result anyone takes
        bool name = false;
        // the player's answer to the summary question, once given, and the
        // points it was for
        Choice answer = Choice::None;
        std::set<int64_t> answered;
    };

    struct Done {
        Result result;
        const void* owner = nullptr;
        Clock::time_point at;
    };

    // the summary question, handed to Asks by Frame
    struct Question {
        int job = 0;
        const void* owner = nullptr;
        int count = 0;
        std::set<int64_t> points;
        std::vector<Asks::UnmarkLine> lines;
        bool handed = false;
    };

    // a job waiting for the player's answer; the worker goes on with other
    // owners' jobs, this owner's wait behind it
    struct Asking {
        Job job;
        Question question;
    };

    static std::mutex mutex;
    static std::condition_variable wake;
    static std::deque<Job> queue;
    static std::deque<Asking> asking;
    static std::map<int, Done> results;
    static int running = 0;
    static const void* runningOwner = nullptr;
    static std::vector<Site> runningSites;
    // the running job, when its account permission was switched off
    static int cancelling = 0;
    static int nextJob = 1;
    static bool stopping = false;
    static std::thread worker;
    static Net::Stopper* stopper = nullptr;
    // the account whose nickname was asked for last and when, frame thread
    // only; nameFailed when HoYoLAB didn't answer it
    static std::string nameAskedFor;
    static Clock::time_point nameAskedAt;
    static bool nameFailed = false;
    static constexpr auto NAME_AGAIN = std::chrono::minutes(5);

    static LoginFor loginFor;
    static Sender sender;
    static std::filesystem::path testFolder;

    static void wipe(Login& login) {
        Credentials::Wipe(login.headers);
        Credentials::Wipe(login.secret);
    }

    // the secret and the account are read one after the other; a log in
    // between gives nothing
    static std::optional<Login> sameLogin(Site site, Login& login) {
        if (Accounts::AccountId(site) == login.account) return login;
        wipe(login);
        return std::nullopt;
    }

    static std::optional<Login> realLogin(Site site) {
        std::string account = Accounts::AccountId(site);
        if (account.empty()) return std::nullopt;
        if (site == Site::HoyolabMap) {
            std::string cookie = Accounts::HoyolabMapCookie();
            if (cookie.empty()) return std::nullopt;
            Login login{ "Cookie: " + cookie + "\r\n", account, "" };
            size_t token = cookie.find("ltoken_v2=");
            if (token != std::string::npos) login.secret = cookie.substr(token + 10);
            Credentials::Wipe(cookie);
            return sameLogin(site, login);
        }
        std::optional<std::string> token = Accounts::AppsampleIdToken(stopper);
        if (!token) return std::nullopt;
        Login login{ "Authorization: Bearer " + *token + "\r\n", account, *token };
        Credentials::Wipe(*token);
        return sameLogin(site, login);
    }

    static bool realSend(const std::string& method, const std::string& url, const std::string& headers, const std::string& body, Net::Response& out) {
        Net::Limits limits;
        limits.publicOnly = true;
        limits.sites = { true, { "hoyolab.com", "googleapis.com" } };
        limits.maxBody = 16 * 1024 * 1024;
        limits.deadlineMs = 30000;
        return Net::sendLogin(method, url, headers, body, out, limits, stopper);
    }

    static std::optional<Login> currentLogin(Site site) {
        return loginFor ? loginFor(site) : realLogin(site);
    }

    static bool send(const std::string& method, const std::string& url, const Login& login, const std::string& more, const std::string& body,
        Net::Response& out) {
        std::string headers = login.headers + more;
        bool reached = sender ? sender(method, url, headers, body, out) : realSend(method, url, headers, body, out);
        Credentials::Wipe(headers);
        return reached;
    }

    static std::filesystem::path backupFolder() {
        return testFolder.empty() ? BackupFolder() : testFolder;
    }

    // the overlay is closing, the running job's asker went or it was cancelled
    static bool stopped() {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping || running < 0 || (running > 0 && running == cancelling);
    }

    // every site of the job ends as "cancelled"
    static Result cancelled(const Job& job) {
        Result result;
        for (Site site : job.request.sites) {
            SiteResult part;
            part.site = site;
            part.error = "cancelled";
            part.why = "the account permission was switched off";
            result.sites.push_back(std::move(part));
        }
        return result;
    }

    // one site's part of a job, between reading what's marked and writing
    struct Work {
        SiteResult result;
        Login login;
        std::vector<int64_t> marked;      // the site's own ids
        std::map<int64_t, int> mapOf;     // HoYoLAB: the map each mark came on
        Plan plan;                        // the site's own ids
        std::map<int64_t, int64_t> ours;  // appsample id to HoYoLAB id, for the result
        bool read = false;
    };

    static void failed(SiteResult& result, const std::string& error, const std::string& why) {
        result.ok = false;
        result.error = error;
        result.why = why;
    }

    // a HoYoLAB answer that isn't retcode 0: what it means for the login
    static void hoyolabRefused(Work& work, const Net::Response& response, bool reached) {
        HoyolabAnswer answer = ReadHoyolab(response);
        if (!reached || !answer.read) {
            failed(work.result, "failed", reached ? "HoYoLAB answered " + std::to_string(response.status) : "HoYoLAB couldn't be reached");
            return;
        }
        if (loggedOut(answer.retcode)) {
            Accounts::Refused(Site::HoyolabMap, work.login.secret);
            failed(work.result, "logged_out", "HoYoLAB said: " + answer.message);
        }
        else if (answer.retcode == -5001) {
            Accounts::CaptchaAsked(Site::HoyolabMap);
            failed(work.result, "captcha", "HoYoLAB wants a captcha first");
        }
        else failed(work.result, "failed", "HoYoLAB said: " + answer.message + " (" + std::to_string(answer.retcode) + ")");
    }

    static bool readHoyolab(Work& work, const ItemCatalog::Catalog& catalog) {
        std::set<int> maps;
        for (const ItemCatalog::Point& point : catalog.allPoints()) maps.insert(point.map);
        for (int map : maps) {
            if (stopped()) {
                failed(work.result, "failed", "stopped before it was read");
                return false;
            }
            Net::Response response;
            bool reached = send("GET", HOYOLAB + "mark_map_point_list?map_id=" + std::to_string(map) + "&" + HOYOLAB_QUERY,
                work.login, HOYOLAB_HEADERS, "", response);
            std::vector<int64_t> ids;
            if (!reached || !ReadHoyolabList(response, ids)) {
                hoyolabRefused(work, response, reached);
                return false;
            }
            for (int64_t id : ids) {
                if (work.mapOf.emplace(id, map).second) work.marked.push_back(id);
            }
        }
        Accounts::Working(Site::HoyolabMap);
        return true;
    }

    // Sends with the job's appsample login. A turned down ID token is
    // refreshed and sent once more; false with the error set when the login
    // is gone or another account's by then.
    static bool sendAppsample(Work& work, const std::string& method, const std::string& url, const std::string& body, Net::Response& response) {
        bool reached = send(method, url, work.login, "", body, response);
        if (!reached || response.status != 401) return true;
        Accounts::AppsampleIdTokenRefused(work.login.secret);
        std::optional<Login> login = currentLogin(Site::Appsample);
        if (!login || login->account != work.login.account) {
            if (!login) failed(work.result, "logged_out", "appsample turned the login down");
            else {
                wipe(*login);
                failed(work.result, "account_changed", "logged in as another account meanwhile");
            }
            return false;
        }
        wipe(work.login);
        work.login = *login;
        wipe(*login);
        response = Net::Response();
        send(method, url, work.login, "", body, response);
        return true;
    }

    static void appsampleFailed(Work& work, const Net::Response& response) {
        failed(work.result, "failed", response.status ? "appsample answered " + std::to_string(response.status) : "appsample couldn't be reached");
    }

    static bool readAppsample(Work& work) {
        if (!goodUid(work.login.account)) {
            failed(work.result, "failed", "appsample's account id doesn't look like one");
            return false;
        }
        Net::Response response;
        if (!sendAppsample(work, "GET", FIRESTORE + USERS + work.login.account + "?mask.fieldPaths=markerIds", "", response)) return false;
        if (ReadAppsampleUser(response, work.marked)) return true;
        appsampleFailed(work, response);
        return false;
    }

    // a HoYoLAB point's id on the work's site, 0 when it can't go there
    static int64_t ownId(Work& work, const ItemCatalog::Catalog& catalog, int64_t id, bool adding) {
        const ItemCatalog::Point* point = catalog.point(id);
        if (!point) return 0;
        if (work.result.site == Site::HoyolabMap) return id;
        if (!point->appsample) return 0;
        // level 0 is unknown and is sent
        if (adding && point->appsampleLevel > 0 && point->appsampleLevel < APPSAMPLE_LEVEL) return 0;
        work.ours[point->appsample] = id;
        return point->appsample;
    }

    // what to send on a site, in its own ids; what can't go is skipped
    static void planSite(Work& work, const Request& request, const ItemCatalog::Catalog& catalog) {
        Site site = work.result.site;
        std::vector<int64_t> add, remove;
        auto own = [&](int64_t id, bool adding) { return ownId(work, catalog, id, adding); };
        for (int64_t id : request.add) {
            int64_t mine = own(id, true);
            if (mine) add.push_back(mine);
            else work.result.skipped.push_back(id);
        }
        for (int64_t id : request.remove) {
            // an unmark for a point not in the item files goes with the map it came on
            int64_t mine = site == Site::HoyolabMap && work.mapOf.count(id) ? id : own(id, false);
            if (mine) remove.push_back(mine);
            else if (site == Site::Appsample) work.result.skipped.push_back(id);
        }
        work.plan = PlanChange(work.marked, add, remove);
    }

    // A backup's own ids back on its site, nothing unmarked: appsample's all
    // of them, paired or not; HoYoLAB's only where the item files know the
    // point's map, the rest are skipped.
    static void planRestore(Work& work, const std::vector<int64_t>& saved, const ItemCatalog::Catalog& catalog) {
        std::vector<int64_t> add;
        for (int64_t id : saved) {
            if (work.result.site == Site::HoyolabMap && !catalog.point(id)) work.result.skipped.push_back(id);
            else add.push_back(id);
        }
        work.plan = PlanChange(work.marked, add, {});
    }

    // a site's own ids as HoYoLAB ids; appsample's without a HoYoLAB pair
    // are left out, they're never touched
    static std::vector<int64_t> hoyolabIds(Site site, const std::vector<int64_t>& own, const ItemCatalog::Catalog& catalog) {
        if (site == Site::HoyolabMap) return own;
        std::unordered_map<int64_t, int64_t> pairs;
        for (const ItemCatalog::Point& point : catalog.allPoints()) {
            if (point.appsample) pairs[point.appsample] = point.id;
        }
        std::vector<int64_t> out;
        for (int64_t id : own) {
            auto pair = pairs.find(id);
            if (pair != pairs.end()) out.push_back(pair->second);
        }
        return out;
    }

    static int64_t ourId(const Work& work, int64_t own) {
        if (work.result.site == Site::HoyolabMap) return own;
        auto it = work.ours.find(own);
        return it == work.ours.end() ? 0 : it->second;
    }

    static std::vector<Asks::UnmarkLine> questionLines(const std::vector<Work*>& works, const ItemCatalog::Catalog& catalog) {
        std::unordered_map<int, std::string> types;
        for (const ItemCatalog::TypeGroup& group : catalog.groups) {
            for (const ItemCatalog::Type& type : group.types) types[type.id] = type.name;
        }
        std::map<std::pair<int, int>, std::string> regions;
        for (const ItemCatalog::Region& region : catalog.regions) regions[{ region.map, region.id }] = region.name;

        std::vector<Asks::UnmarkLine> lines;
        for (const Work* work : works) {
            if (work->plan.remove.empty()) continue;
            Asks::UnmarkLine line;
            line.site = Accounts::SiteLabel(work->result.site);
            line.count = (int)work->plan.remove.size();
            std::map<std::string, int> counts;
            for (int64_t own : work->plan.remove) {
                const ItemCatalog::Point* point = catalog.point(ourId(*work, own));
                std::string name = "Not in the item files";
                if (point) {
                    auto region = regions.find({ point->map, point->area });
                    auto type = types.find(point->type);
                    name = (region != regions.end() ? region->second + ", " : "") + (type != types.end() ? type->second : "Other");
                }
                counts[name]++;
            }
            line.which.assign(counts.begin(), counts.end());
            std::sort(line.which.begin(), line.which.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
            lines.push_back(line);
        }
        return lines;
    }

    static bool sameAccount(Work& work) {
        std::optional<Login> login = currentLogin(work.result.site);
        bool same = login && login->account == work.login.account;
        if (same) {
            wipe(work.login);
            work.login = *login;
        }
        else failed(work.result, "account_changed", "logged out or in as another account meanwhile");
        if (login) wipe(*login);
        return same;
    }

    static bool writeHoyolab(Work& work, const ItemCatalog::Catalog& catalog) {
        std::map<int, std::vector<int64_t>> adds, removes;
        auto mapFor = [&](int64_t id) {
            auto it = work.mapOf.find(id);
            if (it != work.mapOf.end()) return it->second;
            const ItemCatalog::Point* point = catalog.point(id);
            return point ? (int)point->map : 0;
        };
        for (int64_t id : work.plan.add) adds[mapFor(id)].push_back(id);
        for (int64_t id : work.plan.remove) removes[mapFor(id)].push_back(id);
        bool first = true;
        for (bool unmark : { false, true }) {
            for (const auto& [map, ids] : unmark ? removes : adds) {
                for (size_t start = 0; start < ids.size(); start += BATCH) {
                    if (stopped()) {
                        failed(work.result, "failed", "stopped before it was all sent");
                        return false;
                    }
                    if (!first) std::this_thread::sleep_for(BATCH_GAP);
                    first = false;
                    std::vector<int64_t> batch(ids.begin() + start, ids.begin() + std::min(ids.size(), start + BATCH));
                    Net::Response response;
                    bool reached = send("POST", HOYOLAB + "batch_mark_map_point?" + HOYOLAB_QUERY, work.login, HOYOLAB_HEADERS,
                        HoyolabBatch(map, batch, unmark), response);
                    HoyolabAnswer answer = ReadHoyolab(response);
                    if (!reached || answer.retcode != 0) {
                        hoyolabRefused(work, response, reached);
                        return false;
                    }
                    (unmark ? work.result.removed : work.result.added) += (int)batch.size();
                }
            }
        }
        Accounts::Working(Site::HoyolabMap);
        return true;
    }

    static bool writeAppsample(Work& work) {
        bool first = true;
        for (bool remove : { false, true }) {
            const std::vector<int64_t>& ids = remove ? work.plan.remove : work.plan.add;
            for (size_t start = 0; start < ids.size(); start += BATCH) {
                if (stopped()) {
                    failed(work.result, "failed", "stopped before it was all sent");
                    return false;
                }
                if (!first) std::this_thread::sleep_for(BATCH_GAP);
                first = false;
                std::vector<int64_t> batch(ids.begin() + start, ids.begin() + std::min(ids.size(), start + BATCH));
                Net::Response response;
                if (!sendAppsample(work, "POST", FIRESTORE + DOCUMENTS + ":commit", AppsampleCommit(work.login.account, batch, remove), response))
                    return false;
                if (response.status != 200) {
                    appsampleFailed(work, response);
                    return false;
                }
                (remove ? work.result.removed : work.result.added) += (int)batch.size();
            }
        }
        return true;
    }

    // false when HoYoLAB didn't answer it; a player with no nickname is answered
    static bool readName(const Job& job) {
        std::optional<Login> login = currentLogin(Site::HoyolabMap);
        if (!login) return false;
        if (login->account != job.accounts.at(Site::HoyolabMap)) {
            wipe(*login);
            return true;
        }
        Net::Response response;
        bool reached = send("GET", NAME_URL + login->account, *login, NAME_HEADERS, "", response);
        HoyolabAnswer answer = ReadHoyolab(response);
        std::string name = reached ? ReadHoyolabName(response) : "";
        if (!name.empty()) Accounts::SetName(Site::HoyolabMap, login->account, name);
        else std::cerr << "[marks] HoYoLAB's nickname couldn't be read" << (answer.read ? ": " + answer.message : "") << std::endl;
        wipe(*login);
        return reached && answer.read && answer.retcode == 0;
    }

    // nothing when the job needs the player's answer first, the question is
    // in ask then
    static std::optional<Result> run(const Job& job, std::optional<Question>& ask) {
        const Request& request = job.request;
        std::vector<Work> works(request.sites.size());
        std::shared_ptr<const ItemCatalog::Catalog> catalog = ItemCatalog::current();
        for (size_t i = 0; i < request.sites.size(); i++) {
            Work& work = works[i];
            work.result.site = request.sites[i];
            if (!catalog) {
                failed(work.result, "no_items", "the item files aren't loaded");
                continue;
            }
            std::optional<Login> login = currentLogin(work.result.site);
            auto started = job.accounts.find(work.result.site);
            if (!login) {
                // still logged in: appsample's login couldn't be refreshed just now
                if (!loginFor && Accounts::Get(work.result.site).state == Accounts::State::LoggedIn)
                    failed(work.result, "failed", "the login couldn't be refreshed");
                else failed(work.result, "logged_out", "not logged in");
                continue;
            }
            work.login = *login;
            wipe(*login);
            if (started == job.accounts.end() || started->second != work.login.account) {
                failed(work.result, "account_changed", "logged out or in as another account since it was started");
                continue;
            }
            // a restore's backup is checked before the site is read
            std::vector<int64_t> saved;
            if (request.action == Action::Restore && !ReadBackup(backupFolder(), request.backup, work.result.site, work.login.account, saved)) {
                failed(work.result, "no_backup", "the backup isn't there or is another account's");
                continue;
            }
            work.read = work.result.site == Site::HoyolabMap ? readHoyolab(work, *catalog) : readAppsample(work);
            if (!work.read) continue;
            if (request.action == Action::Fetch) {
                work.result.marks = hoyolabIds(work.result.site, work.marked, *catalog);
                std::sort(work.result.marks.begin(), work.result.marks.end());
                // the asked ones that couldn't be marked there
                for (int64_t id : request.add) {
                    if (!ownId(work, *catalog, id, true)) work.result.skipped.push_back(id);
                }
                work.result.ok = true;
            }
            else if (request.action == Action::Restore) planRestore(work, saved, *catalog);
            else planSite(work, request, *catalog);
        }

        if (request.action != Action::Fetch) {
            std::vector<Work*> ready;
            for (Work& work : works) {
                if (work.read) ready.push_back(&work);
            }
            // points, by HoYoLAB id: one point unmarked on both sites is one
            std::set<int64_t> removing;
            for (Work* work : ready) {
                for (int64_t own : work->plan.remove) removing.insert(ourId(*work, own));
            }
            if (removing.size() > 1) {
                // more marked meanwhile than the player was asked about: asked again
                bool seen = std::includes(job.answered.begin(), job.answered.end(), removing.begin(), removing.end());
                Choice choice = !request.owner ? Choice::Keep : seen ? job.answer : Choice::None;
                if (choice == Choice::None) {
                    ask = Question{ job.id, request.owner, (int)removing.size(), std::move(removing), questionLines(ready, *catalog) };
                    ready.clear();
                }
                else if (choice == Choice::Cancel) {
                    for (Work* work : ready) failed(work->result, "cancelled", "the player cancelled the change");
                    ready.clear();
                }
                else if (choice == Choice::Keep) {
                    for (Work* work : ready) {
                        for (int64_t own : work->plan.remove) work->result.kept.push_back(ourId(*work, own));
                        work->plan.remove.clear();
                    }
                }
            }
            for (Work* work : ready) {
                if (work->plan.add.empty() && work->plan.remove.empty()) {
                    work->result.ok = true;
                    continue;
                }
                if (!sameAccount(*work)) continue;
                work->result.backup = SaveBackup(backupFolder(), work->result.site, work->login.account, work->marked);
                if (work->result.backup.empty()) {
                    failed(work->result, "backup_failed", "the backup couldn't be written, nothing was changed");
                    continue;
                }
                work->result.ok = work->result.site == Site::HoyolabMap ? writeHoyolab(*work, *catalog) : writeAppsample(*work);
            }
        }

        for (Work& work : works) wipe(work.login);
        if (ask) return std::nullopt;
        Result result;
        for (Work& work : works) {
            if (!work.result.ok && !work.result.why.empty())
                std::cerr << "[marks] " << Accounts::SiteLabel(work.result.site) << ": " << work.result.why << std::endl;
            result.sites.push_back(std::move(work.result));
        }
        return result;
    }

    static void work() {
        Net::Stopper own;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopper = &own;
        }
        // the first job whose owner isn't waiting for an answer
        auto next = [] {
            return std::find_if(queue.begin(), queue.end(), [](const Job& job) {
                return job.name || std::none_of(asking.begin(), asking.end(), [&](const Asking& a) { return a.job.request.owner == job.request.owner; });
            });
        };
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping || next() != queue.end(); });
                if (stopping) break;
                auto it = next();
                job = std::move(*it);
                queue.erase(it);
                running = job.id;
                runningOwner = job.request.owner;
                runningSites = job.request.sites;
            }
            std::optional<Question> ask;
            std::optional<Result> result;
            bool nameRead = !job.name || readName(job);
            if (!job.name) result = run(job, ask);
            std::lock_guard<std::mutex> lock(mutex);
            // a cancelled one waiting for an answer isn't asked
            if (running == job.id && cancelling == job.id && !result) result = cancelled(job);
            // forgotten while it ran
            if (running == job.id && result) results[job.id] = { std::move(*result), job.request.owner, Clock::now() };
            else if (running == job.id && ask) asking.push_back({ std::move(job), std::move(*ask) });
            cancelling = 0;
            if (!nameRead) nameFailed = true;
            running = 0;
            runningOwner = nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex);
        stopper = nullptr;
    }

    void Init() {
        std::lock_guard<std::mutex> lock(mutex);
        if (worker.joinable()) return;
        stopping = false;
        nameAskedFor.clear();
        worker = std::thread(work);
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            if (stopper) stopper->stop();
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard<std::mutex> lock(mutex);
        queue.clear();
        asking.clear();
        results.clear();
    }

    static std::map<Site, std::string> accountsFor(const std::vector<Site>& sites) {
        std::map<Site, std::string> accounts;
        for (Site site : sites) accounts[site] = Accounts::AccountId(site);
        return accounts;
    }

    void Frame() {
        // the nickname once for each account logged in
        std::string account = loginFor ? "" : Accounts::AccountId(Site::HoyolabMap);
        bool again = false;
        if (!account.empty() && account == nameAskedFor && Clock::now() - nameAskedAt > NAME_AGAIN) {
            std::lock_guard<std::mutex> lock(mutex);
            again = nameFailed;
        }
        if (account != nameAskedFor || again) {
            nameAskedFor = account;
            nameAskedAt = Clock::now();
            if (!account.empty()) {
                Job job;
                job.name = true;
                job.request.sites = { Site::HoyolabMap };
                job.accounts[Site::HoyolabMap] = account;
                std::lock_guard<std::mutex> lock(mutex);
                nameFailed = false;
                job.id = nextJob++;
                queue.push_back(std::move(job));
                wake.notify_all();
            }
        }

        std::unique_lock<std::mutex> lock(mutex);
        for (auto it = results.begin(); it != results.end();) {
            if (Clock::now() - it->second.at > KeepResult) it = results.erase(it);
            else ++it;
        }
        // one question at a time
        if (asking.empty() || asking.front().question.handed) return;
        asking.front().question.handed = true;
        Question copy = asking.front().question;
        lock.unlock();
        int job = copy.job;
        // the answered job goes first in the line and reads the marks again
        Asks::askUnmark(*(Extensions::Extension*)copy.owner, copy.lines, copy.count, [job](Asks::Answer answer) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = std::find_if(asking.begin(), asking.end(), [job](const Asking& a) { return a.job.id == job; });
            if (it == asking.end()) return;
            it->job.answer = answer == Asks::Answer::Allow ? Choice::Unmark : answer == Asks::Answer::DontAllow ? Choice::Keep : Choice::Cancel;
            it->job.answered = it->question.points;
            queue.push_front(std::move(it->job));
            asking.erase(it);
            wake.notify_all();
        });
    }

    int WaitingOf(const void* owner) {
        std::lock_guard<std::mutex> lock(mutex);
        int count = (int)std::count_if(queue.begin(), queue.end(), [owner](const Job& job) { return !job.name && job.request.owner == owner; });
        if (running > 0 && runningOwner == owner) count++;
        count += (int)std::count_if(asking.begin(), asking.end(), [owner](const Asking& a) { return a.job.request.owner == owner; });
        for (const auto& [id, done] : results) count += done.owner == owner ? 1 : 0;
        return count;
    }

    int Start(const Request& request) {
        if (request.sites.empty() || WaitingOf(request.owner) >= MaxWaiting) return 0;
        Job job;
        job.request = request;
        job.accounts = accountsFor(request.sites);
        if (loginFor) {
            for (Site site : request.sites) {
                std::optional<Login> login = loginFor(site);
                job.accounts[site] = login ? login->account : "";
            }
        }
        int id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            id = job.id = nextJob++;
            queue.push_back(std::move(job));
        }
        wake.notify_all();
        return id;
    }

    std::optional<Result> Take(int id, const void* owner) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = results.find(id);
        if (it == results.end() || it->second.owner != owner) return std::nullopt;
        Result result = std::move(it->second.result);
        results.erase(it);
        return result;
    }

    bool Done(int id) {
        std::lock_guard<std::mutex> lock(mutex);
        return results.count(id) > 0;
    }

    void Forget(const void* owner) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.erase(std::remove_if(queue.begin(), queue.end(), [owner](const Job& job) { return !job.name && job.request.owner == owner; }), queue.end());
        for (auto it = results.begin(); it != results.end();) {
            if (it->second.owner == owner) it = results.erase(it);
            else ++it;
        }
        if (running > 0 && runningOwner == owner) running = -1;
        asking.erase(std::remove_if(asking.begin(), asking.end(), [owner](const Asking& a) { return a.job.request.owner == owner; }), asking.end());
    }

    void Cancel(const void* owner, Site site) {
        bool shown = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto mine = [owner, site](const Job& job) {
                return job.request.owner == owner
                    && std::find(job.request.sites.begin(), job.request.sites.end(), site) != job.request.sites.end();
            };
            auto cancel = [](const Job& job) { results[job.id] = { cancelled(job), job.request.owner, Clock::now() }; };
            for (auto it = queue.begin(); it != queue.end();) {
                if (mine(*it)) {
                    cancel(*it);
                    it = queue.erase(it);
                }
                else ++it;
            }
            if (running > 0 && runningOwner == owner && std::find(runningSites.begin(), runningSites.end(), site) != runningSites.end())
                cancelling = running;
            for (auto it = asking.begin(); it != asking.end();) {
                if (mine(it->job)) {
                    shown = shown || it->question.handed;
                    cancel(it->job);
                    it = asking.erase(it);
                }
                else ++it;
            }
        }
        if (shown) Asks::forget(*(const Extensions::Extension*)owner, Asks::Kind::Unmark);
    }

    std::vector<Backup> BackupsOf(Site site) {
        std::string account;
        if (loginFor) {
            std::optional<Login> login = loginFor(site);
            if (login) {
                account = login->account;
                wipe(*login);
            }
        }
        else account = Accounts::AccountId(site);
        if (account.empty()) return {};
        std::string hash = AccountHash(site, account);
        std::vector<Backup> found = Backups(backupFolder());
        found.erase(std::remove_if(found.begin(), found.end(), [&](const Backup& each) { return each.site != site || each.account != hash; }),
            found.end());
        return found;
    }

    void SetForTests(LoginFor login, Sender send, const std::filesystem::path& folder) {
        std::lock_guard<std::mutex> lock(mutex);
        loginFor = login;
        sender = send;
        testFolder = folder;
    }
}
