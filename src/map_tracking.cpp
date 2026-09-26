#include "map_tracking.hpp"
#include "capture.hpp"
#include "map_data.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace MapTracking {
    static constexpr auto TICK_INTERVAL = std::chrono::milliseconds(120);
    // a few misses in a row before hiding, so one blurry frame doesn't make it flicker
    static constexpr int MISSES_BEFORE_HIDE = 3;

    static std::atomic<Status> status{ Status::NoData };
    static std::atomic<bool> stopping{ false };
    static std::thread worker;
    static std::mutex mutex;
    static std::condition_variable wake;

    static MapLocator::Index index;
    static MapLocator::Tracker tracker;

    // guarded by mutex
    static bool frameWaiting = false;
    static bool busy = false;
    static bool resetTracker = false;
    static cv::Mat frame;
    static RECT frameRect = {};
    static MapView view;
    static int misses = 0;

    static std::string downloadStage;
    static std::atomic<int> downloadPercent{ 0 };

    static std::chrono::steady_clock::time_point lastCapture;
    static HWND lastGameWindow = nullptr;

    // loads the index once, then matches whatever frame Tick hands over
    static void workerLoop(std::filesystem::path dataFolder) {
        status = Status::Downloading;
        bool ready = MapData::ensure(dataFolder, [](const char* stage, int percent) {
            std::lock_guard<std::mutex> lock(mutex);
            downloadStage = stage;
            downloadPercent = percent;
        }, &stopping);
        if (stopping) return;
        if (!ready) {
            status = Status::NoData;
            return;
        }

        status = Status::Loading;
        std::filesystem::path manifestPath = dataFolder / "manifest.tsv";
        bool loaded = index.Load(manifestPath, &stopping);
        if (stopping) return;
        status = loaded ? Status::Ready : Status::Failed;
        if (!loaded) {
            std::cerr << "Could not load map data from " << manifestPath << std::endl;
            return;
        }

        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, []() { return stopping || frameWaiting; });
            if (stopping) return;

            cv::Mat current = frame;
            RECT rect = frameRect;
            frame.release();
            frameWaiting = false;
            busy = true;
            if (resetTracker) {
                tracker.Reset();
                resetTracker = false;
            }
            lock.unlock();

            double cx = current.cols / 2.0;
            double cy = current.rows / 2.0;
            MapLocator::TrackStep step = tracker.Step(index, current, cx, cy);

            lock.lock();
            busy = false;
            if (step.kind == MapLocator::TrackStep::Apply) {
                view.visible = true;
                view.result = step.result;
                view.gameRect = rect;
                misses = 0;
            }
            else if (step.kind == MapLocator::TrackStep::Miss) {
                if (++misses >= MISSES_BEFORE_HIDE) {
                    view.visible = false;
                }
            }
        }
    }

    void Start(const std::filesystem::path& dataFolder) {
        if (worker.joinable()) return;
        stopping = false;
        worker = std::thread(workerLoop, dataFolder);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    // game closed or in the background: forget the map until it's back
    static void hide() {
        std::lock_guard<std::mutex> lock(mutex);
        if (view.visible) {
            view.visible = false;
            resetTracker = true;
        }
        misses = 0;
    }

    void Tick(HWND overlayWindow) {
        if (status != Status::Ready) return;

        auto now = std::chrono::steady_clock::now();
        if (now - lastCapture < TICK_INTERVAL) return;

        // only capture while the game, or our own menu, is in front
        HWND game = Capture::findGameWindow();
        HWND foreground = GetForegroundWindow();
        if (!game || IsIconic(game) || (foreground != game && foreground != overlayWindow)) {
            hide();
            return;
        }
        if (game != lastGameWindow) {
            lastGameWindow = game;
            std::lock_guard<std::mutex> lock(mutex);
            resetTracker = true;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            // still working on the last one
            if (busy || frameWaiting) return;
        }

        RECT rect;
        if (!Capture::getClientRectOnScreen(game, rect)) return;

        cv::Mat screenshot;
        lastCapture = now;
        if (!Capture::grabScreen(rect, overlayWindow, screenshot)) return;

        {
            std::lock_guard<std::mutex> lock(mutex);
            frame = screenshot;
            frameRect = rect;
            frameWaiting = true;
        }
        wake.notify_one();
    }

    Status GetStatus() {
        return status;
    }

    std::string GetDownloadStage(int& percent) {
        std::lock_guard<std::mutex> lock(mutex);
        percent = downloadPercent;
        return downloadStage;
    }

    MapView GetView() {
        std::lock_guard<std::mutex> lock(mutex);
        return view;
    }

    const char* GetMapName(int mapId) {
        switch (mapId) {
        case 2: return "Teyvat";
        case 7: return "Enkanomiya";
        case 9: return "The Chasm: Underground Mines";
        default: return "Other map";
        }
    }
}
