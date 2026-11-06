#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "frames.hpp"

// Decrypts the game's messages and splits them into id, head and body. What an
// id means changes every game version, so nothing here reads one.

namespace Messages {
    struct Message {
        uint16_t id = 0;
        Frames::Direction direction = Frames::Direction::Received;
        // PacketHead and the message itself, both protobuf
        std::string head;
        std::string body;
    };

    // the dispatch key a login message was encrypted with
    extern uint16_t KeyId(std::string_view data);
    // every message starts with 45 67 and ends with 89 AB once decrypted
    extern bool Fits(const std::string& key, std::string_view data, bool checkTail = true);
    // false when the key doesn't fit or the sizes inside don't add up
    extern bool Open(const std::string& key, std::string_view data, Frames::Direction direction, Message& out);

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
}
