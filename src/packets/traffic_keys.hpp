#pragma once
#include <filesystem>
#include <string>
#include "session_key.hpp"
#include "../net_stop.hpp"

// The keys the game's traffic is read with, kept up to date without a new
// overlay. packets/ comes with the overlay. The traffic-keys workflow checks
// Irminsul's dispatch keys and auto-artifactarium's login keys every hour and
// puts them on the "map-index" release as one traffic-keys.json:
//   {"format": 1, "built": "2026-11-08T12:17:00Z",
//    "dispatch": {"13980": "base64 key", ...},   Irminsul's keys/gi.json
//    "rsa": {"4": "-----BEGIN RSA PRIVATE KEY-----...", ...},
//    "from": {"irminsul": "commit", "auto-artifactarium": "commit"}}
// The last good one is kept as packets/traffic-keys.json. It's small, so it
// comes whole every time and is compared as it is, with no meta file.

namespace TrafficKeys {
    inline constexpr int FORMAT = 1;
    inline const std::string FILE = "traffic-keys.json";

    // A traffic-keys.json into out. False with the problem when it isn't one
    // this overlay can use: every key has to be good, and every login key load.
    extern bool Read(const std::string& body, SessionKey::Keys& out, std::string& problem);
    // from's keys into into, from's key wins for the same id
    extern void Merge(SessionKey::Keys& into, const SessionKey::Keys& from);

    // packets/ with the saved traffic-keys.json on top. False as for
    // SessionKey::LoadKeys, a saved file that doesn't read is left out.
    extern bool Load(const std::filesystem::path& folder, SessionKey::Keys& out);

    //   Same: the release's file is the one saved
    //   New: it's another one, out has packets/ with it on top
    //   Missing: the release has none yet (404)
    //   Unreached: it couldn't be downloaded
    //   Broken: it came but Read refused it
    enum class Got { Same, New, Missing, Unreached, Broken };
    extern const char* GotCode(Got got);
    // Downloads the release's traffic-keys.json and saves it in folder when
    // it's new. stopper cuts it short from another thread.
    extern Got Download(const std::filesystem::path& folder, SessionKey::Keys& out, Net::Stopper* stopper = nullptr);
    // the same with the file already downloaded, for the tests
    extern Got Take(const std::filesystem::path& folder, int status, const std::string& body, SessionKey::Keys& out);
}
