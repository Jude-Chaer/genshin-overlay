#include "map_tracking.hpp"
#include "capture.hpp"
#include "map_data.hpp"
#include "map_follow.hpp"
#include "settings.hpp"
#include "helpers.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dwmapi.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

// How it fits together. Three threads:
//   zoom bar detector  says if the in-game map is open
//   follow thread      while it's open, wakes up for every frame the game
//                      draws, grabs a few patches of it and compares them
//                      (MapFollow), which moves the view
//   worker             SIFT, slow. Finds the map, finds it again when the
//                      compare lost it, and lands it exactly once it stops.
// SIFT answers for the frame it was given. The follower knows how the map
// moved since, so its answer fixes the view without waiting.
// The draw loop only reads the view, through GetView.
//
// Closing the map is noticed two ways. Pressing M or Esc hides the view right
// away, when running as admin. Anything else (the X button, a controller) is
// noticed by the zoom bar going away.

namespace MapTracking {
    // Finding the map needs this many matching points. Following it takes
    // any match SIFT calls found.
    static constexpr int FIRST_FIX_INLIERS = 15;

    static std::atomic<Status> status{ Status::NoData };
    static std::atomic<bool> stopping{ false };
    static std::thread worker;
    static std::thread followWorker;
    static std::atomic<HWND> overlayWindow{ nullptr };
    // held for a whole step, by the follow thread or by the draw loop (Settings::followOnThread)
    static std::mutex stepMutex;
    static std::mutex mutex;
    static std::condition_variable wake;

    static MapLocator::Index index;

    // guarded by mutex
    static bool frameWaiting = false;
    static bool busy = false;
    static bool resetTracker = false;
    static cv::Mat frame;
    static bool matchDone = false;
    static MapLocator::Result matched;
    static MapView view;
    static std::chrono::steady_clock::time_point lastUpdate;
    // goes up with every publish, WaitForDraw waits for the next one
    static uint64_t answers = 0;
    static std::condition_variable answered;

    // follow thread only
    static MapFollow::Follower follower;
    // a copy of it for GetView, so drawing never waits for a step. Guarded by mutex
    static MapFollow::Follower shown;
    // the last frame handed to the follower, and its id there
    static Capture::Frame lastGameFrame;
    static int frameId = 0;
    // The frame SIFT was given. Session goes up with every reset, a result
    // from before the reset is dropped.
    static int sentId = 0;
    static int session = 0, sentSession = -1;

    static std::atomic<float> lastStepMs{ 0.0f };
    static std::string downloadStage;
    static std::atomic<int> downloadPercent{ 0 };

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

    // loads the index once, then matches whatever frame follow hands over
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
            frame.release();
            frameWaiting = false;
            busy = true;
            lock.unlock();

            // fast: the game at half resolution at most, also to land when the map stops
            double cx = current.cols / 2.0;
            double cy = current.rows / 2.0;
            auto started = std::chrono::steady_clock::now();
            MapLocator::Result result = index.Match(current, cx, cy, true, false);
            lastStepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();

            lock.lock();
            busy = false;
            matched = result;
            matchDone = true;
        }
    }

    // Hands what the follower knows to GetView and wakes the draw loop.
    // Takes the lock and keeps it.
    static void publish(std::unique_lock<std::mutex>& lock, const RECT& rect, std::chrono::steady_clock::time_point now) {
        lock.lock();
        // closed while we were at it
        if (resetTracker) return;
        shown = follower;
        view.visible = follower.Visible();
        view.result = follower.View();
        view.gameRect = rect;
        lastUpdate = now;
        answers++;
        answered.notify_all();
    }

    // One step of following: takes back SIFT's answer, grabs the newest frame
    // for the follower, and hands SIFT a whole grab when the follower wants one.
    static void follow(HWND game, const RECT& rect, std::chrono::steady_clock::time_point now) {
        std::unique_lock<std::mutex> lock(mutex);
        if (resetTracker) {
            resetTracker = false;
            session++;
            follower.Reset();
            lastGameFrame = {};
        }
        bool canSend = !busy && !frameWaiting;
        bool gotMatch = matchDone;
        MapLocator::Result match = matched;
        matchDone = false;
        lock.unlock();

        double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        if (gotMatch && sentSession == session && match.found && (follower.Visible() || match.inliers >= FIRST_FIX_INLIERS)) {
            follower.Matched(sentId, match, seconds);
        }

        bool settle = canSend && follower.WantsSettle(seconds);
        bool look = canSend && !settle && follower.WantsLook(seconds);
        Capture::Frame gameFrame = lastGameFrame;
        MapFollow::Patches patches;
        cv::Mat screenshot, wide;
        if (settle || look) {
            if (!Capture::grabGame(game, rect, screenshot, &gameFrame)) return;
            if (gameFrame.number != lastGameFrame.number) {
                patches = MapFollow::cut(screenshot, rect);
                wide = MapFollow::cutWide(screenshot);
            }
        }
        else if (!MapFollow::grab(game, rect, patches, wide, gameFrame)) {
            // the game didn't draw a new frame yet
            return;
        }
        if (!patches.empty()) {
            int frames = lastGameFrame.number ? (int)std::min<uint64_t>(gameFrame.number - lastGameFrame.number, 100) : 1;
            lastGameFrame = gameFrame;
            follower.Frame(++frameId, patches, wide, rect, gameFrame.seconds, frames);
        }

        if (settle || look) {
            sentId = frameId;
            sentSession = session;
            if (settle) follower.Settling();
            else follower.Looking(seconds);

            lock.lock();
            frame = screenshot;
            frameWaiting = true;
            lock.unlock();
            wake.notify_one();
        }

        publish(lock, rect, now);
    }

    static void followLoop();

    void Start(const std::filesystem::path& dataFolder) {
        if (worker.joinable()) return;
        stopping = false;
        detectorStopping = false;
        worker = std::thread(workerLoop, dataFolder);
        followWorker = std::thread(followLoop);
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

        if (followWorker.joinable())
            followWorker.join();

        Capture::closeGameCapture();
    }

    // game closed or in the background: forget the map until it's back
    static void hide() {
        std::lock_guard<std::mutex> lock(mutex);
        if (view.visible) {
            view.visible = false;
            resetTracker = true;
        }
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
            ignoreUntil = std::chrono::steady_clock::now() + IGNORE_AFTER_CLOSE;
        }
        else {
            // the map is probably opening, look now instead of waiting.
            // If it's still mid animation that look misses and the next one catches it.
            follower.LookNow();
        }
    }

    // one step if the game is in front and its map open, false if there's nothing to follow now
    static bool step() {
        if (status != Status::Ready) return false;

        // only capture while the game, or our own menu, is in front
        HWND game = Capture::findGameWindow();
        HWND foreground = GetForegroundWindow();
        if (!game || IsIconic(game) || (foreground != game && foreground != overlayWindow.load())) {
            hide();
            return false;
        }

        if (!mapOpen.load(std::memory_order_acquire))
            return false;

        // every frame, a quick tap would slip between two screenshots
        checkMapKeys();

        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (now < ignoreUntil) return true;
        }

        if (game != lastGameWindow) {
            lastGameWindow = game;
            std::lock_guard<std::mutex> lock(mutex);
            resetTracker = true;
        }

        RECT rect;
        if (!Capture::getClientRectOnScreen(game, rect)) return false;

        follow(game, rect, now);
        return true;
    }

    // The follow thread. Steps when the game hands over a frame, so the draw
    // loop never waits for a grab.
    static void followLoop() {
        while (!stopping) {
            bool following = false;
            if (Settings::followOnThread) {
                std::lock_guard<std::mutex> lock(stepMutex);
                following = step();
            }
            if (following) Capture::waitForGameFrame(20);
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void Tick(HWND window) {
        overlayWindow = window;
        if (!Settings::followOnThread) {
            std::lock_guard<std::mutex> lock(stepMutex);
            step();
        }
    }

    Status GetStatus() {
        return status;
    }

    bool IsMapOpen() {
        return mapOpen.load(std::memory_order_acquire);
    }

    float GetLastStepMs() {
        return lastStepMs;
    }

    std::string GetDownloadStage(int& percent) {
        std::lock_guard<std::mutex> lock(mutex);
        percent = downloadPercent;
        return downloadStage;
    }

    // seconds between two screen refreshes
    static double refreshSeconds() {
        static double seconds = 0.0;
        if (seconds > 0.0) return seconds;
        DWM_TIMING_INFO timing = { sizeof(timing) };
        LARGE_INTEGER frequency;
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && QueryPerformanceFrequency(&frequency) && timing.qpcRefreshPeriod) {
            seconds = (double)timing.qpcRefreshPeriod / frequency.QuadPart;
        }
        else {
            seconds = 1.0 / 60.0;
        }
        return seconds;
    }

    // When what's drawn now gets on screen. Windows puts the screen together
    // right after a refresh out of what was drawn until then, and shows that
    // on the refresh after.
    static double shownAt(double now) {
        DWM_TIMING_INFO timing = { sizeof(timing) };
        LARGE_INTEGER frequency;
        double refresh = refreshSeconds();
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)) || !QueryPerformanceFrequency(&frequency)) return now + refresh;
        double blank = (double)timing.qpcVBlank / frequency.QuadPart;
        double next = blank + std::ceil((now - blank) / refresh) * refresh;
        return next + refresh;
    }

    // The follow thread has its answer early in a refresh. Drawing right
    // then gets it on screen one refresh sooner than drawing after DwmFlush.
    void WaitForDraw() {
        static uint64_t drawn = 0;
        std::unique_lock<std::mutex> lock(mutex);
        if (!Settings::followOnThread || !view.visible) {
            lock.unlock();
            DwmFlush();
            return;
        }
        answered.wait_for(lock, std::chrono::milliseconds(20), []() { return answers != drawn; });
        drawn = answers;
    }

    MapView GetView() {
        std::lock_guard<std::mutex> lock(mutex);
        MapView copy = view;
        auto now = std::chrono::steady_clock::now();
        copy.secondsSinceUpdate = std::chrono::duration<double>(now - lastUpdate).count();
        if (copy.visible) {
            // What we draw now shows up about a screen refresh later than the
            // game's frame drawn now does, so the view is for then.
            double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
            copy.result = shown.Ahead(Settings::followOnThread ? shownAt(seconds) : seconds + refreshSeconds());
        }
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
                    }
                    else {
                        std::lock_guard<std::mutex> lock(mutex);

                        view.visible = false;
                        resetTracker = true;
                    }
                }
            }

            std::this_thread::sleep_for(DETECTOR_INTERVAL);
        }
    }
}
