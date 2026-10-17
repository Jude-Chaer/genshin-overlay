#pragma once
#include <string>

// The HoYoLAB login. The user logs in on HoYoLAB's own page in a window, we
// keep only ltoken_v2 and ltuid_v2, in Windows' credential store. The cookie
// only leaves this file as the header HoyolabMarks puts on its own requests,
// extensions never see it.

namespace HoyolabAccount {
    extern void Init();
    extern void Shutdown();

    extern bool LoggedIn();
    // the HoYoLAB account id (ltuid_v2), empty when logged out
    extern std::string Account();
    // the window is open, for logging in or for HoYoLAB's map
    extern bool WindowOpen();
    // what went wrong last, empty when nothing did
    extern std::string LastError();

    // opens HoYoLAB's map to log in, closes on its own once the cookie is there
    extern void LogIn();
    // opens HoYoLAB's map already logged in, for a captcha HoYoLAB wants solved
    extern void ShowMap();
    extern void LogOut();
    // HoYoLAB said the login is no good anymore
    extern void LoginExpired();

    // "Cookie: ...\r\n" for a request to HoYoLAB, empty when logged out
    extern std::string CookieHeader();
}
