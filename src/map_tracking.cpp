#include "map_tracking.hpp"
#include "capture.hpp"
#include "map_data.hpp"
#include "helpers.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

// How it fits together: Tick runs on the main thread every frame. About every
// 120ms it takes a screenshot of the game and hands it to the worker thread,
// which runs the Tracker on it and updates the view. A new screenshot is only
// taken once the worker is done with the last one, so a slow match never piles
// up frames, it just lowers how often the view updates.
//
// Closing the map is noticed two ways. Pressing M or Esc hides the view right
// away, when running as admin. Anything else (the X button, a controller) is
// noticed when the screenshots stop matching, which takes about a second.

namespace MapTracking {
    static constexpr auto TICK_INTERVAL = std::chrono::milliseconds(120);
    // two misses in a row before hiding, so one blurry frame while dragging doesn't make it flicker
    static constexpr int MISSES_BEFORE_HIDE = 2;

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
    static std::chrono::steady_clock::time_point frameTime;
    static MapView view;
    static std::chrono::steady_clock::time_point lastUpdate;
    static int misses = 0;

    static std::atomic<float> lastStepMs{ 0.0f };
    static std::string downloadStage;
    static std::atomic<int> downloadPercent{ 0 };

    static std::chrono::steady_clock::time_point lastCapture;
    static HWND lastGameWindow = nullptr;

    // After M or Esc closes the map, the game takes about half a second to fade
    // it out. A screenshot from that half second still shows the map and would
    // bring the view right back, so for a little longer than that we don't take
    // screenshots, and throw away any taken before (their match can finish later).
    static constexpr auto IGNORE_AFTER_CLOSE = std::chrono::milliseconds(800);
    static std::chrono::steady_clock::time_point ignoreUntil;
    static bool mapKeyWasDown = false;

    // Zoombar Detector thread.
    static std::atomic<bool> mapOpen{ false };
    static std::atomic<bool> detectorStopping{ false };

    static std::thread detectorWorker;
    static std::mutex detectorMutex;
    static std::condition_variable detectorWake;

    static HWND detectorGameWindowCache;
    static RECT detectorRectCache;
    static std::once_flag zoomTemplateOnce;
    static cv::Mat zoomTemplate;
    static cv::Mat detectorGreyscaleMatCache;


  

    static constexpr auto DETECTOR_INTERVAL =
        std::chrono::milliseconds(50);

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
            auto takenAt = frameTime;
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
            auto started = std::chrono::steady_clock::now();
            MapLocator::TrackStep step = tracker.Step(index, current, cx, cy);
            lastStepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();

            lock.lock();
            busy = false;
            // taken while the map was still fading out, checked by when the
            // screenshot was taken, not when the match finished
            if (takenAt < ignoreUntil) {
                resetTracker = true;
            }
            else if (step.kind == MapLocator::TrackStep::Apply) {
                view.visible = true;
                view.result = step.result;
                view.gameRect = rect;
                lastUpdate = std::chrono::steady_clock::now();
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
        detectorStopping = false;
        worker = std::thread(workerLoop, dataFolder);
		detectorWorker = std::thread(zoombarDetectorLoop);  
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }

        detectorStopping = true;

        wake.notify_all();
        detectorWake.notify_all();

        if (detectorWorker.joinable())
            detectorWorker.join();

        if (worker.joinable())
            worker.join();

        Capture::closeGameCapture();
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

    // M and Esc open and close the map, no need to wait for the screenshots to
    // notice. The controller, the X button and so on still go through them.
    // We have to watch these keys without taking them from the game, and Windows
    // only lets us see an admin window's keys when we run as admin too. So this
    // only works with the admin setting on, otherwise the screenshots catch it.
    static void checkMapKeys() {
        static bool elevated = Helpers::isElevated();
        if (!elevated) return;

        bool down = (GetAsyncKeyState('M') & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
        bool pressed = down && !mapKeyWasDown;
        mapKeyWasDown = down;
        if (!pressed) return;

        std::lock_guard<std::mutex> lock(mutex);
        if (view.visible) {
            view.visible = false;
            resetTracker = true;
            misses = 0;
            ignoreUntil = std::chrono::steady_clock::now() + IGNORE_AFTER_CLOSE;
        }
        else {
            // the map is probably opening, look now instead of at the next tick.
            // If it's still mid animation that look misses and the next tick catches it.
            lastCapture = {};
        }
    }

    void Tick(HWND overlayWindow) {
        if (status != Status::Ready) return;

        // only capture while the game, or our own menu, is in front
        HWND game = Capture::findGameWindow();
        HWND foreground = GetForegroundWindow();
        if (!game || IsIconic(game) || (foreground != game && foreground != overlayWindow)) {
            hide();
            return;
        }

        if (!mapOpen.load(std::memory_order_acquire))
            return;

        // every frame, a quick tap would slip between two screenshots
        checkMapKeys();

        auto now = std::chrono::steady_clock::now();
        if (now - lastCapture < TICK_INTERVAL) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (now < ignoreUntil) return;
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
        if (!Capture::grabGame(game, rect, screenshot)) return;

        {
            std::lock_guard<std::mutex> lock(mutex);
            frame = screenshot;
            frameRect = rect;
            frameTime = now;
            frameWaiting = true;
        }
        wake.notify_one();
    }

    Status GetStatus() {
        return status;
    }

    float GetLastStepMs() {
        return lastStepMs;
    }

    std::string GetDownloadStage(int& percent) {
        std::lock_guard<std::mutex> lock(mutex);
        percent = downloadPercent;
        return downloadStage;
    }

    MapView GetView() {
        std::lock_guard<std::mutex> lock(mutex);
        MapView copy = view;
        copy.secondsSinceUpdate = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastUpdate).count();
        return copy;
    }

    const char* GetMapName(int mapId) {
        switch (mapId) {
        case 2: return "Teyvat";
        case 7: return "Enkanomiya";
        case 9: return "The Chasm: Underground Mines";
        default: return "Other map";
        }
    }

    bool getZoomBarRect(HWND game, RECT& out)
    {
        RECT client;
        if (!Capture::getClientRectOnScreen(game, client))
            return false;

        int h = client.bottom - client.top;

        double scale = h / 1125.0;

        out.left = client.left + (LONG)(37.5 * scale);
        out.top = client.top + (LONG)(450 * scale);
        out.right = client.left + (LONG)(62.5 * scale);
        out.bottom = client.top + (LONG)(475 * scale);

        return out.right > out.left && out.bottom > out.top;
    }

    void loadZoomTemplate()
    {
        zoomTemplate = cv::imread(
            "zoombar_template.png",
            cv::IMREAD_GRAYSCALE
        );
        if (zoomTemplate.empty()) {
            std::cerr << "Failed to load zoombar_template.png\n";
        }
	}

    bool detectZoomBar(const cv::Mat& image)
    {
        auto start = std::chrono::steady_clock::now();
        std::call_once(zoomTemplateOnce, loadZoomTemplate);

        if (image.empty() || zoomTemplate.empty())
           return false;

        if (image.channels() == 4)
            cv::cvtColor(image, detectorGreyscaleMatCache, cv::COLOR_BGRA2GRAY);
        else if (image.channels() == 3)
            cv::cvtColor(image, detectorGreyscaleMatCache, cv::COLOR_BGR2GRAY);
        else
            detectorGreyscaleMatCache = image;

        if (detectorGreyscaleMatCache.cols < zoomTemplate.cols ||
            detectorGreyscaleMatCache.rows < zoomTemplate.rows)
            return false;

        cv::Mat result;

        cv::matchTemplate(
            detectorGreyscaleMatCache,
            zoomTemplate,
            result,
            cv::TM_CCOEFF_NORMED
        );

        double maxVal;
        cv::minMaxLoc(result, nullptr, &maxVal, nullptr, nullptr);

        constexpr double kMatchThreshold = 0.60;
        return maxVal >= kMatchThreshold;
    }


    void zoombarDetectorLoop()
    {
        bool previousOpen = false;
		detectorGameWindowCache = Capture::findGameWindow();
        getZoomBarRect(detectorGameWindowCache, detectorRectCache);

        while (!detectorStopping) {
			// Refresh the game window and zoom bar rect if the window is closed or moved
            if (!IsWindow(detectorGameWindowCache)) {
                detectorGameWindowCache = Capture::findGameWindow();

                if (!detectorGameWindowCache) {
                    std::this_thread::sleep_for(DETECTOR_INTERVAL);
                    continue;
                }
            }
            if (!getZoomBarRect(detectorGameWindowCache, detectorRectCache)) {
                detectorGameWindowCache = Capture::findGameWindow();

                if (!detectorGameWindowCache ||
                    !getZoomBarRect(detectorGameWindowCache, detectorRectCache)) {
                    std::this_thread::sleep_for(DETECTOR_INTERVAL);
                    continue;
                }
            }


            if (IsIconic(detectorGameWindowCache)) {
                if (previousOpen) {
                    previousOpen = false;
                    mapOpen = false;

                    std::lock_guard<std::mutex> lock(mutex);
                    view.visible = false;
                    resetTracker = true;
                    misses = 0;
                }
                std::this_thread::sleep_for(DETECTOR_INTERVAL);
                continue;
            }

            cv::Mat zoomScreenshot;

            if (Capture::grabGame(detectorGameWindowCache, detectorRectCache, zoomScreenshot)) {
                bool open = detectZoomBar(zoomScreenshot);
                if (open != previousOpen) {
                    previousOpen = open;
                    mapOpen = open;
                    if (open) {
                        std::lock_guard<std::mutex> lock(mutex);

                        resetTracker = true;
                        misses = 0;
                        lastCapture = {};
                    }
                    else {
                        std::lock_guard<std::mutex> lock(mutex);

                        view.visible = false;
                        resetTracker = true;
                        misses = 0;
                        // a match still running would bring the view back
                        ignoreUntil = std::chrono::steady_clock::now();
                    }
                }
            }

            std::this_thread::sleep_for(DETECTOR_INTERVAL);
        }
    }
}
