#pragma once
#include <string>

// Asks GitHub for the app's releases and tells the menu when one is newer
// than this exe. Nothing is downloaded.

namespace UpdateCheck {
    struct Release {
        // empty when there is none
        std::string version;
        std::string page;
    };

    // On its own thread: asks now, then once a day while Check for updates is
    // on. A local build ("dev") never asks. Does nothing while it runs.
    extern void Start();
    extern void Stop();
    // the newer release last found
    extern Release Newer();

    // True when a is a later version than b, both like "v1.2.3" or
    // "v1.2.3-beta.1": v0.2.0 is later than v0.2.0-beta.2, which is later
    // than v0.2.0-beta.1. False when either isn't shaped like that.
    extern bool Later(const std::string& a, const std::string& b);
    // The newest release in GitHub's release list (JSON) that's later than
    // current. Pre-releases count only when current is one.
    extern Release Pick(const std::string& releases, const std::string& current);
}
