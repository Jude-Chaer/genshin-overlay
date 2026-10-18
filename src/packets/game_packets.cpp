#include "game_packets.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <deque>
#include <fstream>
#include <random>
#include <sstream>
#include <nlohmann/json.hpp>

namespace GamePackets {
    static constexpr uint16_t PORTS[] = { 22101, 22102 };
    // the C# Random seeds tried around the login's time, and numbers taken from each
    static constexpr int WINDOW = 3000;
    static constexpr int DEPTH = 5;
    static constexpr const char* COMPRESSED = "the login came compressed, close the game and start it again";

    static uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
    static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    static uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

    static std::string fromBase64(std::string_view text) {
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
        for (auto& [version, key] : keys.items()) {
            if (key.is_string())
                out.dispatch[uint16_t(std::stoi(version))] = fromBase64(key.get<std::string>());
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

    bool NextField(std::string_view& data, Field& out) {
        auto varint = [&](uint64_t& value) {
            value = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                if (data.empty())
                    return false;
                uint8_t b = uint8_t(data[0]);
                data.remove_prefix(1);
                value |= uint64_t(b & 0x7f) << shift;
                if (!(b & 0x80))
                    return true;
            }
            return false;
        };

        if (data.empty())
            return false;
        uint64_t tag;
        if (!varint(tag) || tag >> 3 == 0)
            return false;
        out.number = uint32_t(tag >> 3);
        out.wire = int(tag & 7);
        out.value = 0;
        out.bytes = {};
        switch (out.wire) {
        case 0:
            return varint(out.value);
        case 1:
        case 5: {
            size_t size = out.wire == 1 ? 8 : 4;
            if (data.size() < size)
                return false;
            for (size_t i = 0; i < size; i++)
                out.value |= uint64_t(uint8_t(data[i])) << (8 * i);
            data.remove_prefix(size);
            return true;
        }
        case 2: {
            uint64_t size;
            if (!varint(size) || size > data.size())
                return false;
            out.bytes = data.substr(0, size_t(size));
            data.remove_prefix(size_t(size));
            return true;
        }
        default:
            return false;
        }
    }

    // .NET Framework's System.Random, the game makes its client seed with it
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
        return uint64_t(NextDouble() * double(UINT64_MAX));
    }

    std::string KeyFromSeed(uint64_t seed) {
        std::mt19937_64 first(seed);
        std::mt19937_64 generator(first());
        generator.discard(1);
        std::string key(4096, '\0');
        for (size_t i = 0; i < 512; i++) {
            uint64_t value = generator();
            for (size_t b = 0; b < 8; b++)
                key[i * 8 + b] = char(value >> (56 - 8 * b));
        }
        return key;
    }

    static void decrypt(const std::string& key, std::string& data) {
        for (size_t i = 0; i < data.size(); i++)
            data[i] ^= key[i % key.size()];
    }

    // the first and last two bytes of every message are fixed
    static bool fits(const std::string& key, const std::string& data, bool checkTail = true) {
        size_t n = data.size();
        if (n < 4 || key.empty())
            return false;
        auto at = [&](size_t i) { return uint8_t(data[i] ^ key[i % key.size()]); };
        if (at(0) != 0x45 || at(1) != 0x67)
            return false;
        return !checkTail || (at(n - 2) == 0x89 && at(n - 1) == 0xAB);
    }

    bool SearchKey(uint64_t sentMs, uint64_t serverSeed, const std::string& data, int window, int depth,
        uint64_t& timeSeed, std::string& key) {
        for (int i = 0; i < window; i++) {
            int64_t offset = i % 2 == 0 ? i / 2 : -(i - 1) / 2;
            int64_t time = int64_t(sentMs) + offset;
            CsRandom random{ int32_t(uint32_t(uint64_t(time))) };
            for (int d = 0; d < depth; d++) {
                std::string guess = KeyFromSeed(random.NextSafeUInt64() ^ serverSeed);
                if (fits(guess, data)) {
                    timeSeed = uint64_t(time);
                    key = std::move(guess);
                    return true;
                }
            }
        }
        return false;
    }

    // the game's server seed comes RSA encrypted in the login answer
    struct Sniffer::Rsa {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        std::vector<BCRYPT_KEY_HANDLE> keys;

        explicit Rsa(const std::vector<std::string>& pems) {
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0)
                return;
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
                    keys.push_back(key);
                LocalFree(blob);
            }
        }

        ~Rsa() {
            for (BCRYPT_KEY_HANDLE key : keys)
                BCryptDestroyKey(key);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
        }

        // every 8 byte answer any key gives for any bytes field
        std::vector<uint64_t> seeds(std::string_view body) const {
            std::vector<uint64_t> out;
            Field field;
            while (NextField(body, field)) {
                if (field.wire != 2)
                    continue;
                std::string encrypted = fromBase64(field.bytes);
                if (encrypted.empty())
                    continue;
                for (BCRYPT_KEY_HANDLE key : keys) {
                    UCHAR plain[512];
                    ULONG size = 0;
                    if (BCryptDecrypt(key, (PUCHAR)encrypted.data(), ULONG(encrypted.size()), nullptr, nullptr, 0,
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
    };

    // the game's KCP has 8 extra bytes in each segment header, read only, nothing is acked
    struct Sniffer::Kcp {
        uint32_t conv = 0;
        uint32_t next = 0;
        std::map<uint32_t, std::pair<uint8_t, std::string>> waiting;
        std::deque<std::pair<uint8_t, std::string>> ready;

        static constexpr uint32_t WINDOW = 1024;
        static constexpr size_t OVERHEAD = 24;

        static bool before(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }

        void feed(const uint8_t* data, size_t size, std::vector<std::string>& messages) {
            if (size <= OVERHEAD || le32(data) != conv)
                return;
            for (size_t i = 0; i + 32 <= size;) {
                if (le32(data + i) != conv)
                    break;
                uint8_t cmd = data[i + 8];
                uint8_t frg = data[i + 9];
                uint32_t sn = le32(data + i + 16);
                uint32_t length = le32(data + i + 24);
                if (length > size - i - 32)
                    break;
                const char* content = (const char*)data + i + 32;
                i += 32 + size_t(length);
                if (cmd < 81 || cmd > 84)
                    break;
                if (cmd != 81 || !before(sn, next + WINDOW) || before(sn, next))
                    continue;
                waiting.emplace(sn, std::make_pair(frg, std::string(content, length)));
            }

            move();
            while (!ready.empty()) {
                size_t parts = size_t(ready.front().first) + 1;
                if (ready.size() < parts)
                    break;
                std::string message;
                for (;;) {
                    auto [frg, part] = std::move(ready.front());
                    ready.pop_front();
                    message += part;
                    if (frg == 0)
                        break;
                }
                messages.push_back(std::move(message));
                move();
            }
        }

        void move() {
            while (!waiting.empty() && waiting.begin()->first == next && ready.size() < WINDOW) {
                ready.push_back(std::move(waiting.begin()->second));
                waiting.erase(waiting.begin());
                next++;
            }
        }
    };

    Sniffer::Sniffer(const Keys& keys) : m_dispatchKeys(keys.dispatch), m_rsa(std::make_unique<Rsa>(keys.rsaPem)) {}

    Sniffer::~Sniffer() = default;

    Event Sniffer::Feed(const uint8_t* frame, size_t size, std::vector<Command>& out) {
        // ethernet, maybe vlan tags, then ip and udp
        if (size < 14)
            return Event::None;
        size_t at = 12;
        uint16_t type = be16(frame + at);
        while ((type == 0x8100 || type == 0x88a8) && at + 6 <= size) {
            at += 4;
            type = be16(frame + at);
        }
        at += 2;
        const uint8_t* ip = frame + at;
        size_t left = size - at;
        const uint8_t* udp = nullptr;
        if (type == 0x0800) {
            if (left < 20 || ip[9] != 17 || (be16(ip + 6) & 0x3fff) != 0)
                return Event::None;
            size_t headerSize = size_t(ip[0] & 15) * 4;
            size_t total = be16(ip + 2);
            if (headerSize < 20 || total < headerSize || total > left)
                return Event::None;
            udp = ip + headerSize;
            left = total - headerSize;
        }
        else if (type == 0x86dd) {
            if (left < 40 || ip[6] != 17)
                return Event::None;
            udp = ip + 40;
            left -= 40;
        }
        else
            return Event::None;
        if (left < 8)
            return Event::None;
        size_t udpSize = be16(udp + 4);
        if (udpSize < 8 || udpSize > left)
            return Event::None;

        uint16_t source = be16(udp), destination = be16(udp + 2);
        Direction direction;
        if (source == PORTS[0] || source == PORTS[1])
            direction = Direction::Received;
        else if (destination == PORTS[0] || destination == PORTS[1])
            direction = Direction::Sent;
        else
            return Event::None;

        const uint8_t* payload = udp + 8;
        size_t payloadSize = udpSize - 8;
        if (payloadSize <= 20) {
            if (payloadSize < 4)
                return Event::None;
            uint32_t code = be32(payload);
            if (code == 0xFF) {
                m_sentKcp.reset();
                m_recvKcp.reset();
                m_key = KeyKind::None;
                return Event::Handshake;
            }
            if (code == 404)
                return Event::Disconnected;
            return Event::None;
        }

        receive(direction, payload, payloadSize, out);
        return Event::None;
    }

    void Sniffer::receive(Direction direction, const uint8_t* data, size_t size, std::vector<Command>& out) {
        std::unique_ptr<Kcp>& kcp = direction == Direction::Sent ? m_sentKcp : m_recvKcp;
        if (!kcp) {
            if (size <= Kcp::OVERHEAD)
                return;
            kcp = std::make_unique<Kcp>();
            kcp->conv = le32(data);
        }
        std::vector<std::string> messages;
        kcp->feed(data, size, messages);
        for (std::string& message : messages) {
            Command command;
            if (decode(message, direction, command))
                out.push_back(std::move(command));
        }
    }

    bool Sniffer::decode(std::string& data, Direction direction, Command& out) {
        if (data.size() < 4)
            return false;

        if (m_key == KeyKind::None) {
            uint16_t version = be16((const uint8_t*)data.data()) ^ 0x4567;
            auto found = m_dispatchKeys.find(version);
            if (found == m_dispatchKeys.end()) {
                m_problem = "no login key for game version " + std::to_string(version);
                return false;
            }
            m_key = KeyKind::Dispatch;
            m_keyBytes = found->second;
        }
        else if (m_key == KeyKind::Dispatch && !fits(m_keyBytes, data)) {
            // after the login answer the game switches to the session key
            bool found = false;
            std::vector<uint64_t> times;
            if (m_clientSeed) times.push_back(*m_clientSeed);
            times.insert(times.end(), m_loginTimes.begin(), m_loginTimes.end());
            times.push_back(m_sentMs);
            for (uint64_t serverSeed : m_serverSeeds) {
                uint64_t timeSeed = 0;
                std::string key;
                bool fit = false;
                for (size_t i = 0; i < times.size() && !fit; i++)
                    fit = SearchKey(times[i], serverSeed, data, WINDOW, DEPTH, timeSeed, key);
                if (fit) {
                    m_clientSeed = timeSeed;
                    m_key = KeyKind::Session;
                    m_keyBytes = std::move(key);
                    found = true;
                    break;
                }
            }
            if (!found) {
                m_problem = m_compressedLogin ? COMPRESSED : "couldn't find the session key";
                return false;
            }
        }
        else if (m_key == KeyKind::Session && !fits(m_keyBytes, data, false)) {
            m_key = KeyKind::None;
            m_problem = "the session key stopped fitting";
            return false;
        }

        decrypt(m_keyBytes, data);
        const uint8_t* p = (const uint8_t*)data.data();
        size_t n = data.size();
        if (n < 12 || p[0] != 0x45 || p[1] != 0x67 || p[n - 2] != 0x89 || p[n - 1] != 0xAB)
            return false;
        size_t headSize = be16(p + 4);
        size_t bodySize = be32(p + 6);
        if (10 + headSize + bodySize > n)
            return false;
        out.id = be16(p + 2);
        out.direction = direction;
        out.head = data.substr(10, headSize);
        out.body = data.substr(10 + headSize, bodySize);
        m_problem.clear();

        if (m_key == KeyKind::Dispatch && direction == Direction::Received) {
            uint64_t sentMs = 0, unpackedSize = 0;
            std::string_view head = out.head;
            Field field;
            while (NextField(head, field)) {
                if (field.number == 6 && field.wire == 0)
                    sentMs = field.value;
                if (field.number == 8 && field.wire == 0)
                    unpackedSize = field.value;
            }
            if (unpackedSize) {
                // Since 7.1 the login answer sometimes comes Oodle compressed, and
                // Oodle can't be read here. The game's key generator was still
                // seeded at this login, so after a relog the key is found from it.
                m_compressedLogin = true;
                m_serverSeeds.clear();
                if (sentMs && !m_clientSeed) m_loginTimes.push_back(sentMs);
                m_problem = COMPRESSED;
                return true;
            }
            std::vector<uint64_t> seeds = m_rsa->seeds(out.body);
            if (!seeds.empty()) {
                m_compressedLogin = false;
                m_serverSeeds = std::move(seeds);
                m_sentMs = sentMs;
            }
        }
        return true;
    }
}
