#include "game_traffic.hpp"
#include "game_packets.hpp"
#include "npcap_capture.hpp"
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
    static constexpr const char* STARTED_LATE = "the game was already logged in, close the game and start it again";

    static std::thread worker;
    static std::atomic<bool> stopping = false;
    static std::mutex mutex;
    // guarded by mutex
    static Status status;
    static std::string notice;
    static std::chrono::steady_clock::time_point noticeUntil;

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
                if (sniffer.Feed(frame.data.data(), frame.data.size(), commands) == GamePackets::Event::Handshake) sawLogin = true;
            }
            if (frames.empty()) continue;

            std::string problem = sniffer.Problem();
            if (!problem.empty() && !sawLogin && !sniffer.HasKey()) problem = STARTED_LATE;
            {
                std::lock_guard lock(mutex);
                if (!problem.empty()) status.state = State::CantRead;
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
            ImGui::TextWrapped("Listening. If the game was already running, close it and start it again.");
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
