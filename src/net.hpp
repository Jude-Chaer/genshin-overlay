#pragma once
#include <string>

namespace Net {
    struct Response {
        int status = 0;
        std::string body;
    };

    // Blocking GET over WinHTTP, follows redirects. hoyolab adds the Origin and
    // Referer headers HoYoLAB's API and CDN want. False if it couldn't connect.
    extern bool get(const std::string& url, Response& out, bool hoyolab = false);
    extern bool post(const std::string& url, const std::string& body, Response& out, bool hoyolab = false);
}
