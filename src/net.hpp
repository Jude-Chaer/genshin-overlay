#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <vector>

namespace Net {
    struct Response {
        int status = 0;
        std::string body;
    };

    // Blocking GET over WinHTTP, follows redirects. hoyolab adds the Origin and
    // Referer headers HoYoLAB's API and CDN want. False if it couldn't connect.
    extern bool get(const std::string& url, Response& out, bool hoyolab = false);
    extern bool post(const std::string& url, const std::string& body, Response& out, bool hoyolab = false);
    class WebSocket {
    public:
        WebSocket() = default;
        ~WebSocket();

        WebSocket(const WebSocket&) = delete;
        WebSocket& operator=(const WebSocket&) = delete;

        bool connect(const std::string& url, bool hoyolab = false);
        bool send(const std::string& message);
        bool receive(std::string& message, size_t maxMessageSize = 16 * 1024 * 1024);

        bool poll(std::string& message);
        std::vector<std::string> pollAll();

        void close();

        bool isConnected() const;

    private:
        void receiveLoop();

        HINTERNET connection = nullptr;
        HINTERNET socket = nullptr;

        std::thread receiveThread;
        std::mutex receiveMutex;
        std::mutex sendMutex;

        std::deque<std::string> messages;

        std::atomic<bool> running = false;
        std::atomic<bool> connected = false;
    };
}
