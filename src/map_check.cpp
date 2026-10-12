#include "map_check.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <opencv2/imgproc.hpp>
#include "map_data.hpp"

namespace MapCheck {
    // decoded tiles kept in memory, 64 KB each at full size, less shrunk
    static constexpr size_t TILES_KEPT = 300;
    // a patch looks like the tiles when phase correlation is at least this sure (0 to 1)
    static constexpr double SURE = 0.7;
    // patches that have to be sure before the answer is used
    static constexpr int MIN_SURE = 2;

    using MapData::Grid;

    // what Check was asked
    struct Job {
        int id = 0;
        MapFollow::Patches patches;
        RECT game = {};
        MapLocator::Result view;
    };

    static std::map<int, Grid> grids;
    static std::thread worker;
    static std::mutex mutex;
    static std::condition_variable wake;
    // guarded by mutex
    static bool stopping = false, waiting = false, busy = false;
    static Job job;
    static bool answered = false;
    static int answerId = 0;
    static MapLocator::Result answer;

    // worker thread only
    static MapData::Tiles tiles(TILES_KEPT);

    // One patch of the screen as the tiles say it should look if the map is
    // at view, drawn right at the compare size. Empty if it would take too
    // many tiles.
    static cv::Mat draw(const Grid& grid, const MapLocator::Result& view, const RECT& game, const RECT& rect) {
        int n = MapFollow::SIZE;
        double screenPerPixel = (double)(rect.right - rect.left) / n;
        double upp = view.unitsPerPixel;
        // tiles shrunk so a tile pixel is about a drawn pixel, like the grab gets shrunk
        int shrink = 1;
        while (screenPerPixel * upp / shrink > 1.5 && shrink < 16) shrink *= 2;

        double cx = (game.left + game.right) / 2.0, cy = (game.top + game.bottom) / 2.0;
        double perPixel = screenPerPixel * upp;
        if (perPixel / shrink * n > 1000.0) return {};
        return tiles.Draw(view.mapId, grid, (rect.left - cx) * upp + view.lng, (rect.top - cy) * upp + view.lat, n, perPixel, shrink);
    }

    // where the map really is, false if the tiles can't tell
    static bool compute(const Job& job, MapLocator::Result& out) {
        const MapLocator::Result& view = job.view;
        auto grid = grids.find(view.mapId);
        // Underground the game shows the floor's own art over a dimmed surface,
        // the surface tiles only half match it.
        if (grid == grids.end() || view.unitsPerPixel <= 0.0 || view.groupId != 0) return false;

        std::vector<cv::Mat> drawn;
        for (const RECT& rect : MapFollow::patchRects(job.game)) {
            cv::Mat patch = draw(grid->second, view, job.game, rect);
            if (patch.empty()) return false;
            drawn.push_back(patch);
        }
        MapFollow::Patches tilePatches = MapFollow::shrink(drawn);

        // how the game's map sits compared to the tiles drawn where we think it is
        MapFollow::Motion off = MapFollow::compare(tilePatches, job.patches, job.game);
        if (!off.found) return false;
        // over the sea or zoomed far out the patches barely look like the tiles
        int sure = 0;
        for (size_t i = 0; i < tilePatches.size() && i < job.patches.size(); ++i) {
            double confidence = 0.0;
            if (!tilePatches[i].empty() && !job.patches[i].empty()) cv::phaseCorrelate(tilePatches[i], job.patches[i], cv::noArray(), &confidence);
            if (confidence > SURE) sure++;
        }
        if (sure < MIN_SURE) return false;
        out = MapFollow::moveView(view, off);
        return true;
    }

    static void loop() {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, []() { return stopping || waiting; });
            if (stopping) return;
            Job current = std::move(job);
            waiting = false;
            busy = true;
            lock.unlock();

            MapLocator::Result result;
            bool found = false;
            try {
                found = compute(current, result);
            }
            catch (...) {
                // a check that goes wrong is skipped
            }

            lock.lock();
            busy = false;
            if (found) {
                answered = true;
                answerId = current.id;
                answer = result;
            }
        }
    }

    void Start(const std::filesystem::path& dataFolder) {
        if (worker.joinable()) return;
        grids = MapData::readManifest(dataFolder).grids;
        stopping = false;
        worker = std::thread(loop);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    void Check(int id, const MapFollow::Patches& patches, const RECT& game, const MapLocator::Result& view) {
        std::unique_lock<std::mutex> lock(mutex);
        if (busy || waiting) return;
        job.id = id;
        job.patches.clear();
        for (const cv::Mat& patch : patches) job.patches.push_back(patch.clone());
        job.game = game;
        job.view = view;
        waiting = true;
        lock.unlock();
        wake.notify_one();
    }

    bool Take(int& id, MapLocator::Result& result) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!answered) return false;
        answered = false;
        id = answerId;
        result = answer;
        return true;
    }
}
