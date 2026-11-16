#pragma once
#include <filesystem>
#include <optional>
#include <string>

// Asks GitHub for the app's releases and tells the menu when one is newer
// than this exe. A newer one is downloaded in the background, checked against
// the release's SHA256SUMS.txt and put in place on Restart to update.

namespace UpdateCheck {
    struct Release {
        // empty when there is none
        std::string version;
        std::string page;
        // the release's downloads, empty when it doesn't have them
        std::string installer;
        std::string portable;
        std::string sums;
    };

    enum class Download { None, Downloading, Ready, Failed };

    // On its own thread: asks now, then once a day while Check for updates is
    // on. A local build ("dev") never asks. Does nothing while it runs.
    extern void Start();
    extern void Stop();
    // the newer release last found
    extern Release Newer();
    // how far its download is
    extern Download Downloaded();
    // Runs the downloaded installer, or puts the unpacked portable files next
    // to the exe, and starts the new exe once this one has ended. The app
    // should quit when it's true.
    extern bool Apply();
    // the exe a portable update moved aside, deleted at start
    extern void RemoveOld();

    // True when a is a later version than b, both like "v1.2.3" or
    // "v1.2.3-beta.1": v0.2.0 is later than v0.2.0-beta.2, which is later
    // than v0.2.0-beta.1. False when either isn't shaped like that.
    extern bool Later(const std::string& a, const std::string& b);
    // The newest release in GitHub's release list (JSON) that's later than
    // current. Pre-releases count only when current is one.
    extern Release Pick(const std::string& releases, const std::string& current);
    // the installer's uninstaller is next to the exe
    extern bool Installed(const std::filesystem::path& exeFolder);
    // the sha256 (lowercase hex) SHA256SUMS.txt gives for name
    extern std::optional<std::string> SumFor(const std::string& sums, const std::string& name);
    // A portable zip unpacked into to: the folder holding PocketEsoptron.exe,
    // empty when it couldn't be.
    extern std::filesystem::path Unpack(const std::filesystem::path& zip, const std::filesystem::path& to);
    // Copies the unpacked files over folder, all but the portable file, after
    // moving folder's PocketEsoptron.exe aside as PocketEsoptron.exe.old.
    // False when it couldn't, with the exe put back.
    extern bool PutInPlace(const std::filesystem::path& unpacked, const std::filesystem::path& folder);
}
