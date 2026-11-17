#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <nlohmann/json_fwd.hpp>
#include "net_stop.hpp"

// Files on disk and from the web that the map data and the map items both use.

namespace Files {
    // the "map-index" release, where the workflow puts what the app downloads
    extern const std::string RELEASE;

    // the whole file, empty if it can't be read
    extern std::string readText(const std::filesystem::path& path);
    // Written under another name first, so a file that's there is whole.
    extern bool writeFile(const std::filesystem::path& path, const std::string& data);

    // gzip with the size it unpacks to, which has to match
    extern bool gunzip(const std::string& in, std::string& out, size_t expectedSize);
    extern bool gzip(const std::string& in, std::string& out);
    // as lowercase hex
    extern std::string sha256(const std::string& data);
    // the time now (or secondsAgo before it) as a meta file's "built", 2026-10-17T13:42:00Z
    extern std::string utcNow(long long secondsAgo = 0);

    // A meta file comes from the internet, so every value is checked before
    // it's used: the key's value, or otherwise when it's missing or another type.
    extern std::string stringField(const nlohmann::json& meta, const char* key, const std::string& otherwise = "");
    extern size_t sizeField(const nlohmann::json& meta, const char* key);

    // A release's meta file (items.json, map-index.json) says the format of
    // the file it goes with, the size that file unzips to and its sha256.
    //   Missing: the release has none (404)
    //   Unreached: it couldn't be downloaded
    //   Broken: it isn't JSON with a whole number format, or it's an older format
    //   Newer: it's a format this overlay can't read yet
    enum class Meta { Ok, Missing, Unreached, Broken, Newer };
    extern Meta readMeta(const std::string& body, int format, nlohmann::json& meta);
    // the release's meta file by name, read as above
    extern Meta getMeta(const std::string& name, int format, nlohmann::json& meta, Net::Stopper* stop = nullptr);
    // A gzipped file unzipped to its meta's rawSize, at most maxRawSize, and
    // checked against its sha256. False with the problem when it isn't right.
    extern bool unpack(const std::string& packed, const nlohmann::json& meta, size_t maxRawSize, std::string& raw, std::string& problem);
    // the release's file by name, downloaded and unpacked as above
    extern bool getChecked(const std::string& name, const nlohmann::json& meta, size_t maxRawSize, std::string& raw, std::string& problem,
        Net::Stopper* stop = nullptr);

    // Calls each(0) to each(count - 1), 16 at a time, for downloads. Starts no
    // more once stop says so (when given). tick, when given, is called on this
    // thread every 200 ms with how many are done. Returns when all have ended;
    // when one throws, no more start and its exception is thrown from here.
    extern void inParallel(size_t count, const std::function<void(size_t i)>& each, const std::function<bool()>& stop = nullptr,
        const std::function<void(size_t done)>& tick = nullptr);

    // the data part of HoYoLAB's map API answer, path is what goes after ys_obc
    extern bool getJson(const std::string& path, nlohmann::json& data, bool isStatic = false, Net::Stopper* stop = nullptr);
}
