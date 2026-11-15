#pragma once
#include <optional>
#include <string>
#include "net.hpp"

// The player's HoYoLAB Map and appsample logins, owned by the overlay. Each is kept
// in Credentials under the site's name. Extensions see a site's state, and with
// its account permission, use it through the overlay's calls.

namespace Accounts {
    enum class Site { HoyolabMap, Appsample };
    // "hoyolab_map", "appsample": the store's and Lua's names
    const char* SiteName(Site site);
    std::optional<Site> SiteNamed(const std::string& name);
    // "HoYoLAB Map", "appsample"
    const char* SiteLabel(Site site);

    enum class State { LoggedOut, WindowOpen, LoggedIn, NeedsLogin, Captcha };
    // "logged_out", "window_open", "logged_in", "needs_login", "captcha"
    const char* StateName(State state);

    struct Status {
        State state = State::LoggedOut;
        // HoYoLAB's account id, appsample's email; kept through NeedsLogin
        std::string account;
        // the last thing that went wrong, for the player, empty when nothing did
        std::string problem;
    };

    // reads both logins from the store; one that doesn't read is deleted
    void Init();
    // closes the window and forgets the secrets in memory
    void Shutdown();
    Status Get(Site site);
    // counts up at every change of either site
    int Changes();
    // On the frame's thread: switches the site's permission off for every
    // extension after a login as another account.
    void Frame();

    // Opens the login window for the site, or brings it to the front. It
    // closes on its own once logged in.
    void LogIn(Site site);
    void LogOut(Site site);
    // HoYoLAB's map in the window, already logged in, for its captcha
    void OpenHoyolabMap();
    // closes the window, with wait until its thread has ended
    void CloseWindow(bool wait = true);
    // the site whose window is open, its login or HoYoLAB's map
    std::optional<Site> WindowSite();
    // puts the window back over the overlay after the overlay raised itself
    void RaiseWindow();
    // the window's browser folder, left behind when the overlay was killed
    void DeleteWindowFolder();
    // the pages and frames the site's window may open: https to the site and
    // the sites it logs in with, and about:blank
    bool AllowedUrl(Site site, const std::wstring& url);

    // What Firebase keeps in the page's IndexedDB under firebase:authUser:,
    // as JSON. Nothing when it has no uid or refresh token.
    struct FirebaseUser {
        std::string uid, email, refreshToken, apiKey;
    };
    std::optional<FirebaseUser> ReadFirebaseUser(const std::string& json);

    // From the window: the login it read, kept in the store. False with the
    // problem when it can't be kept. The account seen last is kept as its
    // sha256, so logging in as another switches the permission off everywhere.
    bool FoundHoyolabMap(const std::string& ltuid, const std::string& ltoken);
    bool FoundAppsample(const FirebaseUser& user);
    void WindowOpened(Site site);
    // closed without a login: back to what it was
    void WindowClosed(Site site, const std::string& problem = "");

    // The site said the login is no good anymore: it's deleted, NeedsLogin.
    // With the secret that was sent, nothing happens when the login is newer.
    void Refused(Site site, const std::string& secret = "");
    // HoYoLAB asked for a captcha; Working ends it
    void CaptchaAsked(Site site);
    void Working(Site site);

    // HoYoLAB: "ltuid_v2=...; ltoken_v2=..." for a Cookie header, "" when not logged in
    std::string HoyolabMapCookie();
    // what stays the same for the account: HoYoLAB's account id, appsample's
    // uid; "" when not logged in
    std::string AccountId(Site site);

    // securetoken.googleapis.com's answer to a refresh
    struct Refresh {
        bool ok = false;
        // the token itself was turned down, not a network or server problem
        bool refused = false;
        std::string idToken, refreshToken;
        int expiresIn = 0;
        std::string why;
    };
    Refresh ReadRefresh(const Net::Response& response);

    // appsample's ID token for its requests, refreshed a few minutes before
    // it runs out. Nothing when not logged in or the refresh failed (Refused
    // when the token was turned down). Blocks, not for the frame's thread.
    std::optional<std::string> AppsampleIdToken(Net::Stopper* stopper = nullptr);
    // appsample turned this ID token down: the next AppsampleIdToken
    // refreshes it instead of giving it again
    void AppsampleIdTokenRefused(const std::string& idToken);
}
