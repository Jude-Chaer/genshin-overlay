#include "messages.hpp"

namespace Messages {
    // 45 67, id, head size, body size, then head, body and 89 AB
    static constexpr size_t FRAMING = 12;

    static uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
    static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

    uint16_t KeyId(std::string_view data) {
        if (data.size() < 2)
            return 0;
        return uint16_t(be16((const uint8_t*)data.data()) ^ 0x4567);
    }

    bool Fits(const std::string& key, std::string_view data, bool checkTail) {
        size_t n = data.size();
        if (n < 4 || key.empty())
            return false;
        auto at = [&](size_t i) { return uint8_t(data[i] ^ key[i % key.size()]); };
        if (at(0) != 0x45 || at(1) != 0x67)
            return false;
        return !checkTail || (at(n - 2) == 0x89 && at(n - 1) == 0xAB);
    }

    bool Open(const std::string& key, std::string_view data, Frames::Direction direction, Message& out) {
        if (data.size() < FRAMING || !Fits(key, data))
            return false;
        std::string plain(data);
        for (size_t i = 0; i < plain.size(); i++)
            plain[i] ^= key[i % key.size()];
        const uint8_t* p = (const uint8_t*)plain.data();
        size_t headSize = be16(p + 4);
        size_t bodySize = be32(p + 6);
        if (FRAMING + headSize + bodySize != plain.size())
            return false;
        out.id = be16(p + 2);
        out.direction = direction;
        out.head = plain.substr(10, headSize);
        out.body = plain.substr(10 + headSize, bodySize);
        return true;
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
}
