#include "map_check.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace MapCheck {
    // decoded tiles kept in memory, 64 KB each at full size, less shrunk
    static constexpr size_t TILES_KEPT = 300;
    // a patch looks like the tiles when phase correlation is at least this sure (0 to 1)
    static constexpr double SURE = 0.7;
    // patches that have to be sure before the answer is used
    static constexpr int MIN_SURE = 2;

    // one map's tiles on disk: cols x rows files of tileSize pixels, named
    // x_y.webp. Tile pixel = map units + origin.
    struct Grid {
        int cols = 0, rows = 0, tileSize = 256;
        double originX = 0.0, originY = 0.0;
        std::filesystem::path dir;
    };

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

    using TileKey = std::tuple<int, int, int, int>;  // map, x, y, shrunk by
    static std::map<TileKey, cv::Mat> tiles;
    static std::deque<TileKey> tileOrder;

    // one tile as gray, shrunk by shrink, empty if there's no such tile on disk. Worker thread only.
    static const cv::Mat& tile(int mapId, const Grid& grid, int x, int y, int shrink) {
        TileKey key{ mapId, x, y, shrink };
        auto it = tiles.find(key);
        if (it != tiles.end()) return it->second;

        cv::Mat image;
        std::filesystem::path path = grid.dir / (std::to_string(x) + "_" + std::to_string(y) + ".webp");
        std::ifstream file(path, std::ios::binary);
        std::vector<uchar> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!bytes.empty()) image = cv::imdecode(bytes, cv::IMREAD_GRAYSCALE);
        if (!image.empty() && shrink > 1) cv::resize(image, image, cv::Size(grid.tileSize / shrink, grid.tileSize / shrink), 0, 0, cv::INTER_AREA);

        if (tileOrder.size() >= TILES_KEPT) {
            tiles.erase(tileOrder.front());
            tileOrder.pop_front();
        }
        tileOrder.push_back(key);
        return tiles[key] = image;
    }

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
        int size = grid.tileSize / shrink;

        // drawn pixel u lands on tile pixel a * u + b, pixel middles lined up
        double cx = (game.left + game.right) / 2.0, cy = (game.top + game.bottom) / 2.0;
        double a = screenPerPixel * upp / shrink;
        double bx = ((rect.left + 0.5 * screenPerPixel - cx) * upp + view.lng + grid.originX) / shrink - 0.5;
        double by = ((rect.top + 0.5 * screenPerPixel - cy) * upp + view.lat + grid.originY) / shrink - 0.5;
        int x0 = (int)std::floor(bx) - 2, y0 = (int)std::floor(by) - 2;
        int x1 = (int)std::ceil(bx + a * (n - 1)) + 3, y1 = (int)std::ceil(by + a * (n - 1)) + 3;
        if (x1 - x0 > 1000 || y1 - y0 > 1000) return {};

        cv::Mat area(y1 - y0, x1 - x0, CV_8U, cv::Scalar(0));
        for (int ty = std::max(0, (int)std::floor((double)y0 / size)); ty <= std::min(grid.rows - 1, (y1 - 1) / size); ++ty) {
            for (int tx = std::max(0, (int)std::floor((double)x0 / size)); tx <= std::min(grid.cols - 1, (x1 - 1) / size); ++tx) {
                const cv::Mat& image = tile(view.mapId, grid, tx, ty, shrink);
                if (image.empty()) continue;
                cv::Rect in(tx * size, ty * size, size, size);
                cv::Rect both = in & cv::Rect(x0, y0, x1 - x0, y1 - y0);
                image(both - in.tl()).copyTo(area(both - cv::Point(x0, y0)));
            }
        }

        cv::Mat patch;
        cv::Matx23d to(a, 0.0, bx - x0, 0.0, a, by - y0);
        cv::warpAffine(area, patch, to, cv::Size(n, n), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar(0));
        return patch;
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
            bool found = compute(current, result);

            lock.lock();
            busy = false;
            if (found) {
                answered = true;
                answerId = current.id;
                answer = result;
            }
        }
    }

    static void load(const std::filesystem::path& dataFolder) {
        grids.clear();
        std::ifstream manifest(dataFolder / "manifest.tsv");
        std::string text;
        while (std::getline(manifest, text)) {
            std::istringstream fields(text);
            std::string kind, dir;
            int id = 0;
            Grid grid;
            if (!(fields >> kind) || kind != "grid") continue;
            if (!(fields >> id >> grid.cols >> grid.rows >> grid.tileSize >> grid.originX >> grid.originY >> dir)) continue;
            grid.dir = dataFolder / std::filesystem::u8path(dir);
            grids[id] = grid;
        }
    }

    void Start(const std::filesystem::path& dataFolder) {
        if (worker.joinable()) return;
        load(dataFolder);
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
