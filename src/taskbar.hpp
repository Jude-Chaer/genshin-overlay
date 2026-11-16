#pragma once

// The app's taskbar button, its tray icon and the rule that only one copy
// runs. Both belong to a window of its own that stays minimized; pressing
// either, picking it in Alt+Tab or starting the exe again reaches the running
// copy as a request.
namespace Taskbar {
    // the Tray ones came from the tray icon; Close is the window's close (the
    // X in Alt+Tab, Close window on the button), Quit the tray list's Quit or
    // a start with --quit
    enum class Ask { None, ToggleMenu, OpenMenu, TrayToggleMenu, TrayOpenMenu, RestartAsAdmin, Close, Quit };

    // waits up to 30 s for the copy with this process id to end
    extern void WaitForCopy(unsigned long processId);
    // True when another copy runs: it's sent the request (OpenMenu or
    // RestartAsAdmin, nothing for None), this one should end. Otherwise this
    // copy now holds the name.
    extern bool AnotherCopyRuns(Ask request);
    // Asks the running copy to quit and waits up to 30 s for it to end. False
    // when none runs.
    extern bool QuitRunningCopy();
    // Starts this exe again as admin, waiting for this copy to end. False when
    // the prompt was declined.
    extern bool RestartAsAdmin();
    // the taskbar button with its right-click list's tasks, and the tray icon
    extern void Start();
    extern void Stop();
    // the taskbar button shown or taken off the taskbar, the tray icon stays
    extern void ShowButton(bool shown);
    // the taskbar button's middle on the screen, false when it can't be found
    extern bool ButtonCenter(int& x, int& y);
    // the tray icon's middle on the screen, false when it can't be found
    extern bool TrayIconCenter(int& x, int& y);

    // the flag a start with Windows passes
    extern const char* WithWindowsFlag;
    // the Run value in HKCU that starts this exe with Windows, off when Task
    // Manager turned it off
    extern bool StartsWithWindows();
    // writes the Run value with this exe's path and lets Task Manager start it, or deletes it
    extern void StartWithWindows(bool on);

    // the latest request since the last call, once
    extern Ask Take();
}
