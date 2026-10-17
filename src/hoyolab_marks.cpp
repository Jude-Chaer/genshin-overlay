#include "hoyolab_marks.hpp"
#include "hoyolab_account.hpp"
#include "helpers.hpp"
#include "overlay.hpp"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <imgui.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace HoyolabMarks {
    // the only two hosts the cookie goes to: the map, and the account's game characters
    static const wchar_t* HOST = L"sg-public-api.hoyolab.com";
    static const wchar_t* ACCOUNT_HOST = L"api-account-os.hoyolab.com";
    static const std::string BASE = "/common/map_user/ys_obc/v1/map/point/";
    static const std::string QUERY = "app_sn=ys_obc&lang=en-us";
    // how many points go in one batch, and the wait between two
    static constexpr size_t BATCH = 50;
    static constexpr auto BATCH_GAP = std::chrono::milliseconds(300);

    enum class Kind { Fetch, Change, Restore, Roles };

    struct Job {
        int id = 0;
        Kind kind = Kind::Fetch;
        std::vector<int64_t> add, remove;
        std::string backup;
        bool backUpFetch = false;
    };

    // the yes/no for unmarking, asked by the worker, answered in DrawConfirm
    struct Question {
        bool waiting = false;
        bool answered = false;
        bool yes = false;
        int count = 0;
        std::vector<std::pair<std::string, int>> byType;
        std::string backup;
    };

    static std::mutex mutex;
    static std::condition_variable wake;
    static std::deque<Job> queue;
    static std::map<int, Result> results;
    static int nextJob = 1;
    static int running = 0;
    static Question question;
    static std::vector<Backup> backups;
    static bool stopping = false;
    static std::thread worker;

    // worker only
    struct Point { int map; int type; };
    static std::unordered_map<int64_t, Point> pointIndex;
    static std::unordered_map<int, std::string> typeNames;
    static std::vector<int> maps;
    static bool indexLoaded = false;
    static bool backedUpThisRun = false;
    // what HoYoLAB should have after our last change, a different answer means
    // someone changed it somewhere else since
    static std::set<int64_t> expected;
    static HINTERNET internet = nullptr;

    static std::filesystem::path backupFolder() {
        return Helpers::exeDirectory() / "hoyolab-backups";
    }

    // the item files say which map each point is on and what it is
    static void loadIndex() {
        if (indexLoaded) return;
        std::filesystem::path items = Helpers::exeDirectory() / "items";
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(items / "maps", error)) {
            try { maps.push_back(std::stoi(entry.path().filename().string())); }
            catch (const std::exception&) {}
            std::ifstream file(entry.path() / "tree.json", std::ios::binary);
            json tree = json::parse(file, nullptr, false);
            if (!tree.is_array()) continue;
            for (const json& group : tree) {
                if (!group.is_object() || !group.contains("children") || !group["children"].is_array()) continue;
                for (const json& kind : group["children"]) {
                    if (kind.is_object() && kind["id"].is_number_integer() && kind["name"].is_string())
                        typeNames[kind["id"].get<int>()] = kind["name"].get<std::string>();
                }
            }
        }
        for (const auto& entry : std::filesystem::directory_iterator(items / "positions", error)) {
            std::ifstream file(entry.path(), std::ios::binary);
            json points = json::parse(file, nullptr, false);
            if (!points.is_array()) continue;
            for (const json& point : points) {
                if (!point.is_object() || !point["id"].is_number_integer() || !point["map_id"].is_number_integer()) continue;
                pointIndex[point["id"].get<int64_t>()] = { point["map_id"].get<int>(), point.value("label_id", 0) };
            }
        }
        indexLoaded = !maps.empty() && !pointIndex.empty();
    }

    struct Answer {
        bool reached = false;
        int retcode = -1;
        std::string message;
        json data;
    };

    // The cookie goes only on these requests: our own session with cookie
    // handling and redirects off, always to HoYoLAB over https.
    static Answer call(const wchar_t* host, const wchar_t* method, const std::string& path, const std::string& body) {
        Answer answer;
        std::string cookie = HoyolabAccount::CookieHeader();
        if (cookie.empty()) {
            answer.message = "Not logged in to HoYoLAB.";
            return answer;
        }
        if (!internet) {
            internet = WinHttpOpen(L"genshin-overlay", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (internet) WinHttpSetTimeouts(internet, 10000, 10000, 30000, 30000);
        }

        std::string headers = cookie
            + "Origin: https://act.hoyolab.com\r\nReferer: https://act.hoyolab.com/\r\nx-rpc-map_version: 4.5\r\n"
            + (body.empty() ? "" : "Content-Type: application/json\r\n");
        std::wstring wideHeaders(headers.begin(), headers.end());
        std::wstring target(path.begin(), path.end());
        SecureZeroMemory(&cookie[0], cookie.size());
        SecureZeroMemory(&headers[0], headers.size());

        HINTERNET connection = internet ? WinHttpConnect(internet, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
        HINTERNET request = connection ? WinHttpOpenRequest(connection, method, target.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        std::string text;
        if (request) {
            DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
            if (WinHttpSendRequest(request, wideHeaders.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
                && WinHttpReceiveResponse(request, nullptr)) {
                answer.reached = true;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                    size_t offset = text.size();
                    text.resize(offset + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, &text[offset], available, &read)) break;
                    text.resize(offset + read);
                }
            }
        }
        SecureZeroMemory(&wideHeaders[0], wideHeaders.size() * sizeof(wchar_t));
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);

        if (!answer.reached) {
            answer.message = "Couldn't reach HoYoLAB.";
            return answer;
        }
        json parsed = json::parse(text, nullptr, false);
        if (!parsed.is_object() || !parsed["retcode"].is_number_integer()) {
            answer.message = "HoYoLAB sent back something unexpected.";
            return answer;
        }
        answer.retcode = parsed["retcode"].get<int>();
        if (parsed["message"].is_string()) answer.message = parsed["message"].get<std::string>();
        if (parsed.contains("data")) answer.data = parsed["data"];
        // logged out on their side
        if (answer.retcode == -100 || answer.retcode == -2004 || answer.retcode == 10001) HoyolabAccount::LoginExpired();
        return answer;
    }

    static void fail(Result& result, const Answer& answer) {
        result.ok = false;
        result.captcha = answer.retcode == -5001;
        if (result.captcha) result.error = "HoYoLAB wants a captcha first.";
        else if (!answer.reached || answer.retcode == -1) result.error = answer.message;
        else result.error = "HoYoLAB said: " + answer.message + " (" + std::to_string(answer.retcode) + ")";
    }

    // every point marked there, by map
    static bool fetchAll(std::map<int, std::set<int64_t>>& out, Result& result) {
        for (int map : maps) {
            Answer answer = call(HOST, L"GET", BASE + "mark_map_point_list?map_id=" + std::to_string(map) + "&" + QUERY, "");
            if (answer.retcode != 0) {
                fail(result, answer);
                return false;
            }
            std::set<int64_t>& marks = out[map];
            if (answer.data.is_object() && answer.data["list"].is_array()) {
                for (const json& mark : answer.data["list"]) {
                    if (mark.is_object() && mark["point_id"].is_number_integer()) marks.insert(mark["point_id"].get<int64_t>());
                }
            }
        }
        return true;
    }

    // the Genshin characters on the account, one per server
    static void fetchRoles(Result& result) {
        Answer answer = call(ACCOUNT_HOST, L"GET", "/binding/api/getUserGameRolesByLtoken?game_biz=hk4e_global", "");
        if (answer.retcode != 0) {
            fail(result, answer);
            return;
        }
        if (answer.data.is_object() && answer.data["list"].is_array()) {
            for (const json& role : answer.data["list"]) {
                if (!role.is_object()) continue;
                auto text = [&](const char* key) {
                    auto value = role.find(key);
                    if (value == role.end()) return std::string();
                    if (value->is_string()) return value->get<std::string>();
                    if (value->is_number_integer()) return std::to_string(value->get<int64_t>());
                    return std::string();
                };
                Role found;
                found.uid = text("game_uid");
                found.nickname = text("nickname");
                found.server = text("region_name");
                if (found.server.empty()) found.server = text("region");
                try { found.level = std::stoi(text("level")); }
                catch (const std::exception&) {}
                result.roles.push_back(found);
            }
        }
        result.ok = true;
    }

    static std::set<int64_t> together(const std::map<int, std::set<int64_t>>& byMap) {
        std::set<int64_t> all;
        for (const auto& [map, marks] : byMap) all.insert(marks.begin(), marks.end());
        return all;
    }

    static std::string timeName() {
        std::time_t now = std::time(nullptr);
        std::tm local;
        localtime_s(&local, &now);
        char name[32];
        std::strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S", &local);
        return name;
    }

    static bool saveBackup(const std::map<int, std::set<int64_t>>& byMap, std::string& name) {
        std::error_code error;
        std::filesystem::create_directories(backupFolder(), error);
        // two in the same second get a number, so neither is lost
        name = timeName();
        std::string first = name;
        for (int n = 2; std::filesystem::exists(backupFolder() / (name + ".json"), error); n++) {
            name = first + "_" + std::to_string(n);
        }
        json marks = json::object();
        int count = 0;
        for (const auto& [map, ids] : byMap) {
            marks[std::to_string(map)] = std::vector<int64_t>(ids.begin(), ids.end());
            count += (int)ids.size();
        }
        json data = { { "account", HoyolabAccount::Account() }, { "count", count }, { "marks", marks } };
        std::ofstream file(backupFolder() / (name + ".json"), std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << data.dump(1);
        file.close();
        if (!file) return false;

        std::lock_guard<std::mutex> lock(mutex);
        backups.insert(backups.begin(), { name, count });
        return true;
    }

    static void loadBackupList() {
        std::vector<Backup> found;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(backupFolder(), error)) {
            if (entry.path().extension() != ".json") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            json data = json::parse(file, nullptr, false);
            if (!data.is_object()) continue;
            found.push_back({ entry.path().stem().string(), data.value("count", 0) });
        }
        std::sort(found.begin(), found.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
        std::lock_guard<std::mutex> lock(mutex);
        backups = found;
    }

    static bool readBackup(const std::string& name, std::set<int64_t>& out) {
        // only a name from the list, never a path
        if (name.find_first_of("/\\.:") != std::string::npos) return false;
        std::ifstream file(backupFolder() / (name + ".json"), std::ios::binary);
        json data = json::parse(file, nullptr, false);
        if (!data.is_object() || !data["marks"].is_object()) return false;
        for (const auto& [map, ids] : data["marks"].items()) {
            if (!ids.is_array()) continue;
            for (const json& id : ids) {
                if (id.is_number_integer()) out.insert(id.get<int64_t>());
            }
        }
        return true;
    }

    // waits for the yes or no from DrawConfirm
    static bool askToUnmark(const std::vector<int64_t>& ids, const std::string& backup) {
        std::map<std::string, int> counts;
        for (int64_t id : ids) {
            auto point = pointIndex.find(id);
            auto name = point != pointIndex.end() ? typeNames.find(point->second.type) : typeNames.end();
            counts[name != typeNames.end() ? name->second : "Other"]++;
        }
        std::vector<std::pair<std::string, int>> byType(counts.begin(), counts.end());
        std::sort(byType.begin(), byType.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

        std::unique_lock<std::mutex> lock(mutex);
        question = Question();
        question.waiting = true;
        question.count = (int)ids.size();
        question.byType = byType;
        question.backup = backup;
        wake.wait(lock, [] { return stopping || question.answered; });
        bool yes = question.answered && question.yes;
        question = Question();
        return yes;
    }

    // sends one map's changes in batches, false and the reason when HoYoLAB says no
    static bool send(int map, const std::vector<int64_t>& ids, bool unmark, Result& result) {
        for (size_t start = 0; start < ids.size(); start += BATCH) {
            json list = json::array();
            for (size_t i = start; i < std::min(ids.size(), start + BATCH); i++) {
                list.push_back({ { "point_id", ids[i] }, { "is_delete", unmark } });
            }
            json body = { { "map_id", map }, { "list", list } };
            Answer answer = call(HOST, L"POST", BASE + "batch_mark_map_point?" + QUERY, body.dump());
            if (answer.retcode != 0) {
                fail(result, answer);
                return false;
            }
            size_t sent = std::min(ids.size(), start + BATCH) - start;
            (unmark ? result.removed : result.added) += (int)sent;
            for (size_t i = start; i < start + sent; i++) {
                if (unmark) expected.erase(ids[i]);
                else expected.insert(ids[i]);
            }
            std::this_thread::sleep_for(BATCH_GAP);
        }
        return true;
    }

    static void change(const Job& job, Result& result) {
        std::map<int, std::set<int64_t>> byMap;
        if (!fetchAll(byMap, result)) return;
        std::set<int64_t> current = together(byMap);

        std::set<int64_t> wanted;
        if (job.kind == Kind::Restore) {
            if (!readBackup(job.backup, wanted)) {
                result.error = "Couldn't read that backup.";
                return;
            }
        }
        else {
            wanted.insert(job.add.begin(), job.add.end());
        }

        // only what's different from HoYoLAB, sorted by map
        std::map<int, std::vector<int64_t>> adds, removes;
        std::vector<int64_t> removing;
        for (int64_t id : wanted) {
            if (current.count(id)) continue;
            auto point = pointIndex.find(id);
            if (point == pointIndex.end()) result.unknown.push_back(id);
            else adds[point->second.map].push_back(id);
        }
        for (int64_t id : job.remove) {
            if (!current.count(id)) continue;
            for (const auto& [map, marks] : byMap) {
                if (marks.count(id)) removes[map].push_back(id);
            }
            removing.push_back(id);
        }
        if (adds.empty() && removing.empty()) {
            result.ok = true;
            expected = current;
            return;
        }

        // A backup the first time this run, before any unmark, and whenever
        // HoYoLAB changed since our last change. Not for every tap.
        std::string backup;
        if (!backedUpThisRun || !removing.empty() || current != expected) {
            if (!saveBackup(byMap, backup)) {
                result.error = "Couldn't save a backup of your HoYoLAB marks, nothing was changed.";
                return;
            }
            backedUpThisRun = true;
        }
        result.backup = backup;
        expected = current;

        if (!removing.empty() && !askToUnmark(removing, backup)) {
            result.kept = removing;
            removes.clear();
        }

        for (const auto& [map, ids] : adds) {
            if (!send(map, ids, false, result)) return;
        }
        for (const auto& [map, ids] : removes) {
            if (!send(map, ids, true, result)) return;
        }
        result.ok = true;
    }

    static void work() {
        loadBackupList();
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [] { return stopping || !queue.empty(); });
                if (stopping) break;
                job = queue.front();
                queue.pop_front();
                running = job.id;
            }

            Result result;
            loadIndex();
            if (!indexLoaded) {
                result.error = "The item files aren't there yet.";
            }
            else if (job.kind == Kind::Fetch) {
                std::map<int, std::set<int64_t>> byMap;
                if (fetchAll(byMap, result)) {
                    if (job.backUpFetch && !saveBackup(byMap, result.backup)) {
                        result.error = "Couldn't save a backup of your HoYoLAB marks.";
                    }
                    else {
                        std::set<int64_t> all = together(byMap);
                        result.marks.assign(all.begin(), all.end());
                        result.ok = true;
                    }
                }
            }
            else if (job.kind == Kind::Roles) {
                fetchRoles(result);
            }
            else {
                change(job, result);
            }

            std::lock_guard<std::mutex> lock(mutex);
            running = 0;
            results[job.id] = std::move(result);
        }
        if (internet) WinHttpCloseHandle(internet);
        internet = nullptr;
    }

    static int add(Job job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            job.id = nextJob++;
            queue.push_back(job);
        }
        wake.notify_all();
        return job.id;
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

    int Fetch(bool backup) {
        Job job;
        job.kind = Kind::Fetch;
        job.backUpFetch = backup;
        return add(job);
    }

    int Roles() {
        Job job;
        job.kind = Kind::Roles;
        return add(job);
    }

    int Change(const std::vector<int64_t>& addIds, const std::vector<int64_t>& removeIds) {
        Job job;
        job.kind = Kind::Change;
        job.add = addIds;
        job.remove = removeIds;
        return add(job);
    }

    int Restore(const std::string& backup) {
        Job job;
        job.kind = Kind::Restore;
        job.backup = backup;
        return add(job);
    }

    bool GetResult(int job, Result& out) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = results.find(job);
        if (it == results.end()) return false;
        out = std::move(it->second);
        results.erase(it);
        return true;
    }

    bool Busy() {
        std::lock_guard<std::mutex> lock(mutex);
        return running != 0 || !queue.empty();
    }

    std::vector<Backup> Backups() {
        std::lock_guard<std::mutex> lock(mutex);
        return backups;
    }

    bool NeedsDrawing() {
        std::lock_guard<std::mutex> lock(mutex);
        return question.waiting;
    }

    void DrawConfirm() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!question.waiting || question.answered) return;

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowFocus();
        ImGui::Begin("Unmark on HoYoLAB?##hoyolabConfirm", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::Text("This unmarks %d %s on your HoYoLAB map:", question.count, question.count == 1 ? "point" : "points");
        size_t shown = std::min<size_t>(question.byType.size(), 12);
        for (size_t i = 0; i < shown; i++) {
            ImGui::BulletText("%s  x%d", question.byType[i].first.c_str(), question.byType[i].second);
        }
        if (question.byType.size() > shown) ImGui::TextDisabled("and %d more types", (int)(question.byType.size() - shown));
        if (!question.backup.empty()) ImGui::TextDisabled("Your marks there were saved first: hoyolab-backups\\%s.json", question.backup.c_str());
        ImGui::Spacing();
        bool answered = false, yes = false;
        if (ImGui::Button("Unmark them on HoYoLAB")) answered = yes = true;
        ImGui::SameLine();
        if (ImGui::Button("Keep them marked")) answered = true;
        ImVec2 position = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        Overlay::ClaimMouse(position.x, position.y, position.x + size.x, position.y + size.y);
        ImGui::End();

        if (answered) {
            question.answered = true;
            question.yes = yes;
            lock.unlock();
            wake.notify_all();
        }
    }
}
