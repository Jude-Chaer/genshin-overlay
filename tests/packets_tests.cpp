#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/frames.hpp"

// tests/data/traffic is a cut of a real 7.2 session, see its answers.md. The
// whole recordings with their logins are only on the PC they were made on, the
// tests that need them are skipped without them.

namespace {
    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data" / "traffic";
    struct Frame {
        uint64_t ms = 0;
        std::vector<uint8_t> data;
    };

    // the packet recorder's frames.bin: u64 ms, u8 adapter, u32 size, the frame
    std::vector<Frame> readFrames(const std::filesystem::path& path) {
        std::vector<Frame> frames;
        std::ifstream file(path, std::ios::binary);
        uint8_t record[13];
        while (file.read((char*)record, sizeof(record))) {
            Frame frame;
            for (int b = 0; b < 8; b++)
                frame.ms |= uint64_t(record[b]) << (8 * b);
            uint32_t size = 0;
            for (int b = 0; b < 4; b++)
                size |= uint32_t(record[9 + b]) << (8 * b);
            frame.data.resize(size);
            if (!file.read((char*)frame.data.data(), size))
                break;
            frames.push_back(std::move(frame));
        }
        return frames;
    }

}

TEST_CASE("Every test data frame is the game server's udp") {
    std::vector<Frame> frames = readFrames(DATA / "frames.bin");
    REQUIRE(frames.size() == 1600);
    for (const Frame& frame : frames) {
        Frames::Packet packet = Frames::Read(frame.data.data(), frame.data.size(), Frames::Start::Ethernet);
        CHECK(packet.kind == Frames::Kind::Data);
        CHECK(packet.direction == Frames::Direction::Received);
    }
    // the same frame from its ip header on
    const Frame& frame = frames[0];
    Frames::Packet ip = Frames::Read(frame.data.data() + 14, frame.data.size() - 14, Frames::Start::Ip);
    Frames::Packet ethernet = Frames::Read(frame.data.data(), frame.data.size(), Frames::Start::Ethernet);
    CHECK(ip.kind == Frames::Kind::Data);
    CHECK(ip.size == ethernet.size);
    CHECK(ip.data == ethernet.data);
}
