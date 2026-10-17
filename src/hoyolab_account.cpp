#include "hoyolab_account.hpp"
#include <windows.h>
#include <wincred.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace HoyolabAccount {
    static const wchar_t* CREDENTIAL = L"genshin-overlay/hoyolab";
    static const wchar_t* MAP_PAGE = L"https://act.hoyolab.com/ys/app/interactive-map/index.html?lang=en-us";
    static const wchar_t* COOKIE_SITE = L"https://act.hoyolab.com";
    static const wchar_t* WINDOW_CLASS = L"genshin-overlay-hoyolab";

    // The only sites the window may go to: HoYoLAB, its login, the captcha, and
    // the sites HoYoLAB lets you log in with. Pictures and scripts on a page aren't
    // checked, only where the page itself or one of its frames goes.
    static const wchar_t* ALLOWED[] = {
        L"hoyolab.com", L"hoyoverse.com", L"mihoyo.com", L"geetest.com",
        L"google.com", L"apple.com", L"facebook.com", L"twitter.com", L"x.com", L"discord.com",
    };

    static std::mutex mutex;
    static std::string ltuid, ltoken;
    static std::string lastError;

    static std::thread windowThread;
    static std::atomic<bool> windowOpen = false;
    static std::atomic<HWND> mainWindow = nullptr;

    static void setError(const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex);
        lastError = error;
    }

    static void wipe(std::string& secret) {
        if (!secret.empty()) SecureZeroMemory(&secret[0], secret.size());
        secret.clear();
    }

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

    // a cookie value goes into a request header, nothing that could end it or start another one
    static bool cleanValue(const std::string& value) {
        if (value.empty() || value.size() > 512) return false;
        for (char c : value) {
            if (c <= ' ' || c > '~' || c == ';' || c == ',') return false;
        }
        return true;
    }

    // credential store

    static bool readCredential(std::string& id, std::string& token) {
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(CREDENTIAL, CRED_TYPE_GENERIC, 0, &credential)) return false;
        std::string blob((const char*)credential->CredentialBlob, credential->CredentialBlobSize);
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);

        size_t split = blob.find('\n');
        bool ok = split != std::string::npos;
        if (ok) {
            id = blob.substr(0, split);
            token = blob.substr(split + 1);
            ok = cleanValue(id) && cleanValue(token);
        }
        wipe(blob);
        return ok;
    }

    // Kept for this Windows user on this PC only. Windows encrypts it with the
    // user's login, it shows up in Credential Manager where it can be deleted.
    static bool writeCredential(const std::string& id, const std::string& token) {
        std::string blob = id + "\n" + token;
        std::wstring user = widen(id);
        CREDENTIALW credential = {};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = (LPWSTR)CREDENTIAL;
        credential.Comment = (LPWSTR)L"HoYoLAB login of Genshin Overlay";
        credential.UserName = (LPWSTR)user.c_str();
        credential.CredentialBlob = (LPBYTE)blob.data();
        credential.CredentialBlobSize = (DWORD)blob.size();
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        bool ok = CredWriteW(&credential, 0) != FALSE;
        wipe(blob);
        return ok;
    }

    // the window

    // Each window starts with an empty browser profile in here and it's deleted
    // after, so nothing of the HoYoLAB session stays on disk but the two values.
    static std::filesystem::path profileFolder() {
        PWSTR local = nullptr;
        std::filesystem::path folder;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            folder = std::filesystem::path(local) / "genshin-overlay" / "hoyolab-window";
        }
        CoTaskMemFree(local);
        return folder;
    }

    static bool allowedUrl(const std::wstring& url) {
        if (url == L"about:blank") return true;
        const std::wstring https = L"https://";
        if (url.compare(0, https.size(), https) != 0) return false;
        size_t end = url.find_first_of(L"/?#:", https.size());
        std::wstring host = url.substr(https.size(), end == std::wstring::npos ? std::wstring::npos : end - https.size());
        // user:password@ in front of the host
        if (host.find(L'@') != std::wstring::npos) return false;
        for (auto& c : host) c = towlower(c);
        for (const wchar_t* allowed : ALLOWED) {
            std::wstring site = allowed;
            if (host == site) return true;
            if (host.size() > site.size() && host.compare(host.size() - site.size(), site.size(), site) == 0
                && host[host.size() - site.size() - 1] == L'.') return true;
        }
        return false;
    }

    enum class Mode { LogIn, Map };

    struct Session {
        Mode mode;
        HWND window = nullptr;
        ComPtr<ICoreWebView2Environment> environment;
        ComPtr<ICoreWebView2CookieManager> cookies;
        // the window's own and the popups', by window
        std::map<HWND, ComPtr<ICoreWebView2Controller>> controllers;
        bool checking = false;
        bool done = false;
    };
    static Session* session = nullptr;

    static constexpr UINT_PTR COOKIE_TIMER = 1;

    static void fitToWindow(HWND window) {
        if (!session) return;
        auto it = session->controllers.find(window);
        if (it == session->controllers.end()) return;
        RECT bounds;
        GetClientRect(window, &bounds);
        it->second->put_Bounds(bounds);
    }

    // logged in once HoYoLAB has set both, then the window closes
    static void checkCookies() {
        if (!session || session->mode != Mode::LogIn || session->done || session->checking || !session->cookies) return;
        session->checking = true;
        session->cookies->GetCookies(COOKIE_SITE, Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                if (!session) return S_OK;
                session->checking = false;
                if (FAILED(result) || !list) return S_OK;

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
                    if (!cleanValue(id) || !cleanValue(token)) {
                        setError("HoYoLAB's login looked different than expected, nothing was kept.");
                    }
                    else if (!writeCredential(id, token)) {
                        setError("Couldn't keep the login in Windows' credential store.");
                    }
                    else {
                        std::lock_guard<std::mutex> lock(mutex);
                        wipe(ltoken);
                        ltuid = id;
                        ltoken = token;
                        lastError.clear();
                    }
                    PostMessageW(session->window, WM_CLOSE, 0, 0);
                }
                wipe(token);
                return S_OK;
            }).Get());
    }

    static void setUpWebView(ICoreWebView2Controller* controller);

    static HWND makeWindow(const wchar_t* title, int width, int height) {
        int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        // on top, the overlay covers the whole screen
        return CreateWindowExW(WS_EX_TOPMOST, WINDOW_CLASS, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            x, y, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    // A login button that opens a popup (Google and the like) gets a window of
    // its own with the same rules.
    static HRESULT onNewWindow(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) {
        LPWSTR uri = nullptr;
        args->get_Uri(&uri);
        bool allowed = uri && allowedUrl(uri);
        CoTaskMemFree(uri);
        if (!allowed || !session) {
            args->put_Handled(TRUE);
            return S_OK;
        }

        ComPtr<ICoreWebView2Deferral> deferral;
        args->GetDeferral(&deferral);
        ComPtr<ICoreWebView2NewWindowRequestedEventArgs> request = args;
        HWND popup = makeWindow(L"HoYoLAB", 520, 680);
        session->environment->CreateCoreWebView2Controller(popup, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [popup, request, deferral](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (SUCCEEDED(result) && controller && session) {
                    session->controllers[popup] = controller;
                    setUpWebView(controller);
                    fitToWindow(popup);
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

    static void setUpWebView(ICoreWebView2Controller* controller) {
        ComPtr<ICoreWebView2> webview;
        controller->get_CoreWebView2(&webview);

        // nothing of ours is reachable from the page, and nothing typed is saved
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
            if (!uri || !allowedUrl(uri)) args->put_Cancel(TRUE);
            CoTaskMemFree(uri);
            return S_OK;
        };
        webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(onlyAllowed).Get(), nullptr);
        webview->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(onlyAllowed).Get(), nullptr);
        webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(onNewWindow).Get(), nullptr);
        webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                checkCookies();
                return S_OK;
            }).Get(), nullptr);
    }

    // the map already logged in, with the two values we keep
    static void addOurCookies() {
        std::string id, token;
        {
            std::lock_guard<std::mutex> lock(mutex);
            id = ltuid;
            token = ltoken;
        }
        std::wstring wideId = widen(id), wideToken = widen(token);
        for (auto [name, value] : { std::make_pair(L"ltuid_v2", &wideId), std::make_pair(L"ltoken_v2", &wideToken) }) {
            ComPtr<ICoreWebView2Cookie> cookie;
            if (SUCCEEDED(session->cookies->CreateCookie(name, value->c_str(), L".hoyolab.com", L"/", &cookie))) {
                cookie->put_IsSecure(TRUE);
                session->cookies->AddOrUpdateCookie(cookie.Get());
            }
        }
        wipe(wideToken);
        wipe(token);
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_SIZE:
            fitToWindow(window);
            return 0;
        case WM_TIMER:
            if (wParam == COOKIE_TIMER) checkCookies();
            return 0;
        case WM_DESTROY:
            if (session) {
                auto it = session->controllers.find(window);
                if (it != session->controllers.end()) {
                    it->second->Close();
                    session->controllers.erase(it);
                }
                if (window == session->window) PostQuitMessage(0);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static void runWindow(Mode mode) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::filesystem::path folder = profileFolder();
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);

        LPWSTR version = nullptr;
        if (folder.empty() || FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || !version) {
            setError("The login window needs Microsoft Edge WebView2, it isn't installed.");
            CoTaskMemFree(version);
            CoUninitialize();
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
            windowClass.lpszClassName = WINDOW_CLASS;
            RegisterClassExW(&windowClass);
            registered = true;
        }

        Session current;
        current.mode = mode;
        session = &current;
        float scale = GetDpiForSystem() / 96.0f;
        current.window = makeWindow(mode == Mode::LogIn ? L"Log in to HoYoLAB" : L"HoYoLAB map",
            (int)(mode == Mode::LogIn ? 640 * scale : 1200 * scale), (int)(820 * scale));
        mainWindow = current.window;
        SetForegroundWindow(current.window);

        CreateCoreWebView2EnvironmentWithOptions(nullptr, folder.c_str(), nullptr, Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment || !session) {
                    setError("The login window couldn't start.");
                    if (session) PostMessageW(session->window, WM_CLOSE, 0, 0);
                    return S_OK;
                }
                session->environment = environment;
                HWND window = session->window;
                environment->CreateCoreWebView2Controller(window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [window](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                        if (FAILED(result) || !controller || !session) {
                            setError("The login window couldn't start.");
                            PostMessageW(window, WM_CLOSE, 0, 0);
                            return S_OK;
                        }
                        session->controllers[window] = controller;
                        setUpWebView(controller);
                        fitToWindow(window);

                        ComPtr<ICoreWebView2> webview;
                        controller->get_CoreWebView2(&webview);
                        ComPtr<ICoreWebView2_2> webview2;
                        if (SUCCEEDED(webview.As(&webview2))) webview2->get_CookieManager(&session->cookies);
                        if (!session->cookies) {
                            setError("The login window couldn't start.");
                            PostMessageW(window, WM_CLOSE, 0, 0);
                            return S_OK;
                        }
                        if (session->mode == Mode::Map) addOurCookies();
                        else SetTimer(window, COOKIE_TIMER, 1000, nullptr);
                        webview->Navigate(MAP_PAGE);
                        return S_OK;
                    }).Get());
                return S_OK;
            }).Get());

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        for (auto& [window, controller] : current.controllers) {
            controller->Close();
            DestroyWindow(window);
        }
        current.controllers.clear();
        current.cookies.Reset();
        current.environment.Reset();
        session = nullptr;
        mainWindow = nullptr;

        // the browser takes a moment to let go of its files
        for (int i = 0; i < 50; i++) {
            MSG pending;
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&pending);
            std::error_code error;
            std::filesystem::remove_all(folder, error);
            if (!error && !std::filesystem::exists(folder)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CoUninitialize();
        windowOpen = false;
    }

    static void openWindow(Mode mode) {
        if (windowOpen) {
            HWND window = mainWindow;
            if (window) SetForegroundWindow(window);
            return;
        }
        if (windowThread.joinable()) windowThread.join();
        windowOpen = true;
        windowThread = std::thread(runWindow, mode);
    }

    void Init() {
        std::string id, token;
        if (readCredential(id, token)) {
            std::lock_guard<std::mutex> lock(mutex);
            ltuid = id;
            ltoken = token;
        }
        wipe(token);
    }

    void Shutdown() {
        HWND window = mainWindow;
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (windowThread.joinable()) windowThread.join();
        std::lock_guard<std::mutex> lock(mutex);
        wipe(ltoken);
    }

    bool LoggedIn() {
        std::lock_guard<std::mutex> lock(mutex);
        return !ltoken.empty();
    }

    std::string Account() {
        std::lock_guard<std::mutex> lock(mutex);
        return ltoken.empty() ? "" : ltuid;
    }

    bool WindowOpen() {
        return windowOpen;
    }

    std::string LastError() {
        std::lock_guard<std::mutex> lock(mutex);
        return lastError;
    }

    void LogIn() {
        setError("");
        openWindow(Mode::LogIn);
    }

    void ShowMap() {
        if (!LoggedIn()) return;
        openWindow(Mode::Map);
    }

    void LogOut() {
        CredDeleteW(CREDENTIAL, CRED_TYPE_GENERIC, 0);
        std::lock_guard<std::mutex> lock(mutex);
        wipe(ltoken);
        ltuid.clear();
    }

    void LoginExpired() {
        LogOut();
        setError("HoYoLAB doesn't take the login anymore, log in again.");
    }

    std::string CookieHeader() {
        std::lock_guard<std::mutex> lock(mutex);
        if (ltoken.empty()) return "";
        return "Cookie: ltuid_v2=" + ltuid + "; ltoken_v2=" + ltoken + "\r\n";
    }
}
