#include <windows.h>
#include <psapi.h>
#include "stats.hpp"
#include "map_tracking.hpp"
#include <imgui.h>
#include <chrono>

namespace Stats {
    static constexpr int HISTORY = 120;
    static constexpr auto SAMPLE_INTERVAL = std::chrono::milliseconds(500);

    static float cpuHistory[HISTORY] = {};
    static float ramHistory[HISTORY] = {};
    static float matchHistory[HISTORY] = {};
    static int next = 0;
    static bool filled = false;

    static std::chrono::steady_clock::time_point lastSample;
    static ULONGLONG lastCpuTime = 0;

    static ULONGLONG processCpuTime() {
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime;
        k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;
        u.HighPart = user.dwHighDateTime;
        return k.QuadPart + u.QuadPart;  // 100ns units
    }

    void Update() {
        auto now = std::chrono::steady_clock::now();
        if (now - lastSample < SAMPLE_INTERVAL) return;

        ULONGLONG cpuTime = processCpuTime();
        if (lastCpuTime != 0) {
            // CPU time used since the last sample over the time that passed, split
            // across all cores, so 100% means the whole machine like Task Manager shows it
            double wallTime = std::chrono::duration<double>(now - lastSample).count() * 10000000.0;
            SYSTEM_INFO info;
            GetSystemInfo(&info);
            double cpu = (double)(cpuTime - lastCpuTime) / wallTime / info.dwNumberOfProcessors * 100.0;

            PROCESS_MEMORY_COUNTERS_EX memory = {};
            GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&memory, sizeof(memory));

            cpuHistory[next] = (float)cpu;
            // private bytes, the memory that's only ours (the Memory column in Task Manager)
            ramHistory[next] = memory.PrivateUsage / (1024.0f * 1024.0f);
            matchHistory[next] = MapTracking::GetLastStepMs();
            next = (next + 1) % HISTORY;
            if (next == 0) filled = true;
        }
        lastCpuTime = cpuTime;
        lastSample = now;
    }

    static float maxOf(const float* values) {
        float highest = 0.0f;
        for (int i = 0; i < HISTORY; i++) {
            if (values[i] > highest) highest = values[i];
        }
        return highest;
    }

    static void graph(const char* label, const float* values, const char* format, float minScale) {
        int latest = (next + HISTORY - 1) % HISTORY;
        char text[64];
        snprintf(text, sizeof(text), format, values[latest]);

        float top = (std::max)(minScale, maxOf(values) * 1.2f);
        float width = ImGui::GetFontSize() * 12.0f;
        float height = ImGui::GetFontSize() * 2.2f;
        ImGui::PlotLines(label, values, HISTORY, filled ? next : 0, text, 0.0f, top, ImVec2(width, height));
    }

    void Draw() {
        graph("CPU", cpuHistory, "%.1f %%", 5.0f);
        graph("RAM", ramHistory, "%.0f MB", 50.0f);
        graph("Match", matchHistory, "%.0f ms", 100.0f);
    }
}
