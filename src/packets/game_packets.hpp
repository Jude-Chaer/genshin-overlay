#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Reads the game's traffic from captured network frames, never sends anything.
// Frames go in, the game's messages come out decrypted. Based on auto-artifactarium
// (MIT), see packets/NOTICE.txt.

namespace GamePackets {
    enum class Direction { Sent, Received };

    struct Command {
        uint16_t id = 0;
        Direction direction = Direction::Received;
        // PacketHead and the message itself, both protobuf
        std::string head;
        std::string body;
    };

    enum class Event { None, Handshake, Disconnected };

    struct Keys {
        // the login key for each game version
        std::map<uint16_t, std::string> dispatch;
        std::vector<std::string> rsaPem;
    };

    // dispatch-keys.json and rsa-*.pem from the folder
    extern bool LoadKeys(const std::filesystem::path& folder, Keys& out);

    // a protobuf message read field by field, without knowing its fields
    struct Field {
        uint32_t number = 0;
        // 0 varint, 1 fixed 64, 2 bytes, 5 fixed 32
        int wire = 0;
        uint64_t value = 0;
        std::string_view bytes;
    };
    // false at the end or when the rest doesn't read as protobuf
    extern bool NextField(std::string_view& data, Field& out);

    class Sniffer {
    public:
        explicit Sniffer(const Keys& keys);
        ~Sniffer();

        // one ethernet frame caught at ms (unix, 0 for now), the messages it
        // finishes get added to out
        Event Feed(const uint8_t* frame, size_t size, std::vector<Command>& out, uint64_t ms = 0);
        // waits for the long search, for replays
        void FinishSearch(std::vector<Command>& out);

        // why nothing decodes right now, empty while it works
        const std::string& Problem() const { return m_problem; }
        bool HasKey() const { return m_key == KeyKind::Dispatch || m_key == KeyKind::Session; }
        // past the login, the game's messages read with this session's key
        bool HasSessionKey() const { return m_key == KeyKind::Session; }
        // when the game was started, unix ms. With it a missed first login can
        // still be found, by looking at every time since then
        void SetGameStart(uint64_t unixMs) { m_gameStart = unixMs; }
        // the long search runs, messages wait for it
        bool Searching() const { return m_key == KeyKind::Searching; }

    private:
        enum class KeyKind { None, Dispatch, Session, Searching, Lost };
        struct Kcp;
        struct Rsa;
        struct Search;

        void receive(Direction direction, const uint8_t* data, size_t size, std::vector<Command>& out);
        bool decode(std::string& data, Direction direction, Command& out);
        bool startSearch(const std::string& data);
        // false while the long search still runs
        bool searchDone(std::vector<Command>& out);

        std::map<uint16_t, std::string> m_dispatchKeys;
        std::unique_ptr<Rsa> m_rsa;
        std::unique_ptr<Kcp> m_sentKcp, m_recvKcp;
        KeyKind m_key = KeyKind::None;
        std::string m_keyBytes;
        std::optional<uint64_t> m_clientSeed;
        std::vector<uint64_t> m_serverSeeds;
        // logins whose answer couldn't be read, the generator may have been seeded at one
        std::vector<uint64_t> m_loginTimes;
        bool m_compressedLogin = false;
        uint64_t m_sentMs = 0;
        // the game's clock in the headers minus ours
        int64_t m_clockOffset = 0;
        uint64_t m_gameStart = 0;
        uint64_t m_frameMs = 0;
        std::unique_ptr<Search> m_search;
        std::vector<std::pair<Direction, std::string>> m_waiting;
        std::string m_problem;
    };

    // the session key search, public for the tests
    extern std::string KeyFromSeed(uint64_t seed);
    // tries the C# Random seeded with every time around sentMs, depth numbers each
    extern bool SearchKey(uint64_t sentMs, uint64_t serverSeed, const std::string& data, int window, int depth,
        uint64_t& timeSeed, std::string& key);
    // every time from from to to on all cores, for when the login was missed
    extern bool SearchKeyBetween(uint64_t from, uint64_t to, const std::vector<uint64_t>& serverSeeds, const std::string& data,
        int depth, const std::atomic<bool>& cancel, uint64_t& timeSeed, std::string& key);

    class CsRandom {
    public:
        explicit CsRandom(int32_t seed);
        double NextDouble();
        uint64_t NextSafeUInt64();

    private:
        int32_t sample();
        int32_t m_seeds[56] = {};
        int m_next = 0, m_nextP = 21;
    };
}
