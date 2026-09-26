#include "map_tiles.hpp"
#include "map_tracking.hpp"
#include "net.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
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

    struct MapInfo {
        Grid grid;
        std::string version;
        fs::path dir;
    };

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
    static std::map<int, MapInfo> maps;

    struct FloorInfo {
        Floor box;
        fs::path path;
        std::string url;
    };
    static std::map<std::pair<int, int>, FloorInfo> floors;

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

    static void writeBytes(const fs::path& path, const std::string& data) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        fs::path tmp = path;
        tmp += ".part";
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            file.write(data.data(), (std::streamsize)data.size());
        }
        fs::rename(tmp, path, ec);
    }

    // the grid lines of the manifest say where each map's tiles are
    static void loadGrids() {
        if (gridsLoaded || MapTracking::GetStatus() != MapTracking::Status::Ready) return;

        std::ifstream file(dataFolder / "manifest.tsv");
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream fields(line);
            std::string kind, dir;
            int id = 0;
            MapInfo info;
            if (!(fields >> kind) || kind != "grid") continue;
            if (!(fields >> id >> info.grid.cols >> info.grid.rows >> info.grid.tileSize >> info.grid.originX >> info.grid.originY >> dir)) continue;

            // dir is tiles/<id>_<version>
            info.dir = dataFolder / fs::u8path(dir);
            info.version = dir.substr(dir.find('_') + 1);
            maps[id] = info;
        }

        std::map<std::string, std::string> urls;
        std::ifstream urlFile(dataFolder / "overlay_urls.tsv");
        while (std::getline(urlFile, line)) {
            size_t tab = line.find('\t');
            if (tab != std::string::npos) urls[line.substr(0, tab)] = line.substr(tab + 1);
        }

        // image <map> <group> <floor> <left> <top> <path> <right> <bottom>
        file.clear();
        file.seekg(0);
        while (std::getline(file, line)) {
            std::istringstream fields(line);
            std::string kind, path;
            int mapId = 0, groupId = 0, floorId = 0;
            FloorInfo info;
            if (!(fields >> kind) || kind != "image") continue;
            if (!(fields >> mapId >> groupId >> floorId >> info.box.left >> info.box.top >> path >> info.box.right >> info.box.bottom)) continue;
            info.path = dataFolder / fs::u8path(path);
            info.url = urls[path];
            floors[{ groupId, floorId }] = info;
        }
        gridsLoaded = true;
    }

    static Finished loadFloor(const Key& key) {
        Finished result;
        result.key = key;
        auto it = floors.find({ std::get<1>(key), std::get<2>(key) });
        if (it == floors.end()) return result;
        const FloorInfo& info = it->second;

        if (!fs::exists(info.path)) {
            Net::Response response;
            if (info.url.empty() || !Net::get(info.url, response, true) || response.status != 200) return result;
            writeBytes(info.path, response.body);
        }

        std::vector<uchar> bytes;
        if (!readBytes(info.path, bytes) || bytes.empty()) return result;
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
        const MapInfo& info = it->second;

        fs::path path = info.dir / (std::to_string(x) + "_" + std::to_string(y) + ".webp");
        if (!fs::exists(path)) {
            std::string url = "https://act-webstatic.hoyoverse.com/map_manage/map/" + std::to_string(mapId) + "/" + info.version + "/"
                + std::to_string(x) + "_" + std::to_string(y) + "_P0.webp";
            Net::Response response;
            if (!Net::get(url, response, true)) return result;
            if (response.status == 200) {
                writeBytes(path, response.body);
            }
            // outside the drawn map, remember that there's nothing there
            else if (response.status == 404) {
                writeBytes(path, "");
            }
            else {
                return result;
            }
        }

        std::vector<uchar> bytes;
        if (!readBytes(path, bytes) || bytes.empty()) return result;
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
        out = it->second.grid;
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

        out = info->second.box;
        out.texture = it->second.texture;
        return true;
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
