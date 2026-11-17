#include "kcp.hpp"

namespace Kcp {
    // The game's receive window is 256 segments, so a sender is never further
    // ahead than that. A segment this far past the one missing means it was missed.
    static constexpr uint32_t WINDOW = 1024;
    // copies from two adapters can come a little out of order
    static constexpr uint64_t GRACE_MS = 1000;
    static constexpr uint8_t PUSH = 81;

    static uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
    static bool before(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }

    void Reader::Reset() {
        m_fromStart = true;
        m_hasConv = false;
        m_sent = {};
        m_received = {};
        m_broken = false;
        m_newConnection = false;
    }

    void Reader::Feed(Frames::Direction direction, const uint8_t* data, size_t size, uint64_t ms, std::vector<Message>& out) {
        m_newConnection = false;
        if (size < HEADER)
            return;
        uint32_t conv = le32(data);
        // a new conv only counts once its first segment looks like the game's
        uint8_t firstCmd = data[8];
        if (firstCmd < 81 || firstCmd > 84 || le32(data + 24) > size - HEADER)
            return;
        // after a handshake the old connection's last packets can still come,
        // the new one is the conv whose numbers start near 0
        if (!m_hasConv && m_fromStart && !(le32(data + 16) < WINDOW && le32(data + 20) < WINDOW))
            return;
        if (!m_hasConv) {
            m_hasConv = true;
            m_conv = conv;
        }
        else if (conv != m_conv) {
            // the handshake was missed, but this is a new connection all the same
            Reset();
            m_fromStart = false;
            m_hasConv = true;
            m_conv = conv;
            m_newConnection = true;
        }
        if (m_broken)
            return;

        Stream& stream = direction == Frames::Direction::Sent ? m_sent : m_received;
        Stream& other = direction == Frames::Direction::Sent ? m_received : m_sent;
        for (size_t i = 0; i + HEADER <= size;) {
            const uint8_t* segment = data + i;
            if (le32(segment) != conv)
                break;
            uint8_t cmd = segment[8];
            uint8_t frg = segment[9];
            uint32_t sn = le32(segment + 16);
            uint32_t una = le32(segment + 20);
            uint32_t length = le32(segment + 24);
            if (cmd < 81 || cmd > 84 || length > size - i - HEADER)
                break;
            i += HEADER + size_t(length);

            // every segment says how far it got in the other direction
            if (other.started && before(other.acked, una))
                other.acked = una;
            if (cmd == PUSH)
                push(stream, sn, frg, std::string((const char*)segment + HEADER, length));
        }

        check(m_sent, ms);
        check(m_received, ms);
        if (!m_broken)
            take(stream, direction, out);
    }

    void Reader::push(Stream& stream, uint32_t sn, uint8_t frg, std::string data) {
        if (!stream.started) {
            stream.started = true;
            stream.next = m_fromStart ? 0 : sn;
            stream.acked = stream.next;
        }
        if (before(sn, stream.next))
            return;
        if (!before(sn, stream.next + WINDOW)) {
            m_broken = true;
            return;
        }
        stream.waiting.emplace(sn, std::make_pair(frg, std::move(data)));
    }

    void Reader::check(Stream& stream, uint64_t ms) {
        if (!stream.started || !before(stream.next, stream.acked) || stream.waiting.count(stream.next)) {
            stream.behindSince = 0;
            return;
        }
        // the other side has the segment, so it isn't sent again
        if (!stream.behindSince)
            stream.behindSince = ms ? ms : 1;
        else if (ms >= stream.behindSince + GRACE_MS)
            m_broken = true;
    }

    void Reader::take(Stream& stream, Frames::Direction direction, std::vector<Message>& out) {
        for (;;) {
            while (!stream.waiting.empty() && stream.waiting.begin()->first == stream.next) {
                stream.ready.push_back(std::move(stream.waiting.begin()->second));
                stream.waiting.erase(stream.waiting.begin());
                stream.next++;
            }
            // frg counts down to 0 on the message's last segment
            if (stream.ready.empty())
                return;
            size_t count = size_t(stream.ready.front().first) + 1;
            if (stream.ready.size() < count)
                return;
            for (size_t k = 0; k < count; k++) {
                if (stream.ready[k].first != count - 1 - k) {
                    m_broken = true;
                    return;
                }
            }
            Message message;
            message.direction = direction;
            for (size_t k = 0; k < count; k++) {
                message.data += stream.ready.front().second;
                stream.ready.pop_front();
            }
            out.push_back(std::move(message));
        }
    }
}
