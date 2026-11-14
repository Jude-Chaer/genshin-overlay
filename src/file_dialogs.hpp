#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// Windows' own open and save dialogs, each on a thread of its own so the
// overlay keeps drawing while one is open. Only the file the player picked is
// read or written, so no file permission is needed for it.

namespace FileDialogs {
    // bigger than anything an extension should load or save in one go
    inline const size_t MaxSize = 64 * 1024 * 1024;

    // one type in the dialog's list, like { L"JSON", L"*.json" }; pattern may
    // hold more than one, split by ';'
    struct Filter {
        std::wstring label;
        std::wstring pattern;
    };

    // Written by the dialog's thread, read once done is set. On success error
    // is empty, name is the file's name without where it is, and data what an
    // open read. error: "cancelled", "too_big" or "failed".
    struct Answer {
        std::string error;
        std::string name;
        std::string data;
        std::atomic<bool> done{ false };
    };

    // Shows the dialog and gives the picked path, empty when it was closed.
    // Windows' dialog by default, the tests put their own here.
    using Picker = std::function<std::filesystem::path(HWND owner, bool save, const std::wstring& name, const std::optional<Filter>& filter)>;
    extern Picker picker;

    // Opens one, nullptr while another is open. data is what a save writes,
    // at most MaxSize.
    extern std::shared_ptr<Answer> start(HWND owner, bool save, const std::string& name, const std::string& data,
        const std::optional<Filter>& filter);
    // A filter from Lua's { "JSON", "*.json" }: a label of at most 64 letters
    // and patterns like *.json;*.txt. Nothing when it isn't one.
    extern std::optional<Filter> filterFrom(const std::string& label, const std::string& pattern);
    // a dialog is open now
    extern bool open();
    // Hands the focus back to the game once a dialog closes. The overlay sets
    // it, in the tests it's empty so they never touch the game's window.
    extern std::function<void()> giveFocusBack;
    // each turn
    extern void update();
}
