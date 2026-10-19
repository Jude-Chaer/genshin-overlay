#include "map_tiles.hpp"
#include "map_data.hpp"
#include "map_tracking.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

namespace MapTiles {
    static constexpr int WORKER_COUNT = 6;
    static constexpr int UPLOADS_PER_FRAME = 64;
    static constexpr uint64_t FRAMES_BEFORE_FREE = 120;
    // a request nobody asked for again in this many frames is skipped (the map moved on)
    static constexpr uint64_t FRAMES_BEFORE_STALE = 30;

    // map, x, y, size for tiles. Floors use FLOOR_SIZE as size, with group and floor for x and y.
    typedef std::tuple<int, int, int, int> Key;
    static constexpr int FLOOR_SIZE = -1;
    static constexpr int MAX_FLOOR_SIZE = 2048;

    struct Entry {
        GLuint texture = 0;
        uint64_t lastUsed = 0;
    };

    struct Job {
        Key key;
        uint64_t frame = 0;
    };

    struct Finished {
        Key key;
        cv::Mat image;  // empty if there's no tile or it failed
        bool stale = false;
    };

    static fs::path dataFolder;
    static bool gridsLoaded = false;
    static std::map<int, MapData::Grid> maps;
    // by group and floor
    static std::map<std::pair<int, int>, MapData::Floor> floors;

    // main thread only
    static std::map<Key, Entry> textures;
    static std::set<Key> pending;
    static std::atomic<uint64_t> frame{ 0 };

    // shared with the workers
    static std::mutex mutex;
    static std::condition_variable wake;
    static std::deque<Job> queue;
    static std::vector<Finished> finished;
    static std::vector<std::thread> workers;
    static bool stopping = false;

    static bool readBytes(const fs::path& path, std::vector<uchar>& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    // the manifest says where each map's tiles and floors are
    static void loadGrids() {
        if (gridsLoaded || MapTracking::GetStatus() != MapTracking::Status::Ready) return;

        MapData::Manifest manifest = MapData::readManifest(dataFolder);
        maps = manifest.grids;
        for (const MapData::Floor& floor : manifest.floors) floors[{ floor.groupId, floor.floorId }] = floor;
        gridsLoaded = true;
    }

    static Finished loadFloor(const Key& key) {
        Finished result;
        result.key = key;
        auto it = floors.find({ std::get<1>(key), std::get<2>(key) });
        if (it == floors.end()) return result;
        const MapData::Floor& info = it->second;

        std::vector<uchar> bytes;
        if (!MapData::fetch(info.url, info.path) || !readBytes(info.path, bytes) || bytes.empty()) return result;
        try {
            // keep the transparency, the image only covers part of its box
            cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
            if (image.channels() == 3) cv::cvtColor(image, image, cv::COLOR_BGR2BGRA);
            if (image.channels() == 1) cv::cvtColor(image, image, cv::COLOR_GRAY2BGRA);
            int longest = (std::max)(image.cols, image.rows);
            if (longest > MAX_FLOOR_SIZE) {
                double scale = (double)MAX_FLOOR_SIZE / longest;
                cv::resize(image, image, cv::Size(), scale, scale, cv::INTER_AREA);
            }
            result.image = image;
        }
        catch (...) {
        }
        return result;
    }

    static Finished loadTile(const Key& key) {
        if (std::get<3>(key) == FLOOR_SIZE) return loadFloor(key);

        Finished result;
        result.key = key;
        int mapId = std::get<0>(key), x = std::get<1>(key), y = std::get<2>(key), size = std::get<3>(key);

        auto it = maps.find(mapId);
        if (it == maps.end()) return result;

        std::vector<uchar> bytes;
        if (!MapData::fetchTile(mapId, it->second, x, y) || !readBytes(MapData::tilePath(it->second, x, y), bytes) || bytes.empty()) return result;
        cv::Mat image;
        try {
            image = cv::imdecode(bytes, cv::IMREAD_COLOR);
            if (!image.empty() && image.cols != size) {
                cv::resize(image, image, cv::Size(size, size), 0, 0, cv::INTER_AREA);
            }
        }
        catch (...) {
            image.release();
        }
        result.image = image;
        return result;
    }

    static void workerLoop() {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, []() { return stopping || !queue.empty(); });
            if (stopping) return;

            // newest first, that's what's on screen right now
            Job job = queue.back();
            queue.pop_back();
            lock.unlock();

            Finished result;
            if (frame - job.frame > FRAMES_BEFORE_STALE) {
                result.key = job.key;
                result.stale = true;
            }
            else {
                result = loadTile(job.key);
            }

            lock.lock();
            finished.push_back(std::move(result));
        }
    }

    void Init(const fs::path& folder) {
        dataFolder = folder;
        stopping = false;
        for (int i = 0; i < WORKER_COUNT; i++) {
            workers.emplace_back(workerLoop);
        }
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto& worker : workers) worker.join();
        workers.clear();

        for (auto& entry : textures) {
            if (entry.second.texture != 0) glDeleteTextures(1, &entry.second.texture);
        }
        textures.clear();
    }

    bool GetGrid(int mapId, Grid& out) {
        loadGrids();
        auto it = maps.find(mapId);
        if (it == maps.end()) return false;
        const MapData::Grid& grid = it->second;
        out = { grid.cols, grid.rows, grid.tileSize, grid.originX, grid.originY };
        return true;
    }

    static void request(const Key& key) {
        if (pending.count(key) != 0) return;
        pending.insert(key);
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back({ key, frame.load() });
        }
        wake.notify_one();
    }

    bool GetFloor(int groupId, int floorId, Floor& out) {
        loadGrids();
        auto info = floors.find({ groupId, floorId });
        if (info == floors.end()) return false;

        Key key(0, groupId, floorId, FLOOR_SIZE);
        auto it = textures.find(key);
        if (it == textures.end()) {
            request(key);
            return false;
        }
        it->second.lastUsed = frame;
        if (it->second.texture == 0) return false;

        const MapData::Floor& box = info->second;
        out = { it->second.texture, box.left, box.top, box.right, box.bottom };
        return true;
    }

    std::vector<std::pair<int, int>> FloorsAt(int mapId, double x, double y) {
        loadGrids();
        std::vector<std::pair<int, int>> out;
        for (const auto& [key, floor] : floors) {
            if (floor.mapId != mapId) continue;
            if (x >= (std::min)(floor.left, floor.right) && x <= (std::max)(floor.left, floor.right)
                && y >= (std::min)(floor.top, floor.bottom) && y <= (std::max)(floor.top, floor.bottom)) {
                out.push_back(key);
            }
        }
        return out;
    }

    GLuint GetTile(int mapId, int x, int y, int size) {
        loadGrids();
        if (maps.count(mapId) == 0) return 0;

        Key key(mapId, x, y, size);
        auto it = textures.find(key);
        if (it != textures.end()) {
            it->second.lastUsed = frame;
            return it->second.texture;
        }

        request(key);

        // while it loads, use the same tile at another size if we have it, so zooming doesn't flicker
        for (int other : { 256, 128, 64, 32 }) {
            auto fallback = textures.find(Key(mapId, x, y, other));
            if (fallback != textures.end() && fallback->second.texture != 0) {
                fallback->second.lastUsed = frame;
                return fallback->second.texture;
            }
        }
        return 0;
    }

    static GLuint upload(const cv::Mat& image) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (image.channels() == 4) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.cols, image.rows, 0, GL_BGRA, GL_UNSIGNED_BYTE, image.data);
        }
        else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, image.cols, image.rows, 0, GL_BGR, GL_UNSIGNED_BYTE, image.data);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return texture;
    }

    void EndFrame() {
        std::vector<Finished> done;
        {
            std::lock_guard<std::mutex> lock(mutex);
            size_t count = (std::min)(finished.size(), (size_t)UPLOADS_PER_FRAME);
            done.assign(std::make_move_iterator(finished.begin()), std::make_move_iterator(finished.begin() + count));
            finished.erase(finished.begin(), finished.begin() + count);
        }

        for (auto& result : done) {
            pending.erase(result.key);
            if (result.stale) continue;

            // an empty entry means nothing to draw there, it gets retried once it's freed
            Entry entry;
            entry.lastUsed = frame;
            if (!result.image.empty()) {
                entry.texture = upload(result.image);
            }
            textures[result.key] = entry;
        }

        uint64_t now = ++frame;
        for (auto it = textures.begin(); it != textures.end();) {
            if (now - it->second.lastUsed > FRAMES_BEFORE_FREE) {
                if (it->second.texture != 0) glDeleteTextures(1, &it->second.texture);
                it = textures.erase(it);
            }
            else {
                ++it;
            }
        }
    }
}
