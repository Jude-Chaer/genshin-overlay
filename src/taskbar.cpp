#include "taskbar.hpp"
#include "helpers.hpp"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <UIAutomation.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace Taskbar {
    static const wchar_t* WINDOW_CLASS = L"PocketEsoptron";
    static const wchar_t* MUTEX_NAME = L"Local\\PocketEsoptron";
    static const UINT OPEN_MENU = WM_APP + 1;
    static const UINT RESTART_AS_ADMIN = WM_APP + 2;
    static const UINT TRAY = WM_APP + 3;
    static const UINT QUIT = WM_APP + 4;
    enum TrayItem : UINT { TRAY_OPEN_MENU = 1, TRAY_RESTART_AS_ADMIN, TRAY_QUIT };
    // PKEY_Title, a jump list task's name
    static const PROPERTYKEY TITLE = { { 0xF29F85E0, 0x4FF9, 0x1068, { 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 } }, 2 };

    static HANDLE mutex = nullptr;
    static HWND window = nullptr;
    static std::atomic<Ask> asked{ Ask::None };
    static NOTIFYICONDATAW trayIcon = {};
    // sent to every window when Explorer starts again, the tray icon has to be added again
    static UINT taskbarCreated = 0;
    // the menu's picture, BGRA with its alpha already applied, top row first
    static std::vector<unsigned char> picture;
    static int pictureWidth = 0;
    static int pictureHeight = 0;

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
        for (int i = 0; i < 100 && !other && request != Ask::None; i++) {
            Sleep(100);
            other = FindWindowW(WINDOW_CLASS, nullptr);
        }
        if (other && request != Ask::None) PostMessageW(other, request == Ask::RestartAsAdmin ? RESTART_AS_ADMIN : OPEN_MENU, 0, 0);
        return true;
    }

    bool QuitRunningCopy() {
        HWND other = FindWindowW(WINDOW_CLASS, nullptr);
        if (!other) return false;
        DWORD processId = 0;
        GetWindowThreadProcessId(other, &processId);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
        PostMessageW(other, QUIT, 0, 0);
        if (process) {
            WaitForSingleObject(process, 30000);
            CloseHandle(process);
        }
        else {
            for (int i = 0; i < 300 && IsWindow(other); i++) Sleep(100);
        }
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
        if (picked == TRAY_OPEN_MENU) asked = Ask::TrayOpenMenu;
        else if (picked == TRAY_RESTART_AS_ADMIN) asked = Ask::RestartAsAdmin;
        else if (picked == TRAY_QUIT) asked = Ask::Quit;
    }

    // the picture shrunk to fit in maxWidth by maxHeight, each pixel the
    // average of the ones it covers
    static HBITMAP pictureBitmap(int maxWidth, int maxHeight) {
        double scale = std::min({ 1.0, (double)maxWidth / pictureWidth, (double)maxHeight / pictureHeight });
        int width = std::max(1, (int)(pictureWidth * scale));
        int height = std::max(1, (int)(pictureHeight * scale));
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return nullptr;
        unsigned char* out = (unsigned char*)bits;
        for (int y = 0; y < height; y++) {
            int top = y * pictureHeight / height;
            int bottom = std::max(top + 1, (y + 1) * pictureHeight / height);
            for (int x = 0; x < width; x++) {
                int left = x * pictureWidth / width;
                int right = std::max(left + 1, (x + 1) * pictureWidth / width);
                unsigned sum[4] = {};
                for (int py = top; py < bottom; py++) {
                    for (int px = left; px < right; px++) {
                        const unsigned char* pixel = &picture[((size_t)py * pictureWidth + px) * 4];
                        for (int c = 0; c < 4; c++) sum[c] += pixel[c];
                    }
                }
                unsigned count = (unsigned)((bottom - top) * (right - left));
                for (int c = 0; c < 4; c++) out[((size_t)y * width + x) * 4 + c] = (unsigned char)(sum[c] / count);
            }
        }
        return bitmap;
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
                asked = Ask::Close;
                return 0;
            }
            break;
        case WM_CLOSE:
            asked = Ask::Close;
            return 0;
        // the button's preview and Alt+Tab, lParam holds the biggest size it takes
        case WM_DWMSENDICONICTHUMBNAIL:
            if (!picture.empty()) {
                HBITMAP bitmap = pictureBitmap(HIWORD(lParam), LOWORD(lParam));
                if (bitmap) {
                    DwmSetIconicThumbnail(hwnd, bitmap, 0);
                    DeleteObject(bitmap);
                }
            }
            return 0;
        // hovering the preview: the menu where it was on the screen
        case WM_DWMSENDICONICLIVEPREVIEWBITMAP:
            if (!picture.empty()) {
                HBITMAP bitmap = pictureBitmap(pictureWidth, pictureHeight);
                if (bitmap) {
                    DwmSetIconicLivePreviewBitmap(hwnd, bitmap, nullptr, 0);
                    DeleteObject(bitmap);
                }
            }
            return 0;
        case OPEN_MENU:
            asked = Ask::OpenMenu;
            return 0;
        case RESTART_AS_ADMIN:
            asked = Ask::RestartAsAdmin;
            return 0;
        case QUIT:
            asked = Ask::Quit;
            return 0;
        case TRAY:
            switch (LOWORD(lParam)) {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                asked = Ask::TrayToggleMenu;
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
        ChangeWindowMessageFilterEx(window, QUIT, MSGFLT_ALLOW, nullptr);
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

    bool ButtonShown() {
        return window && IsWindowVisible(window);
    }

    bool HasPicture() {
        return !picture.empty();
    }

    void SetPicture(std::vector<unsigned char> bgra, int width, int height, int screenX, int screenY) {
        if (!window || width <= 0 || height <= 0 || bgra.size() != (size_t)width * height * 4) return;
        bool first = picture.empty();
        picture = std::move(bgra);
        pictureWidth = width;
        pictureHeight = height;

        // the hover preview shows the picture where the window would be restored;
        // that spot is in work area coordinates
        WINDOWPLACEMENT placement = { sizeof(placement) };
        MONITORINFO monitor = { sizeof(monitor) };
        POINT corner = { screenX, screenY };
        if (GetWindowPlacement(window, &placement) && GetMonitorInfoW(MonitorFromPoint(corner, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            int x = screenX - (monitor.rcWork.left - monitor.rcMonitor.left);
            int y = screenY - (monitor.rcWork.top - monitor.rcMonitor.top);
            placement.rcNormalPosition = { x, y, x + width, y + height };
            placement.showCmd = SW_SHOWMINNOACTIVE;
            SetWindowPlacement(window, &placement);
        }

        if (first) {
            BOOL on = TRUE;
            DwmSetWindowAttribute(window, DWMWA_FORCE_ICONIC_REPRESENTATION, &on, sizeof(on));
            DwmSetWindowAttribute(window, DWMWA_HAS_ICONIC_BITMAP, &on, sizeof(on));
        }
        else DwmInvalidateIconicBitmaps(window);
    }

    // found through UI Automation: the taskbar's button named after the app
    bool ButtonCenter(int& x, int& y) {
        HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!bar) return false;
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool found = false;
        IUIAutomation* automation = nullptr;
        IUIAutomationElement* root = nullptr;
        IUIAutomationCondition* isButton = nullptr;
        IUIAutomationElementArray* buttons = nullptr;
        VARIANT button;
        button.vt = VT_I4;
        button.lVal = UIA_ButtonControlTypeId;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
            && SUCCEEDED(automation->ElementFromHandle(bar, &root))
            && SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, button, &isButton))
            && SUCCEEDED(root->FindAll(TreeScope_Descendants, isButton, &buttons))) {
            int count = 0;
            buttons->get_Length(&count);
            for (int i = 0; i < count && !found; i++) {
                IUIAutomationElement* element = nullptr;
                if (FAILED(buttons->GetElement(i, &element))) continue;
                BSTR name = nullptr;
                RECT rect = {};
                // e.g. "Pocket Esoptron - 1 running window"
                if (SUCCEEDED(element->get_CurrentName(&name)) && name && wcsncmp(name, L"Pocket Esoptron", 15) == 0
                    && SUCCEEDED(element->get_CurrentBoundingRectangle(&rect)) && rect.right > rect.left) {
                    x = (rect.left + rect.right) / 2;
                    y = (rect.top + rect.bottom) / 2;
                    found = true;
                }
                SysFreeString(name);
                element->Release();
            }
        }
        if (buttons) buttons->Release();
        if (isButton) isButton->Release();
        if (root) root->Release();
        if (automation) automation->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        return found;
    }

    bool TrayIconCenter(int& x, int& y) {
        if (!trayIcon.hWnd) return false;
        NOTIFYICONIDENTIFIER id = { sizeof(id) };
        id.hWnd = trayIcon.hWnd;
        id.uID = trayIcon.uID;
        RECT rect;
        if (FAILED(Shell_NotifyIconGetRect(&id, &rect))) return false;
        x = (rect.left + rect.right) / 2;
        y = (rect.top + rect.bottom) / 2;
        return true;
    }

    const char* WithWindowsFlag = "--with-windows";
    static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static const wchar_t* APPROVED_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    static const wchar_t* RUN_NAME = L"PocketEsoptron";

    bool StartsWithWindows() {
        if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_NAME, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) != ERROR_SUCCESS) return false;
        // Task Manager's switch: the first byte is odd when it's off
        BYTE approved[12] = {};
        DWORD size = sizeof(approved);
        if (RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_NAME, RRF_RT_REG_BINARY, nullptr, approved, &size) != ERROR_SUCCESS) return true;
        return size == 0 || (approved[0] & 1) == 0;
    }

    void StartWithWindows(bool on) {
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        if (on) {
            wchar_t path[MAX_PATH];
            DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (length && length < MAX_PATH) {
                std::string flag = WithWindowsFlag;
                std::wstring command = L"\"" + std::wstring(path, length) + L"\" " + std::wstring(flag.begin(), flag.end());
                RegSetValueExW(key, RUN_NAME, 0, REG_SZ, (const BYTE*)command.c_str(), (DWORD)((command.size() + 1) * sizeof(wchar_t)));
            }
        }
        else RegDeleteValueW(key, RUN_NAME);
        RegCloseKey(key);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_NAME);
    }

    Ask Take() {
        return asked.exchange(Ask::None);
    }
}
