#include "frames.hpp"

namespace Frames {
    static constexpr uint16_t PORTS[] = { 22101, 22102 };
    // the connect and disconnect packets are 20 bytes, data always has a longer kcp header
    static constexpr size_t CONTROL_SIZE = 20;
    static constexpr uint32_t CONNECT = 0xFF;
    static constexpr uint32_t DISCONNECT = 404;

    static uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
    static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

    static bool gamePort(uint16_t port) { return port == PORTS[0] || port == PORTS[1]; }

    Packet Read(const uint8_t* frame, size_t size, Start start) {
        Packet out;
        const uint8_t* ip = frame;
        size_t left = size;
        if (start == Start::Ethernet) {
            if (size < 14)
                return out;
            // vlan tags sit between the addresses and the type
            size_t at = 12;
            uint16_t type = be16(frame + at);
            while ((type == 0x8100 || type == 0x88a8) && at + 6 <= size) {
                at += 4;
                type = be16(frame + at);
            }
            if (type != 0x0800 && type != 0x86dd)
                return out;
            ip = frame + at + 2;
            left = size - at - 2;
        }
        if (left < 1)
            return out;

        const uint8_t* udp = nullptr;
        const uint8_t* source = nullptr;
        const uint8_t* destination = nullptr;
        uint8_t addressSize = 0;
        int version = ip[0] >> 4;
        if (version == 4) {
            // fragments can't be read on their own, the game's packets fit in one
            if (left < 20 || ip[9] != 17 || (be16(ip + 6) & 0x3fff) != 0)
                return out;
            size_t headerSize = size_t(ip[0] & 15) * 4;
            size_t total = be16(ip + 2);
            if (headerSize < 20 || total < headerSize || total > left)
                return out;
            udp = ip + headerSize;
            left = total - headerSize;
            source = ip + 12;
            destination = ip + 16;
            addressSize = 4;
        }
        else if (version == 6) {
            if (left < 40 || ip[6] != 17)
                return out;
            udp = ip + 40;
            left -= 40;
            source = ip + 8;
            destination = ip + 24;
            addressSize = 16;
        }
        else
            return out;

        if (left < 8)
            return out;
        size_t udpSize = be16(udp + 4);
        if (udpSize < 8 || udpSize > left)
            return out;
        uint16_t sourcePort = be16(udp), destinationPort = be16(udp + 2);
        if (gamePort(sourcePort)) {
            out.direction = Direction::Received;
            std::memcpy(out.server.address, source, addressSize);
            out.server.port = sourcePort;
        }
        else if (gamePort(destinationPort)) {
            out.direction = Direction::Sent;
            std::memcpy(out.server.address, destination, addressSize);
            out.server.port = destinationPort;
        }
        else
            return out;
        out.server.size = addressSize;

        out.data = udp + 8;
        out.size = udpSize - 8;
        if (out.size == CONTROL_SIZE) {
            uint32_t code = be32(out.data);
            // only the game sends the connect, one coming in isn't the game's
            if (code == CONNECT && out.direction == Direction::Sent)
                out.kind = Kind::Handshake;
            else if (code == DISCONNECT)
                out.kind = Kind::Disconnect;
            return out;
        }
        if (out.size > CONTROL_SIZE)
            out.kind = Kind::Data;
        return out;
    }
}
