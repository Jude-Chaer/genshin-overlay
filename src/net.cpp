#include "net.hpp"
#include <windows.h>
#include <winhttp.h>
#include <mutex>

namespace Net {
    static HINTERNET session = nullptr;
    static std::once_flag sessionOnce;

    static std::wstring widen(const std::string& s) {
        return std::wstring(s.begin(), s.end());
    }

    bool get(const std::string& url, Response& out, bool hoyolab) {
        std::call_once(sessionOnce, []() {
            session = WinHttpOpen(L"genshin-overlay", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session) {
                WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
            }
        });
        if (!session) return false;

        std::wstring wideUrl = widen(url);
        URL_COMPONENTS parts = {};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2048;
        wchar_t extra[2048] = {};
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) return false;

        std::wstring target = std::wstring(path) + extra;
        bool ok = false;
        HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;

        if (request) {
            const wchar_t* headers = hoyolab ? L"Origin: https://act.hoyolab.com\r\nReferer: https://act.hoyolab.com/\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
            if (WinHttpSendRequest(request, headers, hoyolab ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                out.status = (int)status;
                out.body.clear();

                ok = true;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                    size_t offset = out.body.size();
                    out.body.resize(offset + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, &out.body[offset], available, &read)) {
                        ok = false;
                        break;
                    }
                    out.body.resize(offset + read);
                }
            }
        }

        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        return ok;
    }
}
