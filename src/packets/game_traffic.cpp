#include "game_traffic.hpp"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../helpers.hpp"

namespace GameTraffic {
    // how long Stop can wait for the worker to notice
    static constexpr int TAKE_MS = 250;
    // while waiting, adapters are listed again this often (a Wi-Fi switch, a
    // VPN), and a capture that couldn't start tries again
    static constexpr int64_t REFRESH_MS = 30000;
    // the game's start, looked up this often until the key is found
    static constexpr int64_t GAME_START_MS = 2000;

    static std::thread worker;
    static std::atomic<bool> stopping = false;
    static std::mutex mutex;
    // guarded by mutex
    static Status status;
    // its own lock, so a listener can ask for the status
    static std::mutex listenersMutex;
    static std::vector<std::pair<int, Listener>> listeners;
    static int nextListener = 1;

    const char* StateCode(const Status& status) {
        switch (status.state) {
        case State::Off: return "off";
        case State::CantListen: return "cant_listen";
        case State::Listening: return Packets::StateCode(status.read);
        }
        return "";
    }

    const char* ReasonCode(const Status& status) {
        switch (status.state) {
        case State::Off: return "";
        case State::CantListen:
            if (status.keysMissing)
                return "keys_missing";
            if (status.keysDamaged)
                return "keys_damaged";
            return PacketCapture::ProblemCode(status.problem);
        case State::Listening: return Packets::ReasonCode(status.reason);
        }
        return "";
    }

    static Status cantListen(PacketCapture::Problem problem, bool keysMissing = false, bool keysDamaged = false) {
        Status out;
        out.state = State::CantListen;
        // a capture that started and then lost every adapter has no problem of its own
        out.problem = problem == PacketCapture::Problem::None ? PacketCapture::Problem::NoAdapter : problem;
        out.keysMissing = keysMissing;
        out.keysDamaged = keysDamaged;
        return out;
    }

    Status FromReader(const Packets::Reader& reader) {
        Status out;
        out.state = State::Listening;
        out.read = reader.GetState();
        out.reason = reader.GetReason();
        out.searchPass = reader.SearchPass();
        out.secondsLeft = reader.SecondsLeft();
        return out;
    }

    uint64_t GameStart() {
        // not Capture::findGameWindow, a minimized game counts here
        HWND window = FindWindowW(L"UnityWndClass", L"Genshin Impact");
        if (!window)
            window = FindWindowW(L"UnityWndClass", L"原神");
        DWORD id = 0;
        if (!window || !GetWindowThreadProcessId(window, &id))
            return 0;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (!process)
            return 0;
        FILETIME created, exited, kernel, user;
        bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
        CloseHandle(process);
        if (!ok)
            return 0;
        uint64_t ticks = uint64_t(created.dwHighDateTime) << 32 | created.dwLowDateTime;
        // FILETIME counts 100 ns from 1601
        return ticks / 10000 - 11644473600000ull;
    }

    static int64_t steadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // once Stop was called the worker's last status is left out, the menu shows off
    static void setStatus(const Status& now, bool stopped = false) {
        std::lock_guard lock(mutex);
        if (stopping && !stopped)
            return;
        bool changed = std::string(StateCode(now)) != StateCode(status) || std::string(ReasonCode(now)) != ReasonCode(status);
        status = now;
        if (changed) {
            std::cout << "Game traffic: " << StateCode(now);
            if (*ReasonCode(now))
                std::cout << " (" << ReasonCode(now) << ")";
            std::cout << std::endl;
        }
    }

    static void hand(const std::vector<Messages::Message>& messages) {
        if (messages.empty())
            return;
        std::vector<std::pair<int, Listener>> now;
        {
            std::lock_guard lock(listenersMutex);
            now = listeners;
        }
        for (const Messages::Message& message : messages)
            for (auto& [id, listener] : now)
                listener(message);
    }

    static void printAdapters(const PacketCapture::Listener& capture) {
        for (const std::string& name : capture.Adapters())
            std::cout << "Game traffic: listening on " << name << std::endl;
    }

    static void listen() {
        SessionKey::Keys keys;
        std::filesystem::path folder = Helpers::exeDirectory() / "packets";
        std::error_code error;
        if (!std::filesystem::exists(folder / "dispatch-keys.json", error)) {
            setStatus(cantListen(PacketCapture::Problem::None, true));
            return;
        }
        // without them every login would end in not_found
        if (!SessionKey::LoadKeys(folder, keys) || keys.dispatch.empty() || !SessionKey::Rsa(keys.rsaPem).AllLoaded()) {
            setStatus(cantListen(PacketCapture::Problem::None, false, true));
            return;
        }
        PacketCapture::Listener capture;
        capture.Start();
        printAdapters(capture);
        Packets::Reader reader(keys);
        std::vector<PacketCapture::Frame> frames;
        std::vector<Messages::Message> messages;
        uint64_t missed = capture.Missed();
        int64_t refreshed = steadyMs(), gameStartAt = 0;
        // the last frame's time and when it came, for the quiet minute
        uint64_t lastFrameMs = 0;
        int64_t lastFrameAt = 0;

        while (!stopping) {
            int64_t now = steadyMs();
            Packets::State read = reader.GetState();
            bool waiting = reader.Idle();
            if (now >= refreshed + REFRESH_MS && (waiting || capture.Live() == 0)) {
                refreshed = now;
                std::vector<std::string> before = capture.Adapters();
                capture.Refresh();
                if (capture.Adapters() != before)
                    printAdapters(capture);
            }
            if (capture.Live() == 0) {
                // every adapter went away, the stream has a hole now
                reader.Missed();
                setStatus(cantListen(capture.GetProblem()));
                std::this_thread::sleep_for(std::chrono::milliseconds(TAKE_MS));
                continue;
            }
            if (read != Packets::State::Reading && now >= gameStartAt + GAME_START_MS) {
                gameStartAt = now;
                reader.SetGameStart(GameStart());
            }

            capture.Take(frames, TAKE_MS);
            messages.clear();
            if (capture.Missed() != missed) {
                missed = capture.Missed();
                reader.Missed();
            }
            for (const PacketCapture::Frame& frame : frames)
                reader.Feed(frame.data.data(), frame.data.size(), frame.start, frame.ms, messages);
            reader.Update(messages);
            if (!frames.empty()) {
                lastFrameMs = frames.back().ms;
                lastFrameAt = steadyMs();
            }
            if (lastFrameMs)
                reader.Tick(lastFrameMs + uint64_t(steadyMs() - lastFrameAt));
            hand(messages);

            Status got = FromReader(reader);
            got.way = capture.GetWay();
            setStatus(got);
        }
    }

    void Start() {
        if (worker.joinable()) {
            if (!stopping)
                return;
            // ticked off and on again quickly, the old one ends first
            worker.join();
        }
        stopping = false;
        worker = std::thread(listen);
    }

    void Stop(bool wait) {
        if (!worker.joinable())
            return;
        stopping = true;
        setStatus(Status(), true);
        if (wait)
            worker.join();
    }

    Status GetStatus() {
        std::lock_guard lock(mutex);
        return status;
    }

    int AddListener(Listener listener) {
        std::lock_guard lock(listenersMutex);
        int id = nextListener++;
        listeners.push_back({ id, std::move(listener) });
        return id;
    }

    void RemoveListener(int id) {
        std::lock_guard lock(listenersMutex);
        for (size_t i = 0; i < listeners.size(); i++)
            if (listeners[i].first == id) {
                listeners.erase(listeners.begin() + i);
                return;
            }
    }
}
