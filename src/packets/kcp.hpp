#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "frames.hpp"

// Puts the game's KCP segments back together into whole messages, both
// directions. Read only, nothing is acked.

namespace Kcp {
    // the game's segment header has 8 bytes more than plain KCP's
    inline constexpr size_t HEADER = 32;

    struct Message {
        Frames::Direction direction = Frames::Direction::Received;
        std::string data;
    };

    class Reader {
    public:
        // a handshake was seen, the next connection starts at sn 0
        void Reset();
        // one udp payload caught at ms, the messages it finishes go to out
        void Feed(Frames::Direction direction, const uint8_t* data, size_t size, uint64_t ms, std::vector<Message>& out);
        // a segment went missing for good or a message's parts didn't add up,
        // nothing more comes out until the next Reset
        bool Broken() const { return m_broken; }
        // a different conv came without a handshake
        bool NewConnection() const { return m_newConnection; }

    private:
        struct Stream {
            bool started = false;
            uint32_t next = 0;
            std::map<uint32_t, std::pair<uint8_t, std::string>> waiting;
            std::deque<std::pair<uint8_t, std::string>> ready;
            // the other side has acked up to here
            uint32_t acked = 0;
            uint64_t behindSince = 0;
        };

        void push(Stream& stream, uint32_t sn, uint8_t frg, std::string data);
        void take(Stream& stream, Frames::Direction direction, std::vector<Message>& out);
        void check(Stream& stream, uint64_t ms);

        bool m_fromStart = false;
        bool m_hasConv = false;
        uint32_t m_conv = 0;
        Stream m_sent, m_received;
        bool m_broken = false;
        bool m_newConnection = false;
    };
}
