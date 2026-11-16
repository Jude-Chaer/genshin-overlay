#include "update_check.hpp"
#include "helpers.hpp"
#include "net.hpp"
#include "files.hpp"
#include <windows.h>
#include <shellapi.h>
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
    static const std::string DOWNLOADS = "https://github.com/PocketEsoptron/PocketEsoptron/releases/download/";
    static const wchar_t* EXE = L"PocketEsoptron.exe";

    static std::thread worker;
    static std::mutex lock;
    static std::condition_variable wake;
    static bool stopping = false;
    static std::unique_ptr<Net::Stopper> stopper;
    static Release found;
    static Download download = Download::None;
    // the version downloaded and where: the installer, or the unpacked portable files
    static std::string readyVersion;
    static std::filesystem::path readyPath;

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
            if (!Later(version, best.version.empty() ? current : best.version)) continue;
            best = { version, page->get<std::string>() };
            auto assets = release.find("assets");
            if (assets == release.end() || !assets->is_array()) continue;
            for (const nlohmann::json& asset : *assets) {
                if (!asset.is_object()) continue;
                auto name = asset.find("name");
                auto url = asset.find("browser_download_url");
                if (name == asset.end() || !name->is_string() || url == asset.end() || !url->is_string()) continue;
                // only the app's own downloads
                if (*url != DOWNLOADS + version + "/" + name->get<std::string>()) continue;
                if (*name == "PocketEsoptron-Setup-" + version + ".exe") best.installer = *url;
                else if (*name == "PocketEsoptron-" + version + "-portable.zip") best.portable = *url;
                else if (*name == "SHA256SUMS.txt") best.sums = *url;
            }
        }
        return best;
    }

    bool Installed(const std::filesystem::path& exeFolder) {
        std::error_code ec;
        return std::filesystem::exists(exeFolder / "unins000.exe", ec);
    }

    // lines of "<sha256>  <name>", the way sha256sum writes them
    std::optional<std::string> SumFor(const std::string& sums, const std::string& name) {
        size_t start = 0;
        while (start < sums.size()) {
            size_t end = sums.find('\n', start);
            std::string line = sums.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? sums.size() : end + 1;
            if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.size() < 66 || line[64] != ' ') continue;
            size_t nameAt = line.find_first_not_of(" *", 64);
            if (nameAt == std::string::npos || line.substr(nameAt) != name) continue;
            std::string sum = line.substr(0, 64);
            for (char& c : sum) c = (char)tolower((unsigned char)c);
            if (sum.find_first_not_of("0123456789abcdef") != std::string::npos) return std::nullopt;
            return sum;
        }
        return std::nullopt;
    }

    // a command run without a window, its exit code or -1
    static int run(std::wstring command, DWORD waitMs) {
        STARTUPINFOW startup = { sizeof(startup) };
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return -1;
        DWORD code = (DWORD)-1;
        if (WaitForSingleObject(process.hProcess, waitMs) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
        else TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return (int)code;
    }

    // with Windows' own tar.exe, which reads zip files
    std::filesystem::path Unpack(const std::filesystem::path& zip, const std::filesystem::path& to) {
        std::error_code ec;
        std::filesystem::remove_all(to, ec);
        std::filesystem::create_directories(to, ec);
        if (ec) return {};
        wchar_t system[MAX_PATH] = {};
        if (!GetSystemDirectoryW(system, MAX_PATH)) return {};
        std::wstring command = L"\"" + std::wstring(system) + L"\\tar.exe\" -xf \"" + zip.wstring() + L"\" -C \"" + to.wstring() + L"\"";
        if (run(command, 120000) != 0) return {};
        if (std::filesystem::exists(to / EXE, ec)) return to;
        // the zip holds one folder
        for (const auto& entry : std::filesystem::directory_iterator(to, ec)) {
            if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / EXE, ec)) return entry.path();
        }
        return {};
    }

    bool PutInPlace(const std::filesystem::path& unpacked, const std::filesystem::path& folder) {
        std::error_code ec;
        std::filesystem::path exe = folder / EXE, old = folder / (std::wstring(EXE) + L".old");
        std::filesystem::remove(old, ec);
        // a running exe can be renamed, not written over
        if (std::filesystem::exists(exe, ec)) {
            std::filesystem::rename(exe, old, ec);
            if (ec) return false;
        }
        bool ok = true;
        auto it = std::filesystem::recursive_directory_iterator(unpacked, ec);
        if (ec) ok = false;
        for (; ok && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                ok = false;
                break;
            }
            std::filesystem::path relative = std::filesystem::relative(it->path(), unpacked, ec);
            if (ec || relative == "portable") {
                ok = !ec;
                continue;
            }
            std::filesystem::path target = folder / relative;
            if (it->is_directory(ec)) std::filesystem::create_directories(target, ec);
            else std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) ok = false;
        }
        if (!ok && std::filesystem::exists(old, ec)) {
            std::filesystem::remove(exe, ec);
            std::filesystem::rename(old, exe, ec);
        }
        return ok;
    }

    void RemoveOld() {
        std::error_code ec;
        std::filesystem::remove(Helpers::exeDirectory() / (std::wstring(EXE) + L".old"), ec);
    }

    static void setDownload(Download state, const std::string& version = "", const std::filesystem::path& path = {}) {
        std::lock_guard<std::mutex> hold(lock);
        download = state;
        readyVersion = version;
        readyPath = path;
    }

    static std::filesystem::path updatesFolder() {
        return Helpers::dataDirectory() / "updates";
    }

    // the file for this copy, checked against the release's sums; the portable one unpacked
    static void fetch(const Release& release) {
        {
            std::lock_guard<std::mutex> hold(lock);
            if (download == Download::Ready && readyVersion == release.version) return;
        }
        bool installed = Installed(Helpers::exeDirectory());
        const std::string& url = installed ? release.installer : release.portable;
        if (url.empty() || release.sums.empty()) return setDownload(Download::None);
        setDownload(Download::Downloading);
        std::string name = url.substr(url.rfind('/') + 1);
        std::filesystem::path folder = updatesFolder();
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        std::filesystem::create_directories(folder, ec);

        Net::Response sums, file;
        Net::Limits small, big;
        small.maxBody = 64 * 1024;
        small.deadlineMs = 30000;
        big.maxBody = 256 * 1024 * 1024;
        big.deadlineMs = 20 * 60 * 1000;
        std::optional<std::string> sum;
        if (Net::get(release.sums, sums, false, nullptr, stopper.get(), small) && sums.status == 200) sum = SumFor(sums.body, name);
        bool got = sum && Net::get(url, file, false, nullptr, stopper.get(), big) && file.status == 200;
        if (stopper->stopped()) return setDownload(Download::None);
        if (!got || Files::sha256(file.body) != *sum) {
            std::cerr << "Update check: couldn't download " << name << (got ? ", its sha256 isn't the one in SHA256SUMS.txt" : "") << std::endl;
            return setDownload(Download::Failed);
        }
        std::filesystem::path saved = folder / name;
        if (!Files::writeFile(saved, file.body)) return setDownload(Download::Failed);
        std::filesystem::path ready = saved;
        if (!installed) {
            ready = Unpack(saved, folder / "unpacked");
            if (ready.empty()) {
                std::cerr << "Update check: couldn't unpack " << name << std::endl;
                return setDownload(Download::Failed);
            }
        }
        std::cout << "Update check: " << release.version << " downloaded" << std::endl;
        setDownload(Download::Ready, release.version, ready);
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
        {
            std::lock_guard<std::mutex> hold(lock);
            found = newer;
        }
        if (!newer.version.empty()) fetch(newer);
        else {
            // what an earlier update left
            std::error_code ec;
            std::filesystem::remove_all(updatesFolder(), ec);
        }
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

    Download Downloaded() {
        std::lock_guard<std::mutex> hold(lock);
        return download;
    }

    bool Apply() {
        std::filesystem::path path;
        {
            std::lock_guard<std::mutex> hold(lock);
            if (download != Download::Ready) return false;
            path = readyPath;
        }
        std::filesystem::path folder = Helpers::exeDirectory();
        std::wstring pid = std::to_wstring(GetCurrentProcessId());
        std::wstring file, arguments;
        if (Installed(folder)) {
            // the installer waits for this process to end, then starts the app
            file = path.wstring();
            arguments = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE=1 /WAITPID=" + pid;
        }
        else {
            if (!PutInPlace(path, folder)) {
                std::cerr << "Update check: couldn't put the new files in " << Helpers::utf8(folder) << std::endl;
                setDownload(Download::Failed);
                return false;
            }
            file = (folder / EXE).wstring();
            arguments = L"--after " + pid;
        }
        SHELLEXECUTEINFOW info = {};
        info.cbSize = sizeof(info);
        info.lpVerb = L"open";
        info.lpFile = file.c_str();
        info.lpParameters = arguments.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            std::cerr << "Update check: couldn't start " << Helpers::utf8(file) << std::endl;
            setDownload(Download::Failed);
            return false;
        }
        return true;
    }
}
