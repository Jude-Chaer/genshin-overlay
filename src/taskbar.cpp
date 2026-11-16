#include "taskbar.hpp"
#include "helpers.hpp"
#include <windows.h>
#include <shobjidl.h>
#include <atomic>
#include <string>

namespace Taskbar {
    static const wchar_t* WINDOW_CLASS = L"PocketEsoptron";
    static const wchar_t* MUTEX_NAME = L"Local\\PocketEsoptron";
    static const UINT OPEN_MENU = WM_APP + 1;
    static const UINT RESTART_AS_ADMIN = WM_APP + 2;
    // PKEY_Title, a jump list task's name
    static const PROPERTYKEY TITLE = { { 0xF29F85E0, 0x4FF9, 0x1068, { 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 } }, 2 };

    static HANDLE mutex = nullptr;
    static HWND window = nullptr;
    static std::atomic<Ask> asked{ Ask::None };

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
