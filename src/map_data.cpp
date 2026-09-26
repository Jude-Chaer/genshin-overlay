#include "map_data.hpp"
#include "net.hpp"
#include "locator/map_locator.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>
#include <zlib.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace MapData {
    static const std::string API = "https://sg-public-api.hoyolab.com/common/map_user/ys_obc";
    static const std::string RELEASE = "https://github.com/Jude-Chaer/genshin-overlay/releases/download/map-index/";
    static const std::string INDEX_FILE = "map-index.bin.gz";
    static const std::string META_FILE = "map-index.json";
    static constexpr int META_FORMAT = 1;
    static constexpr int DOWNLOAD_THREADS = 16;

    // Teyvat, Enkanomiya, The Chasm, and the separate maps from later versions.
    // Ids without their own tiles get skipped.
    static constexpr int MAP_IDS[] = { 2, 7, 9, 34, 36, 37, 40 };

    static bool getJson(const std::string& path, json& data) {
        Net::Response response;
        if (!Net::get(API + path, response, true) || response.status != 200) return false;

        json body = json::parse(response.body, nullptr, false);
        if (body.is_discarded() || body.value("retcode", -1) != 0 || !body.contains("data")) return false;
        data = body["data"];
        return true;
    }

    // numbers go into the manifest exactly as HoYoLAB sends them
    static std::string number(const json& value) {
        return value.dump();
    }

    static std::string readText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    static bool writeFile(const fs::path& path, const std::string& data) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        fs::path tmp = path;
        tmp += ".part";
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file.write(data.data(), (std::streamsize)data.size());
            if (!file) return false;
        }
        fs::rename(tmp, path, ec);
        return !ec;
    }

    bool makePlan(Plan& plan) {
        std::vector<std::string> lines = { "# genshin-overlay map manifest v1" };
        plan.files.clear();
        plan.tileDirs.clear();

        try {
            for (int id : MAP_IDS) {
                std::string query = "map_id=" + std::to_string(id) + "&app_sn=ys_obc&lang=en-us";
                json info;
                if (!getJson("/v1/map/info?" + query, info)) return false;

                const json& detail = info["info"]["detail_v2"];
                if (!detail.is_object() || !detail.contains("total_size") || !detail.contains("origin")) continue;
                const json& size = detail["total_size"];
                const json& origin = detail["origin"];
                std::string version = detail.value("map_version", "");
                if (size.size() < 2 || origin.size() < 2 || version.empty()) continue;

                // the map is cut into 256px tiles named x_y
                const int tile = 256;
                int cols = size[0].get<int>() / tile;
                int rows = size[1].get<int>() / tile;
                std::string dir = "tiles/" + std::to_string(id) + "_" + version;
                plan.tileDirs.insert(std::to_string(id) + "_" + version);
                for (int y = 0; y < rows; y++) {
                    for (int x = 0; x < cols; x++) {
                        std::string name = std::to_string(x) + "_" + std::to_string(y);
                        plan.files.push_back({
                            "https://act-webstatic.hoyoverse.com/map_manage/map/" + std::to_string(id) + "/" + version + "/" + name + "_P0.webp",
                            dir + "/" + name + ".webp"
                        });
                    }
                }
                lines.push_back("grid\t" + std::to_string(id) + "\t" + std::to_string(cols) + "\t" + std::to_string(rows) + "\t"
                    + std::to_string(tile) + "\t" + number(origin[0]) + "\t" + number(origin[1]) + "\t" + dir);

                // underground floors, each one is a single image placed over the map
                json groups;
                if (!getJson("/v2/map/point_group?" + query, groups)) return false;
                for (const json& group : groups.value("list", json::array())) {
                    for (const json& floor : group.value("floors", json::array())) {
                        if (!floor.contains("overlay") || !floor["overlay"].is_object()) continue;
                        const json& overlay = floor["overlay"];
                        std::string url = overlay.value("url", "");
                        if (url.empty()) continue;

                        // the file name is a hash of the image, so a redrawn floor gets a new name
                        std::string fileName = url.substr(url.find_last_of('/') + 1);
                        std::string path = "overlays/" + number(floor["id"]) + "_" + fileName;
                        plan.files.push_back({ url, path });
                        lines.push_back("image\t" + std::to_string(id) + "\t" + number(group["id"]) + "\t" + number(floor["id"]) + "\t"
                            + number(overlay["l_x"]) + "\t" + number(overlay["l_y"]) + "\t" + path + "\t"
                            + number(overlay["r_x"]) + "\t" + number(overlay["r_y"]));
                    }
                }
            }
        }
        catch (const json::exception& e) {
            std::cerr << "Unexpected map API response: " << e.what() << std::endl;
            return false;
        }

        plan.manifest.clear();
        for (const auto& line : lines) {
            plan.manifest += line + "\n";
        }
        return true;
    }

    bool downloadMissing(const fs::path& folder, const std::vector<File>& files, const Progress& progress, const std::atomic<bool>* cancel) {
        std::vector<const File*> missing;
        for (const auto& file : files) {
            if (!fs::exists(folder / fs::u8path(file.path))) {
                missing.push_back(&file);
            }
        }
        if (missing.empty()) return true;

        std::atomic<size_t> next{ 0 };
        std::atomic<size_t> done{ 0 };
        std::atomic<bool> failed{ false };
        auto stop = [&]() { return failed || (cancel && cancel->load()); };

        auto worker = [&]() {
            while (!stop()) {
                size_t i = next.fetch_add(1);
                if (i >= missing.size()) return;

                const File& file = *missing[i];
                fs::path target = folder / fs::u8path(file.path);
                bool saved = false;
                for (int attempt = 0; attempt < 3 && !saved; attempt++) {
                    Net::Response response;
                    if (!Net::get(file.url, response, true)) continue;
                    if (response.status == 200) {
                        saved = writeFile(target, response.body);
                    }
                    // tiles outside the drawn map don't exist, an empty file stops us asking again
                    else if (response.status == 404) {
                        saved = writeFile(target, "");
                    }
                }
                if (!saved) failed = true;
                done++;
            }
        };

        std::vector<std::thread> pool;
        for (int t = 0; t < DOWNLOAD_THREADS; t++) {
            pool.emplace_back(worker);
        }
        while (done < missing.size() && !stop()) {
            if (progress) progress("Downloading map images", (int)(done * 100 / missing.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        for (auto& thread : pool) thread.join();
        return !stop();
    }

    // tile folders from map versions HoYoLAB has replaced
    static void removeOldTiles(const fs::path& folder, const Plan& plan) {
        std::error_code ec;
        fs::path tiles = folder / "tiles";
        if (!fs::exists(tiles)) return;
        for (const auto& entry : fs::directory_iterator(tiles, ec)) {
            if (entry.is_directory() && plan.tileDirs.count(entry.path().filename().string()) == 0) {
                fs::remove_all(entry.path(), ec);
            }
        }
    }

    static bool gunzip(const std::string& in, std::string& out, size_t expectedSize) {
        z_stream stream = {};
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
        out.resize(expectedSize);
        stream.next_in = (Bytef*)in.data();
        stream.avail_in = (uInt)in.size();
        stream.next_out = (Bytef*)out.data();
        stream.avail_out = (uInt)out.size();
        int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        return result == Z_STREAM_END && stream.total_out == expectedSize;
    }

    static bool gzip(const std::string& in, std::string& out) {
        z_stream stream = {};
        if (deflateInit2(&stream, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
        out.resize(deflateBound(&stream, (uLong)in.size()));
        stream.next_in = (Bytef*)in.data();
        stream.avail_in = (uInt)in.size();
        stream.next_out = (Bytef*)out.data();
        stream.avail_out = (uInt)out.size();
        int result = deflate(&stream, Z_FINISH);
        out.resize(stream.total_out);
        deflateEnd(&stream);
        return result == Z_STREAM_END;
    }

    static std::string sha256(const std::string& data) {
        unsigned char hash[32] = {};
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), hash, sizeof(hash));
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned char c : hash) {
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        return out;
    }

    static bool fetchPublishedMeta(json& meta) {
        Net::Response response;
        if (!Net::get(RELEASE + META_FILE, response) || response.status != 200) return false;
        meta = json::parse(response.body, nullptr, false);
        return !meta.is_discarded() && meta.value("format", 0) == META_FORMAT;
    }

    bool downloadPrebuiltIndex(const fs::path& folder, const Plan& plan, const Progress& progress) {
        json meta;
        if (!fetchPublishedMeta(meta) || meta.value("manifest", "") != plan.manifest) return false;

        if (progress) progress("Downloading map index", 0);
        Net::Response response;
        if (!Net::get(RELEASE + meta.value("file", INDEX_FILE), response) || response.status != 200) return false;

        std::string raw;
        size_t rawSize = meta.value("rawSize", (size_t)0);
        if (!gunzip(response.body, raw, rawSize) || sha256(raw) != meta.value("sha256", "")) {
            std::cerr << "The downloaded map index is damaged" << std::endl;
            return false;
        }

        fs::path index = folder / "index.bin";
        if (!writeFile(index, raw)) return false;
        return MapLocator::IndexMatchesManifest(index, plan.manifest);
    }

    bool ensure(const fs::path& folder, const Progress& progress, const std::atomic<bool>* cancel) {
        fs::path manifestPath = folder / "manifest.tsv";

        if (progress) progress("Checking map data", 0);
        Plan plan;
        if (!makePlan(plan)) {
            std::cerr << "Couldn't reach HoYoLAB, using the map data already on disk" << std::endl;
            return fs::exists(manifestPath);
        }

        if (!fs::exists(manifestPath) || readText(manifestPath) != plan.manifest) {
            if (!writeFile(manifestPath, plan.manifest)) return false;
        }

        if (MapLocator::IndexMatchesManifest(folder / "index.bin", plan.manifest)) return true;
        if (downloadPrebuiltIndex(folder, plan, progress)) return true;

        // no matching index published yet, get the images and build it here
        removeOldTiles(folder, plan);
        return downloadMissing(folder, plan.files, progress, cancel);
    }

    int makeIndex(const fs::path& folder) {
        std::cout << "Reading HoYoLAB map info..." << std::endl;
        Plan plan;
        if (!makePlan(plan)) {
            std::cerr << "Couldn't read the map API" << std::endl;
            return 1;
        }

        json published;
        if (fetchPublishedMeta(published) && published.value("manifest", "") == plan.manifest) {
            std::cout << "The published index is already current" << std::endl;
            return 3;
        }

        std::cout << plan.files.size() << " images" << std::endl;
        int lastShown = -10;
        bool downloaded = downloadMissing(folder, plan.files, [&](const char*, int percent) {
            if (percent >= lastShown + 10) {
                lastShown = percent;
                std::cout << "  " << percent << "%" << std::endl;
            }
        });
        if (!downloaded) {
            std::cerr << "Download failed" << std::endl;
            return 1;
        }
        removeOldTiles(folder, plan);
        fs::path manifestPath = folder / "manifest.tsv";
        if (!writeFile(manifestPath, plan.manifest)) return 1;

        std::cout << "Building the index..." << std::endl;
        MapLocator::Index index;
        if (!index.Load(manifestPath)) {
            std::cerr << "Building the index failed" << std::endl;
            return 1;
        }

        std::string raw = readText(folder / "index.bin");
        std::string packed;
        if (raw.empty() || !gzip(raw, packed)) return 1;

        char built[32];
        std::time_t now = std::time(nullptr);
        std::tm utc;
        gmtime_s(&utc, &now);
        std::strftime(built, sizeof(built), "%Y-%m-%dT%H:%M:%SZ", &utc);

        json meta = {
            { "format", META_FORMAT },
            { "built", built },
            { "file", INDEX_FILE },
            { "size", packed.size() },
            { "rawSize", raw.size() },
            { "sha256", sha256(raw) },
            { "manifest", plan.manifest },
        };

        fs::path out = folder / "out";
        if (!writeFile(out / INDEX_FILE, packed) || !writeFile(out / META_FILE, meta.dump(1))) return 1;
        std::cout << "Wrote " << INDEX_FILE << " (" << packed.size() / 1000000.0 << " MB) and " << META_FILE << std::endl;
        return 0;
    }
}
