#pragma once
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

        // one ethernet frame, the messages it finishes get added to out
        Event Feed(const uint8_t* frame, size_t size, std::vector<Command>& out);

        // why nothing decodes right now, empty while it works
        const std::string& Problem() const { return m_problem; }
        bool HasKey() const { return m_key != KeyKind::None; }

    private:
        enum class KeyKind { None, Dispatch, Session };
        struct Kcp;
        struct Rsa;

        void receive(Direction direction, const uint8_t* data, size_t size, std::vector<Command>& out);
        bool decode(std::string& data, Direction direction, Command& out);

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
        std::string m_problem;
    };

    // the session key search, public for the tests
    extern std::string KeyFromSeed(uint64_t seed);
    // tries the C# Random seeded with every time around sentMs, depth numbers each
    extern bool SearchKey(uint64_t sentMs, uint64_t serverSeed, const std::string& data, int window, int depth,
        uint64_t& timeSeed, std::string& key);

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
