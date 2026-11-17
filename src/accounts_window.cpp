#include "accounts.hpp"
#include "controls.hpp"
#include "credentials.hpp"
#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>

// The login window: a WebView2 window on its own thread, with a fresh browser
// profile that's deleted after, so nothing of the site's session stays on disk
// but what Accounts keeps. Only the site's pages, its login and the sites it
// logs in with can be opened in it.

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace Accounts {
    static const wchar_t* WINDOW_CLASS = L"genshin-overlay-login";
    static const wchar_t* HOYOLAB_MAP = L"https://act.hoyolab.com/ys/app/interactive-map/index.html?lang=en-us";
    static const wchar_t* HOYOLAB_COOKIES = L"https://act.hoyolab.com";
    static const wchar_t* APPSAMPLE_MAP = L"https://genshin-impact-map.appsample.com/";

    // Where the page and its frames may go; pictures and scripts on a page
    // aren't checked. Each with the name the strip gives it.
    struct AllowedSite {
        const wchar_t* host;
        const char* label;
    };
    static const AllowedSite HOYOLAB_SITES[] = {
        { L"hoyolab.com", "HoYoLAB" }, { L"hoyoverse.com", "HoYoLAB" }, { L"mihoyo.com", "HoYoLAB" }, { L"geetest.com", "HoYoLAB" },
        { L"accounts.google.com", "Google" }, { L"apple.com", "Apple" }, { L"facebook.com", "Facebook" },
        { L"accounts.youtube.com", "Google" }, { L"twitter.com", "X" }, { L"x.com", "X" }, { L"discord.com", "Discord" },
    };
    static const AllowedSite APPSAMPLE_SITES[] = {
        { L"appsample.com", "appsample" }, { L"genshin-impact.firebaseapp.com", "appsample" }, { L"accounts.google.com", "Google" },
        { L"accounts.youtube.com", "Google" },
    };

    // reads the user Firebase keeps in the page; the answer comes on the next look
    static const wchar_t* READ_FIREBASE_USER = LR"(
        (() => {
            const open = indexedDB.open('firebaseLocalStorageDb');
            open.onsuccess = () => {
                const db = open.result;
                if (!db.objectStoreNames.contains('firebaseLocalStorage')) return;
                const all = db.transaction('firebaseLocalStorage').objectStore('firebaseLocalStorage').getAll();
                all.onsuccess = () => {
                    const user = all.result.find(entry => String(entry.fbase_key).startsWith('firebase:authUser:'));
                    if (user) window.__overlayUser = JSON.stringify(user.value);
                };
            };
            const found = window.__overlayUser || '';
            window.__overlayUser = '';
            return found;
        })()
    )";

    enum class Mode { LogIn, Map };

    struct Popup {
        ComPtr<ICoreWebView2Controller> controller;
        HWND host = nullptr, line = nullptr, cancel = nullptr;
    };

    struct Session {
        Site site;
        Mode mode;
        HWND window = nullptr;
        ComPtr<ICoreWebView2Environment> environment;
        ComPtr<ICoreWebView2CookieManager> cookies;
        ComPtr<ICoreWebView2> page;
        // the window's own and the popups', by window
        std::map<HWND, Popup> windows;
        // the overlay's look when it opened
        Controls::Theme theme;
        HBRUSH ground = nullptr;
        bool checking = false;
        bool done = false;
    };
    static thread_local Session* session = nullptr;

    static std::thread windowThread;
    static std::atomic<bool> windowOpen = false;
    // the site of the open window, -1 for none
    static std::atomic<int> windowSite = -1;
    static std::atomic<HWND> mainWindow = nullptr;
    static std::atomic<DWORD> windowThreadId = 0;

    static constexpr UINT_PTR LOOK_TIMER = 1;
    static constexpr int CANCEL_ID = 1;

    static void wipe(std::wstring& secret) {
        if (!secret.empty()) SecureZeroMemory(&secret[0], secret.size() * sizeof(wchar_t));
        secret.clear();
    }

    static std::string narrow(const std::wstring& text) {
        if (text.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        std::string out(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size, nullptr, nullptr);
        return out;
    }

    static std::wstring widen(const std::string& text) {
        if (text.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
        return out;
    }

    static std::filesystem::path profileFolder() {
        PWSTR local = nullptr;
        std::filesystem::path folder;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            folder = std::filesystem::path(local) / "genshin-overlay" / "login-window";
        }
        CoTaskMemFree(local);
        return folder;
    }

    // the host of an https url, lowercase, empty for anything else
    static std::wstring hostOf(const std::wstring& url) {
        const std::wstring https = L"https://";
        if (url.compare(0, https.size(), https) != 0) return L"";
        size_t end = url.find_first_of(L"/\\?#:", https.size());
        std::wstring host = url.substr(https.size(), end == std::wstring::npos ? std::wstring::npos : end - https.size());
        // user:password@ in front of the host
        if (host.find(L'@') != std::wstring::npos) return L"";
        for (auto& c : host) c = towlower(c);
        return host;
    }

    // Google's sign-in on its country sites, where it sets the login too:
    // accounts.google.de, accounts.google.co.uk, accounts.google.com.sa
    static bool googleCountrySite(const std::wstring& host) {
        const std::wstring start = L"accounts.google.";
        if (host.compare(0, start.size(), start) != 0) return false;
        std::wstring rest = host.substr(start.size());
        if (rest.compare(0, 4, L"com.") == 0) rest = rest.substr(4);
        else if (rest.compare(0, 3, L"co.") == 0) rest = rest.substr(3);
        return rest.size() == 2 && iswalpha(rest[0]) && iswalpha(rest[1]);
    }

    static const AllowedSite* allowedSite(Site site, const std::wstring& url) {
        std::wstring host = hostOf(url);
        if (host.empty()) return nullptr;
        auto check = [&](const auto& list) -> const AllowedSite* {
            for (const AllowedSite& allowed : list) {
                std::wstring name = allowed.host;
                if (host == name) return &allowed;
                if (host.size() > name.size() && host.compare(host.size() - name.size(), name.size(), name) == 0
                    && host[host.size() - name.size() - 1] == L'.') return &allowed;
            }
            return nullptr;
        };
        const AllowedSite* found = site == Site::HoyolabMap ? check(HOYOLAB_SITES) : check(APPSAMPLE_SITES);
        static const AllowedSite GOOGLE = { L"accounts.google.com", "Google" };
        if (!found && googleCountrySite(host)) found = &GOOGLE;
        return found;
    }

    bool AllowedUrl(Site site, const std::wstring& url) {
        return url == L"about:blank" || allowedSite(site, url);
    }

    // the strip over the page: the site it's on, who sees what's typed, Cancel
    static HFONT stripFont() {
        static HFONT font = nullptr;
        if (!font) {
            NONCLIENTMETRICSW metrics = { sizeof(metrics) };
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
            font = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        return font;
    }

    static int padding() {
        return MulDiv(6, GetDpiForSystem(), 96);
    }

    static int lineHeight() {
        static int height = 0;
        if (!height) {
            HDC screen = GetDC(nullptr);
            HGDIOBJ before = SelectObject(screen, stripFont());
            TEXTMETRICW metrics = {};
            GetTextMetricsW(screen, &metrics);
            SelectObject(screen, before);
            ReleaseDC(nullptr, screen);
            height = metrics.tmHeight + metrics.tmExternalLeading;
        }
        return height;
    }

    static int stripHeight() {
        return lineHeight() * 2 + padding() * 3;
    }

    static void showSite(HWND window, const std::wstring& url) {
        if (!session) return;
        auto it = session->windows.find(window);
        if (it == session->windows.end()) return;
        const AllowedSite* site = allowedSite(session->site, url);
        std::wstring host = hostOf(url);
        SetWindowTextW(it->second.host, host.c_str());
        std::string line = site ? std::string("Only ") + site->label + " sees what you type here." : "";
        SetWindowTextW(it->second.line, widen(line).c_str());
    }

    static void layOut(HWND window) {
        if (!session) return;
        auto it = session->windows.find(window);
        if (it == session->windows.end()) return;
        RECT bounds;
        GetClientRect(window, &bounds);
        int strip = stripHeight(), pad = padding(), line = lineHeight(), button = MulDiv(80, GetDpiForSystem(), 96);
        int textWidth = std::max(0, (int)bounds.right - button - pad * 6);
        MoveWindow(it->second.host, pad * 2, pad + pad / 2, textWidth, line, TRUE);
        MoveWindow(it->second.line, pad * 2, pad + pad / 2 + line, textWidth, line, TRUE);
        MoveWindow(it->second.cancel, bounds.right - button - pad * 2, pad, button, strip - pad * 2, TRUE);
        if (it->second.controller) {
            RECT page = bounds;
            page.top = strip;
            it->second.controller->put_Bounds(page);
        }
    }

    static COLORREF colorOf(unsigned int rgba) {
        return RGB((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF);
    }

    // the title bar in the overlay's colours (Windows 11), dark or light with it
    static void paintTitleBar(HWND window) {
        BOOL dark = session->theme.dark;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        COLORREF caption = colorOf(session->theme.panel), text = colorOf(session->theme.text);
        DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
    }

    // Cancel as the overlay's plain button
    static void drawCancel(const DRAWITEMSTRUCT& item) {
        const Controls::Theme& theme = session->theme;
        bool pressed = item.itemState & ODS_SELECTED;
        HDC dc = item.hDC;
        FillRect(dc, &item.rcItem, session->ground);
        HBRUSH fill = CreateSolidBrush(colorOf(pressed ? theme.picked : theme.button));
        HPEN edge = CreatePen(PS_SOLID, 1, colorOf(theme.buttonLine));
        HGDIOBJ oldBrush = SelectObject(dc, fill), oldPen = SelectObject(dc, edge);
        int round = MulDiv(8, GetDpiForSystem(), 96);
        RoundRect(dc, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, round, round);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(fill);
        DeleteObject(edge);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, colorOf(theme.text));
        HGDIOBJ oldFont = SelectObject(dc, stripFont());
        RECT box = item.rcItem;
        DrawTextW(dc, L"Cancel", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
    }

    static HWND makeWindow(const wchar_t* title, int width, int height, HWND owner = nullptr) {
        int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        // over the overlay
        HWND window = CreateWindowExW(WS_EX_TOPMOST, WINDOW_CLASS, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window || !session) return window;
        paintTitleBar(window);
        Popup& popup = session->windows[window];
        HINSTANCE instance = GetModuleHandleW(nullptr);
        popup.host = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        popup.line = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        popup.cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, window, (HMENU)(INT_PTR)CANCEL_ID, instance, nullptr);
        for (HWND part : { popup.host, popup.line, popup.cancel }) SendMessageW(part, WM_SETFONT, (WPARAM)stripFont(), TRUE);
        layOut(window);
        return window;
    }

    // HoYoLAB: logged in once it has set both cookies
    static void checkCookies() {
        if (!session || session->site != Site::HoyolabMap || session->mode != Mode::LogIn || session->done || session->checking || !session->cookies) return;
        session->checking = true;
        session->cookies->GetCookies(HOYOLAB_COOKIES, Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                if (!session) return S_OK;
                session->checking = false;
                if (FAILED(result) || !list || session->done) return S_OK;

                std::string id, token;
                UINT count = 0;
                list->get_Count(&count);
                for (UINT i = 0; i < count; i++) {
                    ComPtr<ICoreWebView2Cookie> cookie;
                    if (FAILED(list->GetValueAtIndex(i, &cookie))) continue;
                    LPWSTR name = nullptr, value = nullptr;
                    cookie->get_Name(&name);
                    cookie->get_Value(&value);
                    if (name && value) {
                        std::wstring wideValue = value;
                        if (wcscmp(name, L"ltuid_v2") == 0) id = narrow(wideValue);
                        if (wcscmp(name, L"ltoken_v2") == 0) token = narrow(wideValue);
                        wipe(wideValue);
                        SecureZeroMemory(value, wcslen(value) * sizeof(wchar_t));
                    }
                    CoTaskMemFree(name);
                    CoTaskMemFree(value);
                }
                if (!id.empty() && !token.empty()) {
                    session->done = true;
                    FoundHoyolabMap(id, token);
                    PostMessageW(session->window, WM_CLOSE, 0, 0);
                }
                Credentials::Wipe(token);
                return S_OK;
            }).Get());
    }

    // appsample: logged in once Firebase has saved its user in the page
    static void checkFirebase() {
        if (!session || session->site != Site::Appsample || session->done || session->checking || !session->page) return;
        LPWSTR source = nullptr;
        session->page->get_Source(&source);
        bool onAppsample = source && hostOf(source) == L"genshin-impact-map.appsample.com";
        CoTaskMemFree(source);
        if (!onAppsample) return;
        session->checking = true;
        session->page->ExecuteScript(READ_FIREBASE_USER, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT result, LPCWSTR answer) -> HRESULT {
                if (!session) return S_OK;
                session->checking = false;
                if (FAILED(result) || !answer || session->done) return S_OK;
                // the script's string, as JSON
                std::wstring wide = answer;
                std::string text = narrow(wide);
                wipe(wide);
                nlohmann::json quoted = nlohmann::json::parse(text, nullptr, false);
                Credentials::Wipe(text);
                if (!quoted.is_string()) return S_OK;
                std::string value = quoted.get<std::string>();
                quoted = nullptr;
                if (value.empty()) return S_OK;
                std::optional<FirebaseUser> user = ReadFirebaseUser(value);
                Credentials::Wipe(value);
                if (!user) return S_OK;
                session->done = true;
                FoundAppsample(*user);
                Credentials::Wipe(user->refreshToken);
                PostMessageW(session->window, WM_CLOSE, 0, 0);
                return S_OK;
            }).Get());
    }

    static void setUpWebView(HWND window, ICoreWebView2Controller* controller);

    // A login button that opens a popup (Google and the like) gets a window of
    // its own over the login window, with the same rules.
    static HRESULT onNewWindow(ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
        LPWSTR uri = nullptr;
        args->get_Uri(&uri);
        bool allowed = uri && session && AllowedUrl(session->site, uri);
        CoTaskMemFree(uri);
        if (!allowed) {
            args->put_Handled(TRUE);
            return S_OK;
        }

        ComPtr<ICoreWebView2Deferral> deferral;
        args->GetDeferral(&deferral);
        ComPtr<ICoreWebView2NewWindowRequestedEventArgs> request = args;
        float scale = GetDpiForSystem() / 96.0f;
        HWND popup = makeWindow(widen(SiteLabel(session->site)).c_str(), (int)(520 * scale), (int)(720 * scale), session->window);
        session->environment->CreateCoreWebView2Controller(popup, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [popup, request, deferral](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (SUCCEEDED(result) && controller && session) {
                    session->windows[popup].controller = controller;
                    setUpWebView(popup, controller);
                    layOut(popup);
                    ComPtr<ICoreWebView2> webview;
                    controller->get_CoreWebView2(&webview);
                    request->put_NewWindow(webview.Get());
                    webview->add_WindowCloseRequested(Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
                        [popup](ICoreWebView2*, IUnknown*) -> HRESULT {
                            DestroyWindow(popup);
                            return S_OK;
                        }).Get(), nullptr);
                }
                else {
                    request->put_Handled(TRUE);
                    DestroyWindow(popup);
                }
                deferral->Complete();
                return S_OK;
            }).Get());
        return S_OK;
    }

    static void setUpWebView(HWND window, ICoreWebView2Controller* controller) {
        ComPtr<ICoreWebView2> webview;
        controller->get_CoreWebView2(&webview);

        // nothing of the overlay is reachable from the page, and nothing typed is saved
        ComPtr<ICoreWebView2Settings> settings;
        webview->get_Settings(&settings);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_IsWebMessageEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        ComPtr<ICoreWebView2Settings4> moreSettings;
        if (SUCCEEDED(settings.As(&moreSettings))) {
            moreSettings->put_IsPasswordAutosaveEnabled(FALSE);
            moreSettings->put_IsGeneralAutofillEnabled(FALSE);
        }

        auto onlyAllowed = [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            if (!uri || !session || !AllowedUrl(session->site, uri)) args->put_Cancel(TRUE);
            CoTaskMemFree(uri);
            return S_OK;
        };
        webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(onlyAllowed).Get(), nullptr);
        webview->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(onlyAllowed).Get(), nullptr);
        webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(onNewWindow).Get(), nullptr);
        webview->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
            [window](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                LPWSTR source = nullptr;
                sender->get_Source(&source);
                if (source) showSite(window, source);
                CoTaskMemFree(source);
                return S_OK;
            }).Get(), nullptr);
        webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                checkCookies();
                return S_OK;
            }).Get(), nullptr);
    }

    // HoYoLAB's map already logged in, with the two values kept
    static void addHoyolabCookies() {
        std::string header = HoyolabMapCookie();
        size_t split = header.find("; ltoken_v2=");
        if (split == std::string::npos) return;
        std::wstring id = widen(header.substr(9, split - 9)), token = widen(header.substr(split + 12));
        Credentials::Wipe(header);
        for (auto [name, value] : { std::make_pair(L"ltuid_v2", &id), std::make_pair(L"ltoken_v2", &token) }) {
            ComPtr<ICoreWebView2Cookie> cookie;
            if (SUCCEEDED(session->cookies->CreateCookie(name, value->c_str(), L".hoyolab.com", L"/", &cookie))) {
                cookie->put_IsSecure(TRUE);
                session->cookies->AddOrUpdateCookie(cookie.Get());
            }
        }
        wipe(token);
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_SIZE:
            layOut(window);
            return 0;
        case WM_ERASEBKGND:
            if (session && session->ground) {
                RECT bounds;
                GetClientRect(window, &bounds);
                FillRect((HDC)wParam, &bounds, session->ground);
                return 1;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (session && session->ground) {
                auto it = session->windows.find(window);
                bool host = it != session->windows.end() && (HWND)lParam == it->second.host;
                SetTextColor((HDC)wParam, colorOf(host ? session->theme.faint : session->theme.text));
                SetBkColor((HDC)wParam, colorOf(session->theme.panel));
                return (LRESULT)session->ground;
            }
            break;
        case WM_DRAWITEM:
            if (session && session->ground && wParam == CANCEL_ID) {
                drawCancel(*(const DRAWITEMSTRUCT*)lParam);
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wParam == LOOK_TIMER) {
                checkCookies();
                checkFirebase();
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == CANCEL_ID && session) PostMessageW(session->window, WM_CLOSE, 0, 0);
            return 0;
        case WM_DESTROY:
            if (session) {
                auto it = session->windows.find(window);
                if (it != session->windows.end()) {
                    if (it->second.controller) it->second.controller->Close();
                    session->windows.erase(it);
                }
                if (window == session->window) PostQuitMessage(0);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static void runWindow(Site site, Mode mode, Controls::Theme theme) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // a folder of its own, so nothing a window before left is ever read
        std::filesystem::path folder = profileFolder();
        if (!folder.empty()) folder /= std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());

        LPWSTR version = nullptr;
        if (folder.empty() || FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || !version) {
            CoTaskMemFree(version);
            CoUninitialize();
            WindowClosed(site, "The login window needs Microsoft Edge WebView2, and it isn't installed.");
            windowSite = -1;
            windowOpen = false;
            return;
        }
        CoTaskMemFree(version);

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW windowClass = { sizeof(windowClass) };
            windowClass.lpfnWndProc = windowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
            windowClass.lpszClassName = WINDOW_CLASS;
            RegisterClassExW(&windowClass);
            registered = true;
        }

        Session current;
        current.site = site;
        current.mode = mode;
        current.theme = theme;
        current.ground = CreateSolidBrush(colorOf(theme.panel));
        session = &current;
        float scale = GetDpiForSystem() / 96.0f;
        std::wstring title = mode == Mode::Map ? L"HoYoLAB Map" : L"Log in to " + widen(SiteLabel(site));
        current.window = makeWindow(title.c_str(), (int)((mode == Mode::LogIn ? 640 : 1200) * scale), (int)(820 * scale));
        mainWindow = current.window;
        windowThreadId = GetCurrentThreadId();
        SetForegroundWindow(current.window);
        std::string problem;

        CreateCoreWebView2EnvironmentWithOptions(nullptr, folder.c_str(), nullptr, Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&problem](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                // closed before it started
                if (!session) return S_OK;
                if (FAILED(result) || !environment) {
                    problem = "The login window couldn't start.";
                    PostMessageW(session->window, WM_CLOSE, 0, 0);
                    return S_OK;
                }
                session->environment = environment;
                HWND window = session->window;
                environment->CreateCoreWebView2Controller(window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [window, &problem](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                        if (!session) {
                            if (controller) controller->Close();
                            return S_OK;
                        }
                        if (FAILED(result) || !controller) {
                            problem = "The login window couldn't start.";
                            PostMessageW(window, WM_CLOSE, 0, 0);
                            return S_OK;
                        }
                        session->windows[window].controller = controller;
                        setUpWebView(window, controller);
                        layOut(window);

                        controller->get_CoreWebView2(&session->page);
                        ComPtr<ICoreWebView2_2> withCookies;
                        if (SUCCEEDED(session->page.As(&withCookies))) withCookies->get_CookieManager(&session->cookies);
                        if (!session->cookies) {
                            problem = "The login window couldn't start.";
                            PostMessageW(window, WM_CLOSE, 0, 0);
                            return S_OK;
                        }
                        if (session->mode == Mode::Map) addHoyolabCookies();
                        else SetTimer(window, LOOK_TIMER, 1000, nullptr);
                        session->page->Navigate(session->site == Site::HoyolabMap ? HOYOLAB_MAP : APPSAMPLE_MAP);
                        return S_OK;
                    }).Get());
                return S_OK;
            }).Get());

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        // WM_DESTROY takes each one out of the list
        while (!current.windows.empty()) {
            auto it = current.windows.begin();
            HWND window = it->first;
            if (it->second.controller) it->second.controller->Close();
            DestroyWindow(window);
            current.windows.erase(window);
        }
        current.page.Reset();
        current.cookies.Reset();
        current.environment.Reset();
        session = nullptr;
        mainWindow = nullptr;
        windowThreadId = 0;
        DeleteObject(current.ground);
        WindowClosed(site, problem);
        windowSite = -1;
        windowOpen = false;

        // the browser takes a moment to let go of its files
        for (int i = 0; i < 50; i++) {
            MSG pending;
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&pending);
            std::error_code error;
            std::filesystem::remove_all(folder, error);
            if (!error && !std::filesystem::exists(folder)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (std::filesystem::exists(folder)) std::cerr << "[accounts] the login window's browser folder couldn't be deleted: " << folder.string() << std::endl;
        CoUninitialize();
    }

    static void openWindow(Site site, Mode mode) {
        if (windowOpen) {
            HWND window = mainWindow;
            if (window) SetForegroundWindow(window);
            return;
        }
        // one before may still be deleting its folder
        if (windowThread.joinable()) windowThread.detach();
        windowOpen = true;
        windowSite = (int)site;
        if (mode == Mode::LogIn) WindowOpened(site);
        windowThread = std::thread(runWindow, site, mode, Controls::GetTheme());
    }

    // the login window and its popups
    void RaiseWindow() {
        DWORD thread = windowThreadId;
        if (!thread) return;
        EnumThreadWindows(thread, [](HWND window, LPARAM) -> BOOL {
            if (IsWindowVisible(window)) SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            return TRUE;
        }, 0);
    }

    std::optional<Site> WindowSite() {
        int site = windowSite;
        if (site < 0) return std::nullopt;
        return (Site)site;
    }

    void DeleteWindowFolder() {
        if (windowOpen) return;
        std::filesystem::path folder = profileFolder();
        std::error_code ignored;
        if (!folder.empty()) std::filesystem::remove_all(folder, ignored);
    }

    void LogIn(Site site) {
        openWindow(site, Mode::LogIn);
    }

    void OpenHoyolabMap() {
        if (HoyolabMapCookie().empty()) return;
        openWindow(Site::HoyolabMap, Mode::Map);
    }

    void CloseWindow(bool wait) {
        HWND window = mainWindow;
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (wait && windowThread.joinable()) windowThread.join();
    }
}
