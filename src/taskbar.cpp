#include "taskbar.hpp"
#include "helpers.hpp"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <atomic>
#include <string>

namespace Taskbar {
    static const wchar_t* WINDOW_CLASS = L"PocketEsoptron";
    static const wchar_t* MUTEX_NAME = L"Local\\PocketEsoptron";
    static const UINT OPEN_MENU = WM_APP + 1;
    static const UINT RESTART_AS_ADMIN = WM_APP + 2;
    static const UINT TRAY = WM_APP + 3;
    enum TrayItem : UINT { TRAY_OPEN_MENU = 1, TRAY_RESTART_AS_ADMIN, TRAY_QUIT };
    // PKEY_Title, a jump list task's name
    static const PROPERTYKEY TITLE = { { 0xF29F85E0, 0x4FF9, 0x1068, { 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 } }, 2 };

    static HANDLE mutex = nullptr;
    static HWND window = nullptr;
    static std::atomic<Ask> asked{ Ask::None };
    static NOTIFYICONDATAW trayIcon = {};
    // sent to every window when Explorer starts again, the tray icon has to be added again
    static UINT taskbarCreated = 0;

    void WaitForCopy(unsigned long processId) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (!process) return;
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
    }

    bool AnotherCopyRuns(Ask request) {
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
        if (other) PostMessageW(other, request == Ask::RestartAsAdmin ? RESTART_AS_ADMIN : OPEN_MENU, 0, 0);
        return true;
    }

    bool RestartAsAdmin() {
        return Helpers::relaunchAsAdmin(L"--after " + std::to_wstring(GetCurrentProcessId()));
    }

    static void addTrayIcon() {
        Shell_NotifyIconW(NIM_ADD, &trayIcon);
        Shell_NotifyIconW(NIM_SETVERSION, &trayIcon);
    }

    // the tray icon's right-click list, at x, y on the screen
    static void showTrayList(int x, int y) {
        HMENU list = CreatePopupMenu();
        if (!list) return;
        AppendMenuW(list, MF_STRING, TRAY_OPEN_MENU, L"Open menu");
        if (!Helpers::isElevated()) AppendMenuW(list, MF_STRING, TRAY_RESTART_AS_ADMIN, L"Restart as admin");
        AppendMenuW(list, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(list, MF_STRING, TRAY_QUIT, L"Quit");
        // the list closes on a click elsewhere only while its window is in front
        SetForegroundWindow(window);
        UINT picked = TrackPopupMenuEx(list, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, x, y, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(list);
        if (picked == TRAY_OPEN_MENU) asked = Ask::OpenMenu;
        else if (picked == TRAY_RESTART_AS_ADMIN) asked = Ask::RestartAsAdmin;
        else if (picked == TRAY_QUIT) asked = Ask::Quit;
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
        case RESTART_AS_ADMIN:
            asked = Ask::RestartAsAdmin;
            return 0;
        case TRAY:
            switch (LOWORD(lParam)) {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                asked = Ask::ToggleMenu;
                break;
            case WM_CONTEXTMENU:
                showTrayList(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
                break;
            }
            return 0;
        }
        if (message == taskbarCreated && taskbarCreated) {
            addTrayIcon();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static IShellLinkW* task(const std::wstring& exe, const wchar_t* arguments, const wchar_t* title) {
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return nullptr;
        link->SetPath(exe.c_str());
        link->SetArguments(arguments);
        link->SetIconLocation(exe.c_str(), 0);
        IPropertyStore* properties = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&properties)))) {
            PROPVARIANT value = {};
            value.vt = VT_LPWSTR;
            size_t bytes = (wcslen(title) + 1) * sizeof(wchar_t);
            value.pwszVal = (wchar_t*)CoTaskMemAlloc(bytes);
            if (value.pwszVal) {
                memcpy(value.pwszVal, title, bytes);
                properties->SetValue(TITLE, value);
                properties->Commit();
            }
            PropVariantClear(&value);
            properties->Release();
        }
        return link;
    }

    // Restart as admin when not admin yet: it starts the exe, which hands the
    // request to the running copy
    static void setTasks() {
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ICustomDestinationList* list = nullptr;
        IObjectCollection* tasks = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))
            && SUCCEEDED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks)))) {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring exe = path;
            IShellLinkW* restart = Helpers::isElevated() ? nullptr : task(exe, L"--restart-as-admin", L"Restart as admin");
            if (restart) {
                tasks->AddObject(restart);
                restart->Release();
            }
            UINT slots = 0;
            IObjectArray* removed = nullptr;
            IObjectArray* taskArray = nullptr;
            if (SUCCEEDED(list->BeginList(&slots, IID_PPV_ARGS(&removed)))) {
                if (SUCCEEDED(tasks->QueryInterface(IID_PPV_ARGS(&taskArray)))) {
                    list->AddUserTasks(taskArray);
                    taskArray->Release();
                }
                list->CommitList();
                removed->Release();
            }
        }
        if (tasks) tasks->Release();
        if (list) list->Release();
        if (SUCCEEDED(com)) CoUninitialize();
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
        ChangeWindowMessageFilterEx(window, RESTART_AS_ADMIN, MSGFLT_ALLOW, nullptr);
        ShowWindow(window, SW_SHOWMINNOACTIVE);
        setTasks();

        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (taskbarCreated) ChangeWindowMessageFilterEx(window, taskbarCreated, MSGFLT_ALLOW, nullptr);
        trayIcon.cbSize = sizeof(trayIcon);
        trayIcon.hWnd = window;
        trayIcon.uID = 1;
        trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        trayIcon.uCallbackMessage = TRAY;
        trayIcon.hIcon = windowClass.hIconSm;
        wcscpy_s(trayIcon.szTip, L"Pocket Esoptron");
        trayIcon.uVersion = NOTIFYICON_VERSION_4;
        addTrayIcon();
    }

    void Stop() {
        if (trayIcon.hWnd) Shell_NotifyIconW(NIM_DELETE, &trayIcon);
        trayIcon.hWnd = nullptr;
        if (window) DestroyWindow(window);
        window = nullptr;
        if (mutex) CloseHandle(mutex);
        mutex = nullptr;
    }

    void ShowButton(bool shown) {
        if (window) ShowWindow(window, shown ? SW_SHOWMINNOACTIVE : SW_HIDE);
    }

    Ask Take() {
        return asked.exchange(Ask::None);
    }
}
