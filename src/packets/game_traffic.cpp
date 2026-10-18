#include "game_traffic.hpp"
#include "game_chests.hpp"
#include "game_oculi.hpp"
#include "game_packets.hpp"
#include "npcap_capture.hpp"
#include "../capture.hpp"
#include "../helpers.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <imgui.h>

namespace GameTraffic {
    // how long a note stays on screen
    static constexpr auto NOTICE_TIME = std::chrono::seconds(10);
    static constexpr const char* STARTED_LATE = "the game was already logged in, log out to the door and back in once";
    static constexpr const char* NOT_MATCHED = "opened a chest it can't match to a marker, mark it yourself";
    static constexpr const char* OCULUS_NOT_MATCHED = "picked up an oculus it can't match to a marker, mark it yourself";

    static std::thread worker;
    static std::atomic<bool> stopping = false;
    static std::mutex mutex;
    // guarded by mutex
    static Status status;
    static std::string notice;
    static std::chrono::steady_clock::time_point noticeUntil;
    // the HoYoLAB points of the chests opened and oculi picked up, oldest first, never cleared
    static std::vector<int64_t> opened;

    static void setStatus(State state, const std::string& text) {
        std::lock_guard lock(mutex);
        status.state = state;
        status.text = text;
    }

    // only the problems the player can fix in game get a note on screen
    static void showNotice(const std::string& text) {
        std::lock_guard lock(mutex);
        notice = text;
        noticeUntil = std::chrono::steady_clock::now() + NOTICE_TIME;
    }

    // when the game was started, unix ms, 0 when it can't be read
    static uint64_t gameStart() {
        HWND window = Capture::findGameWindow();
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

    static int64_t pointOf(const GameChests::Chest& chest, const GameChests::Tracker& tracker,
        std::vector<GameChests::Marker>& markers) {
        // the item files may still be downloading when the overlay starts
        if (markers.empty())
            GameChests::LoadMarkers(Helpers::exeDirectory() / "items" / "positions", markers);
        return GameChests::Match(chest, tracker.Chests(), markers);
    }

    static int64_t pointOf(const GameOculi::Picked& picked, const GameOculi::Tracker& tracker,
        std::vector<GameChests::Marker>& markers) {
        if (markers.empty())
            GameOculi::LoadMarkers(Helpers::exeDirectory() / "items" / "positions", markers);
        return GameOculi::Match(picked.object, picked.label, tracker.Objects(), markers);
    }

    static void listen() {
        GamePackets::Keys keys;
        if (!GamePackets::LoadKeys(Helpers::exeDirectory() / "packets", keys)) {
            setStatus(State::CantListen, "the packets folder next to the exe is missing");
            return;
        }
        std::vector<std::string> adapters;
        std::string problem;
        if (!NpcapCapture::Start(adapters, problem)) {
            setStatus(State::CantListen, problem);
            return;
        }
        for (const std::string& name : adapters) std::cout << "Game traffic: listening on " << name << std::endl;
        setStatus(State::Waiting, "");

        GamePackets::Sniffer sniffer(keys);
        GameChests::Tracker chests;
        std::vector<GameChests::Marker> markers;
        GameOculi::Tracker oculi;
        std::vector<GameChests::Marker> oculusMarkers;
        std::vector<NpcapCapture::Frame> frames;
        std::vector<GamePackets::Command> commands;
        // a login seen since the start, without one the key can't be found yet
        bool sawLogin = false;
        std::string lastProblem;
        while (!stopping) {
            // short, so Stop doesn't hold up the menu
            NpcapCapture::Take(frames, 250);
            for (const NpcapCapture::Frame& frame : frames) {
                commands.clear();
                if (sniffer.Feed(frame.data.data(), frame.data.size(), commands, frame.ms) == GamePackets::Event::Handshake) {
                    sawLogin = true;
                    sniffer.SetGameStart(gameStart());
                }
                for (const GamePackets::Command& command : commands) {
                    if (command.direction != GamePackets::Direction::Received)
                        continue;
                    GameOculi::Picked picked;
                    if (oculi.Feed(command.id, command.body, frame.ms, picked)) {
                        int64_t point = pointOf(picked, oculi, oculusMarkers);
                        std::cout << "Game traffic: oculus " << picked.object.group << " " << picked.object.config << " picked up, point " << point << std::endl;
                        if (point == 0) {
                            showNotice(OCULUS_NOT_MATCHED);
                            continue;
                        }
                        std::lock_guard lock(mutex);
                        opened.push_back(point);
                        continue;
                    }
                    GameChests::Chest chest;
                    bool placed = false;
                    if (!chests.Feed(command.id, command.body, chest, placed))
                        continue;
                    // inspecting things and places of interest come as opened too,
                    // only the chests the game showed us have a gadget to tell
                    if (!placed)
                        continue;
                    int64_t point = pointOf(chest, chests, markers);
                    std::cout << "Game traffic: chest " << chest.group << " " << chest.config << " opened, point " << point << std::endl;
                    if (point == 0) {
                        showNotice(NOT_MATCHED);
                        continue;
                    }
                    std::lock_guard lock(mutex);
                    opened.push_back(point);
                }
            }
            if (frames.empty()) continue;

            std::string problem = sniffer.Problem();
            if (!problem.empty() && !sawLogin && !sniffer.HasKey()) problem = STARTED_LATE;
            {
                std::lock_guard lock(mutex);
                if (!problem.empty()) status.state = State::CantRead;
                else if (sniffer.Searching()) status.state = State::Searching;
                else if (sniffer.HasKey()) status.state = State::Reading;
                status.text = problem;
            }
            if (problem != lastProblem) {
                lastProblem = problem;
                if (!problem.empty()) {
                    std::cout << "Game traffic: " << problem << std::endl;
                    showNotice(problem);
                }
            }
        }
        NpcapCapture::Stop();
    }

    void Start() {
        if (worker.joinable()) return;
        stopping = false;
        worker = std::thread(listen);
    }

    void Stop() {
        if (!worker.joinable()) return;
        stopping = true;
        worker.join();
        std::lock_guard lock(mutex);
        status = Status();
        noticeUntil = {};
    }

    size_t OpenedPoints(size_t from, std::vector<int64_t>& out) {
        std::lock_guard lock(mutex);
        for (size_t i = from; i < opened.size(); i++) out.push_back(opened[i]);
        return opened.size();
    }

    Status GetStatus() {
        std::lock_guard lock(mutex);
        return status;
    }

    void DrawStatus() {
        Status now = GetStatus();
        switch (now.state) {
        case State::Off:
            ImGui::TextDisabled("Not listening");
            break;
        case State::CantListen:
            ImGui::TextWrapped("Can't listen: %s", now.text.c_str());
            break;
        case State::Waiting:
            ImGui::TextWrapped("Listening. If you're already logged in, log out to the door and back in once.");
            break;
        case State::Searching:
            ImGui::TextWrapped("Looking for the session key, this can take a minute");
            break;
        case State::Reading:
            ImGui::Text("Working");
            break;
        case State::CantRead:
            ImGui::TextWrapped("Can't read the game: %s", now.text.c_str());
            break;
        }
    }

    void DrawNotice() {
        std::string text;
        {
            std::lock_guard lock(mutex);
            if (std::chrono::steady_clock::now() >= noticeUntil) return;
            text = notice;
        }
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 80.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##game traffic", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("Genshin Overlay: %s", text.c_str());
        ImGui::End();
    }

    bool NeedsDrawing() {
        std::lock_guard lock(mutex);
        return std::chrono::steady_clock::now() < noticeUntil;
    }
}
