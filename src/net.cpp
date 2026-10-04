#include "net.hpp"

// HACK: glew is *not* needed here, but not putting it here gives a redefinition warning of 'APIENTRY' when compiling (Doesn't cause errors, as far as I could tell though).
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <windows.h>
#include <winhttp.h>
#include <mutex>
#include <iostream>
#include <thread>
#include <deque>
#include <atomic>
#include <vector>

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
	bool Net::post(const std::string& url, const std::string& body, Response& out, bool hoyolab)
	{
		std::call_once(sessionOnce, []() {
			session = WinHttpOpen(L"genshin-overlay",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);

			if (session)
				WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
			});

		if (!session)
			return false;

		std::wstring wideUrl = widen(url);

		URL_COMPONENTS parts = {};
		parts.dwStructSize = sizeof(parts);

		wchar_t host[256] = {};
		wchar_t path[2048] = {};
		wchar_t extra[2048] = {};

		parts.lpszHostName = host;
		parts.dwHostNameLength = 256;
		parts.lpszUrlPath = path;
		parts.dwUrlPathLength = 2048;
		parts.lpszExtraInfo = extra;
		parts.dwExtraInfoLength = 2048;

		if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
			return false;

		std::wstring target = std::wstring(path) + extra;

		HINTERNET connection = WinHttpConnect(session,host,parts.nPort,0);

		if (!connection)
			return false;

		HINTERNET request = WinHttpOpenRequest(connection,L"POST",target.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);

		if (!request) {
			WinHttpCloseHandle(connection);
			return false;
		}

		const wchar_t* headers = hoyolab
			? L"Content-Type: application/json\r\n"
			L"Origin: https://act.hoyolab.com\r\n"
			L"Referer: https://act.hoyolab.com/\r\n"
			: L"Content-Type: application/json\r\n";

		bool ok = false;

		if (WinHttpSendRequest(request,headers,(DWORD)-1L,(LPVOID)body.data(),(DWORD)body.size(),(DWORD)body.size(),0) && WinHttpReceiveResponse(request, nullptr)) {
			DWORD status = 0;
			DWORD size = sizeof(status);

			WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);

			out.status = (int)status;
			out.body.clear();

			ok = true;

			DWORD available = 0;

			while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
				size_t offset = out.body.size();
				out.body.resize(offset + available);

				DWORD read = 0;

				if (!WinHttpReadData(request,&out.body[offset],available,&read)){
					ok = false;
					break;
				}

				out.body.resize(offset + read);
			}
		}

		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);

		return ok;
	}

    WebSocket::~WebSocket() {
        close();
    }

    bool WebSocket::connect(const std::string& url, bool hoyolab)
    {
        close();

        std::string httpUrl;
        bool secure = false;

        if (url.rfind("wss://", 0) == 0) {
            httpUrl = "https://" + url.substr(6);
            secure = true;
        }
        else if (url.rfind("ws://", 0) == 0) {
            httpUrl = "http://" + url.substr(5);
        }
        else {
            std::cerr << "WebSocket: invalid URL scheme" << std::endl;
            return false;
        }

        std::call_once(sessionOnce, [] {
            session = WinHttpOpen(
                L"GenshinOverlay/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0
            );
            });

        if (!session) {
            std::cerr << "WebSocket: WinHttpOpen failed: "
                << GetLastError() << std::endl;
            return false;
        }

        std::wstring wideUrl = widen(httpUrl);

        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);

        wchar_t host[256]{};
        wchar_t path[4096]{};

        components.lpszHostName = host;
        components.dwHostNameLength = ARRAYSIZE(host);
        components.lpszUrlPath = path;
        components.dwUrlPathLength = ARRAYSIZE(path);

        if (!WinHttpCrackUrl(
            wideUrl.c_str(),
            static_cast<DWORD>(wideUrl.length()),
            0,
            &components))
        {
            std::cerr << "WebSocket: WinHttpCrackUrl failed: "
                << GetLastError() << std::endl;
            return false;
        }

        HINTERNET connectionHandle = WinHttpConnect(
            session,
            components.lpszHostName,
            components.nPort,
            0
        );

        if (!connectionHandle) {
            std::cerr << "WebSocket: WinHttpConnect failed: "
                << GetLastError() << std::endl;
            return false;
        }

        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET request = WinHttpOpenRequest(
            connectionHandle,
            L"GET",
            components.lpszUrlPath,
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags
        );

        if (!request) {
            std::cerr << "WebSocket: WinHttpOpenRequest failed: "
                << GetLastError() << std::endl;
            WinHttpCloseHandle(connectionHandle);
            return false;
        }

        if (hoyolab) {
            WinHttpAddRequestHeaders(
                request,
                L"Origin: https://www.hoyolab.com\r\n"
                L"Referer: https://www.hoyolab.com/\r\n",
                -1,
                WINHTTP_ADDREQ_FLAG_ADD
            );
        }

        if (!WinHttpSetOption(
            request,
            WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
            nullptr,
            0))
        {
            std::cerr << "WebSocket: WinHttpSetOption failed: "
                << GetLastError() << std::endl;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connectionHandle);
            return false;
        }

        if (!WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0))
        {
            std::cerr << "WebSocket: WinHttpSendRequest failed: "
                << GetLastError() << std::endl;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connectionHandle);
            return false;
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            std::cerr << "WebSocket: WinHttpReceiveResponse failed: "
                << GetLastError() << std::endl;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connectionHandle);
            return false;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);

        if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE |
            WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX))
        {
            std::cerr << "WebSocket: WinHttpQueryHeaders failed: "
                << GetLastError() << std::endl;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connectionHandle);
            return false;
        }

        if (status != 101) {
            std::cerr << "WebSocket: server returned HTTP "
                << status << std::endl;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connectionHandle);
            return false;
        }

        HINTERNET socketHandle = WinHttpWebSocketCompleteUpgrade(
            request,
            0
        );

        if (!socketHandle) {
            std::cerr << "WebSocket: WinHttpWebSocketCompleteUpgrade failed: "
                << GetLastError() << std::endl;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connectionHandle);
            return false;
        }

        WinHttpCloseHandle(request);

        connection = connectionHandle;
        socket = socketHandle;

        running = true;
        connected = true;

        receiveThread = std::thread(
            &WebSocket::receiveLoop,
            this
        );

        return true;
    }

    bool WebSocket::send(const std::string& message) {
        if (!connected || !socket)
            return false;

        std::lock_guard<std::mutex> lock(sendMutex);

        if (!connected || !socket)
            return false;

        DWORD result = WinHttpWebSocketSend(
            socket,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            (PVOID)message.data(),
            static_cast<DWORD>(message.size())
        );

        if (result != ERROR_SUCCESS) {
            connected = false;
            return false;
        }

        return true;
    }

    bool WebSocket::receive(
        std::string& message,
        size_t maxMessageSize
    ) {
        message.clear();

        if (!socket)
            return false;

        std::string buffer;
        buffer.reserve(4096);

        std::vector<char> chunk(16 * 1024);

        bool complete = false;

        while (!complete) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;

            DWORD result = WinHttpWebSocketReceive(
                socket,
                chunk.data(),
                static_cast<DWORD>(chunk.size()),
                &bytesRead,
                &type
            );

            if (result != ERROR_SUCCESS) {
                connected = false;
                return false;
            }

            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                connected = false;
                return false;
            }

            if (buffer.size() + bytesRead > maxMessageSize) {
                connected = false;
                return false;
            }

            if (bytesRead > 0)
                buffer.append(chunk.data(), bytesRead);

            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                complete = true;
            }
        }

        message = std::move(buffer);
        return true;
    }

    void WebSocket::receiveLoop() {
        while (running && connected) {
            std::string message;

            if (!receive(message))
                break;

            if (message.empty())
                continue;

            {
                std::lock_guard<std::mutex> lock(receiveMutex);
                messages.push_back(std::move(message));
            }
        }

        connected = false;
        running = false;
    }

    bool WebSocket::poll(std::string& message) {
        std::lock_guard<std::mutex> lock(receiveMutex);

        if (messages.empty())
            return false;

        message = std::move(messages.front());
        messages.pop_front();

        return true;
    }

    std::vector<std::string> WebSocket::pollAll() {
        std::vector<std::string> result;

        std::lock_guard<std::mutex> lock(receiveMutex);

        while (!messages.empty()) {
            result.push_back(std::move(messages.front()));
            messages.pop_front();
        }

        return result;
    }

    void WebSocket::close() {
        running = false;
        connected = false;

        if (socket) {
            WinHttpWebSocketClose(
                socket,
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                nullptr,
                0
            );
        }

        if (receiveThread.joinable())
            receiveThread.join();

        if (socket) {
            WinHttpCloseHandle(socket);
            socket = nullptr;
        }

        if (connection) {
            WinHttpCloseHandle(connection);
            connection = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(receiveMutex);
            messages.clear();
        }
    }

    bool WebSocket::isConnected() const {
        return connected;
    }
}
