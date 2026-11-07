#include "session_key.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>
#include "messages.hpp"

namespace SessionKey {
    static constexpr size_t KEY_SIZE = 4096;

    static std::string fromBase64(std::string_view text) {
        // a length of 0 would mean read up to a 0 byte
        if (text.empty())
            return {};
        DWORD size = 0;
        if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr))
            return {};
        std::string out(size, '\0');
        if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, (BYTE*)out.data(), &size, nullptr, nullptr))
            return {};
        out.resize(size);
        return out;
    }

    bool LoadKeys(const std::filesystem::path& folder, Keys& out) {
        std::ifstream file(folder / "dispatch-keys.json");
        if (!file)
            return false;
        nlohmann::json keys = nlohmann::json::parse(file, nullptr, false);
        if (!keys.is_object())
            return false;
        for (auto& [id, key] : keys.items()) {
            if (!key.is_string())
                continue;
            int number = std::atoi(id.c_str());
            std::string bytes = fromBase64(key.get<std::string>());
            if (number > 0 && number <= UINT16_MAX && bytes.size() == KEY_SIZE)
                out.dispatch[uint16_t(number)] = bytes;
        }

        std::error_code error;
        for (auto& entry : std::filesystem::directory_iterator(folder, error)) {
            std::string name = entry.path().filename().string();
            if (name.rfind("rsa-", 0) != 0 || entry.path().extension() != ".pem")
                continue;
            std::ifstream pem(entry.path(), std::ios::binary);
            std::stringstream text;
            text << pem.rdbuf();
            out.rsaPem.push_back(text.str());
        }
        return !out.dispatch.empty() && !out.rsaPem.empty();
    }

    CsRandom::CsRandom(int32_t seed) {
        const int32_t big = INT32_MAX;
        int32_t subtraction = seed == INT32_MIN ? INT32_MAX : (seed < 0 ? -seed : seed);
        int32_t mj = 161803398 - subtraction;
        m_seeds[55] = mj;
        int32_t mk = 1;
        for (int i = 1; i < 55; i++) {
            int ii = 21 * i % 55;
            m_seeds[ii] = mk;
            mk = mj - mk;
            if (mk < 0)
                mk += big;
            mj = m_seeds[ii];
        }
        for (int k = 1; k < 5; k++) {
            for (int i = 1; i < 56; i++) {
                m_seeds[i] = int32_t(uint32_t(m_seeds[i]) - uint32_t(m_seeds[1 + (i + 30) % 55]));
                if (m_seeds[i] < 0)
                    m_seeds[i] += big;
            }
        }
    }

    int32_t CsRandom::sample() {
        if (++m_next >= 56)
            m_next = 1;
        if (++m_nextP >= 56)
            m_nextP = 1;
        int32_t value = int32_t(uint32_t(m_seeds[m_next]) - uint32_t(m_seeds[m_nextP]));
        if (value == INT32_MAX)
            value--;
        if (value < 0)
            value += INT32_MAX;
        m_seeds[m_next] = value;
        return value;
    }

    double CsRandom::NextDouble() {
        return sample() * (1.0 / INT32_MAX);
    }

    uint64_t CsRandom::NextSafeUInt64() {
        return Number(sample());
    }

    uint64_t CsRandom::Number(int32_t sample) {
        return uint64_t(sample * (1.0 / INT32_MAX) * double(UINT64_MAX));
    }

    std::string KeyFromSeed(uint64_t seed) {
        std::mt19937_64 first(seed);
        std::mt19937_64 generator(first());
        generator.discard(1);
        std::string key(KEY_SIZE, '\0');
        for (size_t i = 0; i < KEY_SIZE / 8; i++) {
            uint64_t value = generator();
            for (size_t b = 0; b < 8; b++)
                key[i * 8 + b] = char(value >> (56 - 8 * b));
        }
        return key;
    }

    // the first 8 bytes of KeyFromSeed, enough to rule most guesses out
    static uint64_t keyHead(uint64_t seed) {
        std::mt19937_64 first(seed);
        std::mt19937_64 generator(first());
        generator.discard(1);
        return generator();
    }

    // what the first two key bytes must be for the message to start with 45 67
    static uint16_t wantedHead(const std::vector<std::string>& messages) {
        const std::string& data = messages[0];
        return uint16_t((uint8_t(data[0]) ^ 0x45) << 8 | (uint8_t(data[1]) ^ 0x67));
    }

    static bool tryNumber(uint64_t number, uint16_t head, const std::vector<uint64_t>& serverSeeds,
        const std::vector<std::string>& messages, uint64_t& serverSeed, std::string& key) {
        for (uint64_t seed : serverSeeds) {
            if (uint16_t(keyHead(number ^ seed) >> 48) != head)
                continue;
            std::string guess = KeyFromSeed(number ^ seed);
            bool all = true;
            for (const std::string& message : messages)
                all = all && Messages::Fits(guess, message);
            if (all) {
                serverSeed = seed;
                key = std::move(guess);
                return true;
            }
        }
        return false;
    }

    static bool usable(const std::vector<uint64_t>& serverSeeds, const std::vector<std::string>& messages) {
        if (serverSeeds.empty() || messages.empty())
            return false;
        for (const std::string& message : messages) {
            if (message.size() < 4)
                return false;
        }
        return true;
    }

    bool SearchNear(uint64_t time, int window, int first, int count, const std::vector<uint64_t>& serverSeeds,
        const std::vector<std::string>& messages, Found& out) {
        if (!usable(serverSeeds, messages) || first < 1)
            return false;
        uint16_t head = wantedHead(messages);
        // closest to time first
        for (int i = 0; i < window; i++) {
            int64_t offset = i % 2 == 0 ? i / 2 : -(i + 1) / 2;
            uint64_t seed = uint64_t(int64_t(time) + offset);
            CsRandom random{ int32_t(uint32_t(seed)) };
            for (int n = 1; n < first; n++)
                random.NextSafeUInt64();
            for (int n = first; n < first + count; n++) {
                uint64_t number = random.NextSafeUInt64();
                if (tryNumber(number, head, serverSeeds, messages, out.serverSeed, out.key)) {
                    out.timeSeed = seed;
                    out.number = n;
                    out.sample = 0;
                    return true;
                }
            }
        }
        return false;
    }

    // runs work(i) for every i from 0 to count - 1, spread over all cores but one,
    // until work says it found it or cancel is set. done goes up as they're tried
    static void spread(uint64_t count, const std::atomic<bool>& cancel, std::atomic<uint64_t>* done,
        const std::function<bool(uint64_t)>& work) {
        // added to done in steps, so the threads don't all wait on it
        constexpr uint64_t STEP = 4096;
        unsigned cores = std::thread::hardware_concurrency();
        unsigned threads = cores > 2 ? cores - 1 : 1u;
        std::atomic<bool> found = false;
        auto run = [&](unsigned firstIndex) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            uint64_t tried = 0;
            for (uint64_t i = firstIndex; i < count && !found && !cancel; i += threads) {
                if (work(i))
                    found = true;
                if (done && ++tried == STEP) {
                    *done += tried;
                    tried = 0;
                }
            }
            if (done)
                *done += tried;
        };
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < threads; i++)
            pool.emplace_back(run, i);
        for (std::thread& thread : pool)
            thread.join();
    }

    bool SearchBetween(uint64_t from, uint64_t to, int depth, const std::vector<uint64_t>& serverSeeds,
        const std::vector<std::string>& messages, const std::atomic<bool>& cancel, Found& out, std::atomic<uint64_t>* done) {
        if (!usable(serverSeeds, messages) || to < from || depth < 1)
            return false;
        uint16_t head = wantedHead(messages);
        std::mutex mutex;
        bool found = false;
        spread(to - from + 1, cancel, done, [&](uint64_t i) {
            uint64_t seed = from + i;
            CsRandom random{ int32_t(uint32_t(seed)) };
            for (int n = 1; n <= depth; n++) {
                uint64_t serverSeed;
                std::string key;
                if (!tryNumber(random.NextSafeUInt64(), head, serverSeeds, messages, serverSeed, key))
                    continue;
                std::lock_guard lock(mutex);
                if (!found) {
                    found = true;
                    out.timeSeed = seed;
                    out.number = n;
                    out.sample = 0;
                    out.serverSeed = serverSeed;
                    out.key = std::move(key);
                }
                return true;
            }
            return false;
        });
        return found;
    }

    bool SearchSamples(int32_t first, int32_t last, const std::vector<uint64_t>& serverSeeds,
        const std::vector<std::string>& messages, const std::atomic<bool>& cancel, Found& out, std::atomic<uint64_t>* done) {
        if (!usable(serverSeeds, messages) || last < first)
            return false;
        uint16_t head = wantedHead(messages);
        std::mutex mutex;
        bool found = false;
        spread(uint64_t(int64_t(last) - first + 1), cancel, done, [&](uint64_t i) {
            int32_t sample = int32_t(first + int64_t(i));
            uint64_t serverSeed;
            std::string key;
            if (!tryNumber(CsRandom::Number(sample), head, serverSeeds, messages, serverSeed, key))
                return false;
            std::lock_guard lock(mutex);
            if (!found) {
                found = true;
                out.timeSeed = 0;
                out.number = 0;
                out.sample = sample;
                out.serverSeed = serverSeed;
                out.key = std::move(key);
            }
            return true;
        });
        return found;
    }

    Rsa::Rsa(const std::vector<std::string>& pems) : m_wanted(pems.size()) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0)
            return;
        m_algorithm = algorithm;
        for (const std::string& pem : pems) {
            DWORD derSize = 0;
            if (!CryptStringToBinaryA(pem.data(), DWORD(pem.size()), CRYPT_STRING_BASE64HEADER, nullptr, &derSize, nullptr, nullptr))
                continue;
            std::vector<BYTE> der(derSize);
            if (!CryptStringToBinaryA(pem.data(), DWORD(pem.size()), CRYPT_STRING_BASE64HEADER, der.data(), &derSize, nullptr, nullptr))
                continue;
            BYTE* blob = nullptr;
            DWORD blobSize = 0;
            if (!CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, der.data(), derSize, CRYPT_DECODE_ALLOC_FLAG,
                nullptr, &blob, &blobSize))
                continue;
            BCRYPT_KEY_HANDLE key = nullptr;
            if (BCryptImportKeyPair(algorithm, nullptr, LEGACY_RSAPRIVATE_BLOB, &key, blob, blobSize, 0) == 0)
                m_keys.push_back(key);
            LocalFree(blob);
        }
    }

    Rsa::~Rsa() {
        for (void* key : m_keys)
            BCryptDestroyKey(BCRYPT_KEY_HANDLE(key));
        if (m_algorithm)
            BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE(m_algorithm), 0);
    }

    std::vector<uint64_t> Rsa::ServerSeeds(std::string_view body) const {
        std::vector<uint64_t> out;
        Messages::Field field;
        while (Messages::NextField(body, field)) {
            if (field.wire != 2)
                continue;
            std::string encrypted = fromBase64(field.bytes);
            if (encrypted.empty())
                continue;
            for (void* key : m_keys) {
                UCHAR plain[512];
                ULONG size = 0;
                if (BCryptDecrypt(BCRYPT_KEY_HANDLE(key), (PUCHAR)encrypted.data(), ULONG(encrypted.size()), nullptr, nullptr, 0,
                    plain, sizeof(plain), &size, BCRYPT_PAD_PKCS1) != 0 || size != 8)
                    continue;
                uint64_t seed = 0;
                for (int i = 0; i < 8; i++)
                    seed = seed << 8 | plain[i];
                out.push_back(seed);
            }
        }
        return out;
    }
}
