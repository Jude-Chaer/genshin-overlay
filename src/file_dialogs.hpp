#pragma once
#include <string>
#include <windows.h>

// Windows' own save and open dialogs, each on a thread of its own so the
// overlay keeps drawing while one is open. Only the file the user picked is
// written or read, so an extension needs no permission for files outside its folder.

namespace FileDialogs {
    struct Result {
        // false when the dialog was closed without picking a file
        bool picked = false;
        bool ok = false;
        // just the file's name, not where it is
        std::string name;
        std::string data;
    };

    // 0 while another dialog is open
    extern int Save(HWND owner, const std::string& suggestedName, const std::string& data);
    extern int Open(HWND owner);
    // false while the dialog is open, after that the result once
    extern bool GetResult(int dialog, Result& out);
}
