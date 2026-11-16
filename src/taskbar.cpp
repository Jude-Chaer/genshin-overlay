#include "taskbar.hpp"
#include "helpers.hpp"
#include <windows.h>
#include <atomic>
#include <string>

namespace Taskbar {
    static const wchar_t* WINDOW_CLASS = L"PocketEsoptron";
    static const wchar_t* MUTEX_NAME = L"Local\\PocketEsoptron";
    static const UINT OPEN_MENU = WM_APP + 1;

    static HANDLE mutex = nullptr;
    static HWND window = nullptr;
    static std::atomic<Ask> asked{ Ask::None };

    void WaitForCopy(unsigned long processId) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (!process) return;
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
    }

    bool AnotherCopyRuns() {
        mutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
        // a copy running as admin denies a normal one access to its mutex
        bool taken = mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS;
        if (!taken) return false;
        // the running copy makes its window once the overlay is up
        HWND other = FindWindowW(WINDOW_CLASS, nullptr);
        for (int i = 0; i < 100 && !other; i++) {
            Sleep(100);
            other = FindWindowW(WINDOW_CLASS, nullptr);
        }
        if (other) PostMessageW(other, OPEN_MENU, 0, 0);
        return true;
    }

    bool RestartAsAdmin() {
        return Helpers::relaunchAsAdmin(L"--after " + std::to_wstring(GetCurrentProcessId()));
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_SYSCOMMAND:
            switch (wParam & 0xFFF0) {
            // pressing the button, or picking it in Alt+Tab, asks a minimized window to restore
            case SC_RESTORE:
            case SC_MAXIMIZE:
                asked = Ask::ToggleMenu;
                return 0;
            case SC_CLOSE:
                asked = Ask::Quit;
                return 0;
            }
            break;
        case WM_CLOSE:
            asked = Ask::Quit;
            return 0;
        case OPEN_MENU:
            asked = Ask::OpenMenu;
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Start() {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = WINDOW_CLASS;
        windowClass.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        windowClass.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        RegisterClassExW(&windowClass);

        window = CreateWindowExW(WS_EX_APPWINDOW, WINDOW_CLASS, L"Pocket Esoptron",
            WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (!window) return;
        // a second copy started without admin can still reach one running as admin
        ChangeWindowMessageFilterEx(window, OPEN_MENU, MSGFLT_ALLOW, nullptr);
        ShowWindow(window, SW_SHOWMINNOACTIVE);
    }

    void Stop() {
        if (window) DestroyWindow(window);
        window = nullptr;
        if (mutex) CloseHandle(mutex);
        mutex = nullptr;
    }

    Ask Take() {
        return asked.exchange(Ask::None);
    }
}
