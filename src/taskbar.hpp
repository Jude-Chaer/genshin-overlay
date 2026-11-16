#pragma once

// The app's taskbar button and the rule that only one copy runs. The button
// belongs to a window of its own that stays minimized; pressing it, picking it
// in Alt+Tab or starting the exe again reaches the running copy as a request.
namespace Taskbar {
    enum class Ask { None, ToggleMenu, OpenMenu, Quit };

    // waits up to 30 s for the copy with this process id to end
    extern void WaitForCopy(unsigned long processId);
    // True when another copy runs: it's asked to open its menu, this one
    // should end. Otherwise this copy now holds the name.
    extern bool AnotherCopyRuns();
    // Starts this exe again as admin, waiting for this copy to end. False when
    // the prompt was declined.
    extern bool RestartAsAdmin();
    extern void Start();
    extern void Stop();

    // the latest request since the last call, once
    extern Ask Take();
}
