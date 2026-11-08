#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json_fwd.hpp>

// Finds the key the game encrypts its messages with after the login. Based on
// auto-artifactarium (MIT), see packets/NOTICE.txt.
//
// The key is KeyFromSeed(number ^ server seed). The server seed comes RSA
// encrypted in the login answer. The number is drawn from .NET's Random,
// seeded with the time the game made it, so it is one of 2^31 values.

namespace SessionKey {
    struct Keys {
        // the login key for each dispatch key id
        std::map<uint16_t, std::string> dispatch;
        std::vector<std::string> rsaPem;
    };

    // dispatch-keys.json and rsa-*.pem from the folder
    extern bool LoadKeys(const std::filesystem::path& folder, Keys& out);
    // The keys of a dispatch-keys.json, {"id": "base64 key"}, into out. How
    // many were left out for a bad id or a key that isn't 4096 bytes.
    extern size_t AddDispatch(const nlohmann::json& keys, Keys& out);

    // .NET Framework's System.Random
    class CsRandom {
    public:
        explicit CsRandom(int32_t seed);
        double NextDouble();
        uint64_t NextSafeUInt64();
        // what NextSafeUInt64 gives for one of the 2^31 samples
        static uint64_t Number(int32_t sample);

    private:
        int32_t sample();
        int32_t m_seeds[56] = {};
        int m_next = 0, m_nextP = 21;
    };

    extern std::string KeyFromSeed(uint64_t seed);

    struct Found {
        // the time the generator was seeded at and which of its numbers, 0 when found by sample
        uint64_t timeSeed = 0;
        int number = 0;
        int32_t sample = 0;
        uint64_t serverSeed = 0;
        std::string key;
    };

    // The key has to fit every one of messages, the first of them is checked
    // first. One message alone lets about one wrong key in 4 billion through.
    // Numbers first to first + count - 1 of each time seed within window ms of time.
    extern bool SearchNear(uint64_t time, int window, int first, int count, const std::vector<uint64_t>& serverSeeds,
        const std::vector<std::string>& messages, Found& out);
    // Every time seed from from to to, numbers 1 to depth, on all cores but one.
    // done counts the time seeds tried, for the time left.
    extern bool SearchBetween(uint64_t from, uint64_t to, int depth, const std::vector<uint64_t>& serverSeeds,
        const std::vector<std::string>& messages, const std::atomic<bool>& cancel, Found& out,
        std::atomic<uint64_t>* done = nullptr);
    // Every sample from first to last, no time needed. All of them is
    // SAMPLES_FIRST to SAMPLES_LAST.
    inline constexpr int32_t SAMPLES_FIRST = 0, SAMPLES_LAST = INT32_MAX - 1;
    extern bool SearchSamples(int32_t first, int32_t last, const std::vector<uint64_t>& serverSeeds,
        const std::vector<std::string>& messages, const std::atomic<bool>& cancel, Found& out,
        std::atomic<uint64_t>* done = nullptr);

    // reads the server seed out of the login answer
    class Rsa {
    public:
        explicit Rsa(const std::vector<std::string>& pems);
        ~Rsa();
        Rsa(const Rsa&) = delete;
        Rsa& operator=(const Rsa&) = delete;

        // there was at least one key and every one loaded
        bool AllLoaded() const { return m_wanted > 0 && m_keys.size() == m_wanted; }
        // every 8 byte answer any key gives for any bytes field of body
        std::vector<uint64_t> ServerSeeds(std::string_view body) const;

    private:
        void* m_algorithm = nullptr;
        std::vector<void*> m_keys;
        size_t m_wanted = 0;
    };
}
