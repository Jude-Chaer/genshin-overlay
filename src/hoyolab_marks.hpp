#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The points marked on HoYoLAB's map for the logged in account (HoyolabAccount),
// read and changed in the background. Before a change the marks there are saved
// to hoyolab-backups/ next to the exe, and anything that would unmark a point
// on HoYoLAB waits for the user to say yes in a window drawn here, an extension
// can't answer it.

namespace HoyolabMarks {
    struct Role {
        std::string uid;
        std::string nickname;
        std::string server;
        int level = 0;
    };

    struct Result {
        bool ok = false;
        std::string error;
        // HoYoLAB wants a captcha before it takes more changes
        bool captcha = false;
        // what's marked on HoYoLAB, every map together (fetch only)
        std::vector<int64_t> marks;
        int added = 0;
        int removed = 0;
        // the unmarks the user said no to, they stay marked on HoYoLAB
        std::vector<int64_t> kept;
        // points not in the item files, so no map to send them with
        std::vector<int64_t> unknown;
        // the backup saved before this job, empty when it didn't need one
        std::string backup;
        // the account's game characters (Roles only)
        std::vector<Role> roles;
    };

    struct Backup {
        std::string name;
        int count = 0;
    };

    extern void Init();
    extern void Shutdown();

    // each gives back a job, its result comes from GetResult
    // with backup the marks are saved to a backup too
    extern int Fetch(bool backup = false);
    extern int Roles();
    extern int Change(const std::vector<int64_t>& add, const std::vector<int64_t>& remove);
    // marks everything in the backup that isn't marked anymore, unmarks nothing
    extern int Restore(const std::string& backup);

    // false while the job runs, after that it gives the result once
    extern bool GetResult(int job, Result& out);
    extern bool Busy();
    // newest first
    extern std::vector<Backup> Backups();

    // the yes/no window for unmarking, every frame
    extern void DrawConfirm();
    // a question is waiting, the overlay has to draw even with the map closed
    extern bool NeedsDrawing();
}
