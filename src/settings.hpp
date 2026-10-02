#pragma once

// The app's own settings, saved in settings.json next to the exe.

namespace Settings {
    // Start as admin. Only needed to see M and Esc while the game has focus,
    // so the map overlay can close the moment the map does.
    extern bool runAsAdmin;

    // Switches for parts of the map following, in the menu under Advanced.
    // patches that only see open water don't count when following
    extern bool ignoreSea;
    // the map is drawn where it will be when the picture shows up, not where it was seen
    extern bool guessAhead;
    // nothing is guessed ahead while the map zooms
    extern bool steadyZoom;
    // SIFT looks again when the map stands still but looks different, as after picking a floor
    extern bool lookAgainStill;
    // following runs on its own thread instead of in the draw loop
    extern bool followOnThread;
    // the patches are shrunk on the GPU before they're read back
    extern bool shrinkOnGpu;

    extern void Load();
    extern void Save();
}
