#include "game_traffic.hpp"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
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

    const char* StateCode(State state) {
        switch (state) {
        case State::Off: return "off";
        case State::CantListen: return "cant_listen";
        case State::WaitingForLogin: return Packets::StateCode(Packets::State::WaitingForLogin);
        case State::Searching: return Packets::StateCode(Packets::State::Searching);
        case State::Reading: return Packets::StateCode(Packets::State::Reading);
        case State::NeedsRelog: return Packets::StateCode(Packets::State::NeedsRelog);
        case State::NoKey: return Packets::StateCode(Packets::State::NoKey);
        }
        return "";
    }

    const char* ReasonCode(Reason reason) {
        switch (reason) {
        case Reason::None: return "";
        case Reason::NpcapMissing: return PacketCapture::ProblemCode(PacketCapture::Problem::NpcapMissing);
        case Reason::NpcapTooOld: return PacketCapture::ProblemCode(PacketCapture::Problem::NpcapTooOld);
        case Reason::NpcapAdminOnly: return PacketCapture::ProblemCode(PacketCapture::Problem::NpcapAdminOnly);
        case Reason::NoAdapter: return PacketCapture::ProblemCode(PacketCapture::Problem::NoAdapter);
        case Reason::KeysMissing: return "keys_missing";
        case Reason::CompressedLogin: return Packets::ReasonCode(Packets::Reason::CompressedLogin);
        case Reason::StartedLate: return Packets::ReasonCode(Packets::Reason::StartedLate);
        case Reason::MissedPackets: return Packets::ReasonCode(Packets::Reason::MissedPackets);
        case Reason::KeyStoppedFitting: return Packets::ReasonCode(Packets::Reason::KeyStoppedFitting);
        case Reason::UnknownKeyId: return Packets::ReasonCode(Packets::Reason::UnknownKeyId);
        case Reason::NotFound: return Packets::ReasonCode(Packets::Reason::NotFound);
        }
        return "";
    }

    static Reason fromCapture(PacketCapture::Problem problem) {
        switch (problem) {
        case PacketCapture::Problem::NpcapMissing: return Reason::NpcapMissing;
        case PacketCapture::Problem::NpcapTooOld: return Reason::NpcapTooOld;
        case PacketCapture::Problem::NpcapAdminOnly: return Reason::NpcapAdminOnly;
        default: return Reason::NoAdapter;
        }
    }

    Status FromReader(const Packets::Reader& reader) {
        Status out;
        switch (reader.GetState()) {
        case Packets::State::WaitingForLogin: out.state = State::WaitingForLogin; break;
        case Packets::State::Searching: out.state = State::Searching; break;
        case Packets::State::Reading: out.state = State::Reading; break;
        case Packets::State::NeedsRelog: out.state = State::NeedsRelog; break;
        case Packets::State::NoKey: out.state = State::NoKey; break;
        }
        switch (reader.GetReason()) {
        case Packets::Reason::None: out.reason = Reason::None; break;
        case Packets::Reason::CompressedLogin: out.reason = Reason::CompressedLogin; break;
        case Packets::Reason::StartedLate: out.reason = Reason::StartedLate; break;
        case Packets::Reason::MissedPackets: out.reason = Reason::MissedPackets; break;
        case Packets::Reason::KeyStoppedFitting: out.reason = Reason::KeyStoppedFitting; break;
        case Packets::Reason::UnknownKeyId: out.reason = Reason::UnknownKeyId; break;
        case Packets::Reason::NotFound: out.reason = Reason::NotFound; break;
        }
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
        bool changed = now.state != status.state || now.reason != status.reason;
        status = now;
        if (changed) {
            std::cout << "Game traffic: " << StateCode(now.state);
            if (now.reason != Reason::None)
                std::cout << " (" << ReasonCode(now.reason) << ")";
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
        if (!SessionKey::LoadKeys(Helpers::exeDirectory() / "packets", keys)) {
            setStatus({ State::CantListen, Reason::KeysMissing });
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
                setStatus({ State::CantListen, fromCapture(capture.GetProblem()) });
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
