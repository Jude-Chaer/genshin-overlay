#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Picks the game's UDP packets out of captured frames. Npcap gives ethernet
// frames, Windows' raw sockets give ip packets.

namespace Frames {
    enum class Start { Ethernet, Ip };
    enum class Direction { Sent, Received };
    // Handshake is the game connecting, Disconnect the server closing it
    enum class Kind { None, Data, Handshake, Disconnect };

    // an ip address (4 bytes for ipv4, 16 for ipv6) and a udp port
    struct Peer {
        uint8_t address[16] = {};
        uint8_t size = 0;
        uint16_t port = 0;

        bool operator==(const Peer& other) const {
            return size == other.size && port == other.port && std::memcmp(address, other.address, size) == 0;
        }
        bool operator!=(const Peer& other) const { return !(*this == other); }
    };

    struct Packet {
        Kind kind = Kind::None;
        Direction direction = Direction::Received;
        // the game server's side: where it came from or where it went
        Peer server;
        // the udp payload, points into the frame
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    extern Packet Read(const uint8_t* frame, size_t size, Start start);
}
