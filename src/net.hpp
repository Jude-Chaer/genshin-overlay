#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <vector>
#include <functional>
#include "net_stop.hpp"

namespace Net {
    struct Response {
        int status = 0;
        std::string body;
        // the place publicOnly refused, empty when nothing was
        std::string refused;
        // refused for not being one of the sites
        bool notListed = false;
        // past the deadline, or the body past maxBody
        bool timedOut = false;
        bool tooBig = false;
    };

    // The sites a request may reach: each one and the names under it
    // (hoyolab.com takes in bbs-api-os.hoyolab.com). None set means any.
    struct Sites {
        bool only = false;
        std::vector<std::string> names;
    };
    // host is one of the sites or under one, letter case aside
    extern bool onSites(const std::string& host, const Sites& sites);

    // What an extension's request is held to. publicOnly refuses this PC and
    // the home network before anything is sent, see publicAddress. sites holds
    // it to those, from the first host through every redirect. maxBody (bytes)
    // and deadlineMs (the whole request) are off at 0.
    struct Limits {
        bool publicOnly = false;
        Sites sites;
        size_t maxBody = 0;
        int deadlineMs = 0;
    };

    // Blocking GET over WinHTTP, follows redirects. hoyolab adds the Origin and
    // Referer headers HoYoLAB's API and CDN want. True only with the whole body:
    // false if it couldn't connect, the body was cut short, it went past limits,
    // or progress or stopper stopped it.
    extern bool get(const std::string& url, Response& out, bool hoyolab = false, const Progress& progress = nullptr, Stopper* stopper = nullptr,
        const Limits& limits = {});
    extern bool post(const std::string& url, const std::string& body, Response& out, bool hoyolab = false, const Limits& limits = {},
        Stopper* stopper = nullptr);

    // Out on the internet: not this PC, not a private, link-local or Tailscale
    // range, not a network this PC's adapters are on. The address as text,
    // IPv4 or IPv6.
    extern bool publicAddress(const std::wstring& address);

    // every request goes through this proxy (host:port) when it's set, the way
    // Clash or v2rayN set one for the whole PC; for the tests
    extern std::wstring testProxy;

    struct SocketCall;
    class WebSocket {
    public:
        WebSocket() = default;
        ~WebSocket();

        WebSocket(const WebSocket&) = delete;
        WebSocket& operator=(const WebSocket&) = delete;

        // blocks until it's open or failed; stopper ends the wait
        bool connect(const std::string& url, bool hoyolab = false, const Limits& limits = {}, Stopper* stopper = nullptr);
        bool send(const std::string& message);
        bool receive(std::string& message, size_t maxMessageSize = 16 * 1024 * 1024);

        bool poll(std::string& message);
        std::vector<std::string> pollAll();

        void close();

        bool isConnected() const;
        // messages waiting to be polled at most, 0 for no cap; one more ends the connection
        void setMaxQueued(size_t count) { maxQueued = count; }
        bool overflowed() const { return overflow; }
        bool connectTimedOut() const { return timedOut; }
        // the place the last connect refused, empty when it didn't
        const std::string& refusedPlace() const { return refused; }
        bool refusedNotListed() const { return notListed; }

    private:
        void receiveLoop();

        HINTERNET connection = nullptr;
        HINTERNET socket = nullptr;
        HINTERNET ownSession = nullptr;
        SocketCall* steps = nullptr;
        std::string refused;
        bool notListed = false;
        bool timedOut = false;
        size_t maxQueued = 0;
        std::atomic<bool> overflow = false;

        std::thread receiveThread;
        std::mutex receiveMutex;
        std::mutex sendMutex;

        std::deque<std::string> messages;

        std::atomic<bool> running = false;
        std::atomic<bool> connected = false;
    };
}
