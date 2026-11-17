#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "accounts.hpp"
#include "net.hpp"

// The points marked on HoYoLAB Map and appsample for the logged in
// accounts, read and changed by the overlay on a worker, one job at a time.
// Ids are always HoYoLAB point ids; appsample's go through each point's pair
// in the item files. Before every write the site's marks are saved to a dated
// backup in the data folder. A change that unmarks more than one point waits for
// the player's answer to the overlay's question, then reads the marks again;
// other extensions' jobs go on meanwhile.

namespace Marks {
    using Site = Accounts::Site;

    // Restore adds back what a backup of the logged in account had, it never unmarks
    enum class Action { Fetch, Change, Restore };

    struct SiteResult {
        Site site = Site::HoyolabMap;
        bool ok = false;
        // "logged_out", "captcha", "account_changed", "no_items", "failed",
        // "backup_failed", "cancelled", "no_backup"; empty when ok
        std::string error;
        // what went wrong, for the log
        std::string why;
        // Fetch: every point marked there, by HoYoLAB id
        std::vector<int64_t> marks;
        int added = 0;
        int removed = 0;
        // unmarks the player said no to, they stay marked there
        std::vector<int64_t> kept;
        // not sent: not in the item files, no appsample pair, or under
        // appsample's level 5
        std::vector<int64_t> skipped;
        // the backup made before this write, empty when nothing was written
        std::string backup;
    };

    struct Result {
        // a site each, in the order asked
        std::vector<SiteResult> sites;
    };

    struct Request {
        std::vector<Site> sites;
        Action action = Action::Fetch;
        std::vector<int64_t> add, remove;
        // Restore: the backup's name, from BackupsOf
        std::string backup;
        // who asked, for the question and Forget; nullptr for the overlay
        const void* owner = nullptr;
    };

    // How many jobs one owner may have waiting or not collected
    inline const int MaxWaiting = 8;
    // a result nobody took is dropped after this long
    inline const auto KeepResult = std::chrono::minutes(5);

    void Init();
    void Shutdown();
    // On the frame's thread: hands a waiting question to Asks, drops old
    // results, asks for the HoYoLAB nickname after a log in
    void Frame();

    // the job's id, 0 when the owner has MaxWaiting already
    int Start(const Request& request);
    // the result once, nothing while it runs or for another owner's job
    std::optional<Result> Take(int id, const void* owner);
    bool Done(int id);
    int WaitingOf(const void* owner);
    // its jobs and results go, a question it's waiting on is cancelled
    void Forget(const void* owner);
    // its jobs for the site that haven't started end as "cancelled" for
    // every site in them, a question one is waiting on goes away; one
    // already running finishes as it is
    void Cancel(const void* owner, Site site);

    // The pieces below are the worker's, here to be tested.

    // A HoYoLAB answer: retcode 0 is fine, -100, -2004 and 10001 mean logged
    // out, -5001 a captcha
    struct HoyolabAnswer {
        bool read = false;
        int retcode = -1;
        std::string message;
    };
    HoyolabAnswer ReadHoyolab(const Net::Response& response);
    // adds mark_map_point_list's point ids to out, false when the answer isn't one
    bool ReadHoyolabList(const Net::Response& response, std::vector<int64_t>& out);
    // batch_mark_map_point's body
    std::string HoyolabBatch(int map, const std::vector<int64_t>& ids, bool unmark);
    // the player's HoYoLAB nickname from getUserFullInfo, "" when it has none
    std::string ReadHoyolabName(const Net::Response& response);

    // appsample's users/{uid} document: adds its markerIds to out. 404 is a
    // player with nothing marked yet. False when it isn't one.
    bool ReadAppsampleUser(const Net::Response& response, std::vector<int64_t>& out);
    // a Firestore commit that adds or removes these ids from markerIds and
    // leaves the rest of the list and the document as they are
    std::string AppsampleCommit(const std::string& uid, const std::vector<int64_t>& ids, bool remove);

    // Backups: one file per write, "<site>-<date>.json" in the folder, with
    // the account as the sha256 of "site:account".
    struct Backup {
        std::string name;
        Site site = Site::HoyolabMap;
        int count = 0;
        std::string account;
    };
    std::filesystem::path BackupFolder();
    // backups kept per site and account, the oldest go
    inline const size_t KeepBackups = 20;
    // the file's name, "" when it couldn't be written; the account's backups
    // past KeepBackups on that site are deleted
    std::string SaveBackup(const std::filesystem::path& folder, Site site, const std::string& account, const std::vector<int64_t>& marks);
    // newest first
    std::vector<Backup> Backups(const std::filesystem::path& folder);
    // the marks in a backup of this account, false for another account's or one that doesn't read
    bool ReadBackup(const std::filesystem::path& folder, const std::string& name, Site site, const std::string& account,
        std::vector<int64_t>& out);
    std::string AccountHash(Site site, const std::string& account);
    // the backups of the account logged in on the site now, newest first
    std::vector<Backup> BackupsOf(Site site);

    // What a change does on one site, given what's marked there: the adds
    // and removes that change something, by HoYoLAB id
    struct Plan {
        std::vector<int64_t> add, remove;
    };
    Plan PlanChange(const std::vector<int64_t>& marked, const std::vector<int64_t>& add, const std::vector<int64_t>& remove);

    // The login a job uses on a site: the header lines that carry it, the
    // account it's for, and what Accounts::Refused compares
    struct Login {
        std::string headers;
        std::string account;
        std::string secret;
    };
    using LoginFor = std::function<std::optional<Login>(Site site)>;
    // sends a request with a login, as Net::sendLogin
    using Sender = std::function<bool(const std::string& method, const std::string& url, const std::string& headers,
        const std::string& body, Net::Response& out)>;
    // tests put their own here, nullptr gives back the real ones; the
    // backups go to folder, empty for BackupFolder
    void SetForTests(LoginFor login, Sender sender, const std::filesystem::path& folder = {});
}
