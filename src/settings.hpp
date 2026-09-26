#pragma once

// The app's own settings, saved in settings.json next to the exe.

namespace Settings {
    // Start as admin. Only needed to see M and Esc while the game has focus,
    // so the map overlay can close the moment the map does.
    extern bool runAsAdmin;

    extern void Load();
    extern void Save();
}
