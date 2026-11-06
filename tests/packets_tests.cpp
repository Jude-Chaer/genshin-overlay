#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/frames.hpp"
#include "packets/kcp.hpp"
#include "packets/messages.hpp"
#include "packets/session_key.hpp"

// tests/data/traffic is a cut of a real 7.2 session, see its answers.md. The
// whole recordings with their logins are only on the PC they were made on, the
// tests that need them are skipped without them.

namespace {
    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data" / "traffic";
    const std::filesystem::path KEYS = std::filesystem::path(__FILE__).parent_path().parent_path() / "packets";
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

    SessionKey::Keys keys() {
        SessionKey::Keys out;
        REQUIRE(SessionKey::LoadKeys(KEYS, out));
        return out;
    }

    // the answer in answers.md: the first number of time seed 1793968363276
    const uint64_t TEST_TIME_SEED = 1793968363276, TEST_SERVER_SEED = 10378004310333966569ull;
    const int32_t TEST_SAMPLE = 1385065457;

    std::string testKey() {
        SessionKey::CsRandom random{ int32_t(uint32_t(TEST_TIME_SEED)) };
        return SessionKey::KeyFromSeed(random.NextSafeUInt64() ^ TEST_SERVER_SEED);
    }

    std::vector<Kcp::Message> kcpMessages(const std::vector<Frame>& frames, bool handshakeFirst = false, bool* broken = nullptr) {
        Kcp::Reader kcp;
        if (handshakeFirst)
            kcp.Reset();
        std::vector<Kcp::Message> out;
        for (const Frame& frame : frames) {
            Frames::Packet packet = Frames::Read(frame.data.data(), frame.data.size(), Frames::Start::Ethernet);
            if (packet.kind == Frames::Kind::Data)
                kcp.Feed(packet.direction, packet.data, packet.size, frame.ms, out);
        }
        if (broken)
            *broken = kcp.Broken();
        return out;
    }

    std::vector<std::string> firstData(const std::vector<Kcp::Message>& messages, size_t count) {
        std::vector<std::string> out;
        for (size_t i = 0; i < messages.size() && i < count; i++)
            out.push_back(messages[i].data);
        return out;
    }

}

TEST_CASE("CsRandom gives what .NET Framework's Random gives") {
    struct Case { int32_t seed; double first, second; };
    // from [System.Random]::new(seed).NextDouble() in Windows PowerShell
    Case cases[] = {
        { 0, 0.72624326996795985, 0.81732535959096875 },
        { 12345, 0.066746934813795109, 0.070159508879370752 },
        { -7, 0.38322046929189024, 0.87125568272138743 },
        { 1792258392, 0.2749534120200916, 0.56214887674997971 },
        { INT32_MIN, 0.72624326996795985, 0.81732535959096875 },
    };
    for (const Case& c : cases) {
        SessionKey::CsRandom random(c.seed);
        CHECK(random.NextDouble() == c.first);
        CHECK(random.NextDouble() == c.second);
    }
}

TEST_CASE("NextField reads protobuf without knowing the message") {
    // a known format, written by hand: 1: 150, 2: "hi", 3: fixed32 1
    std::string data("\x08\x96\x01\x12\x02hi\x1d\x01\x00\x00\x00", 12);
    std::string_view rest = data;
    Messages::Field field;
    REQUIRE(Messages::NextField(rest, field));
    CHECK((field.number == 1 && field.wire == 0 && field.value == 150));
    REQUIRE(Messages::NextField(rest, field));
    CHECK((field.number == 2 && field.bytes == "hi"));
    REQUIRE(Messages::NextField(rest, field));
    CHECK((field.number == 3 && field.wire == 5 && field.value == 1));
    CHECK_FALSE(Messages::NextField(rest, field));

    // a length past the end
    std::string broken = "\x12\x09hi";
    rest = broken;
    CHECK_FALSE(Messages::NextField(rest, field));
}

TEST_CASE("The keys next to the overlay load") {
    SessionKey::Keys loaded = keys();
    CHECK(loaded.dispatch.size() >= 10);
    CHECK(loaded.rsaPem.size() == 2);
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

TEST_CASE("KCP starts at the first sn it sees when there was no handshake") {
    std::vector<Frame> frames = readFrames(DATA / "frames.bin");
    CHECK(kcpMessages(frames).size() == 685);
    // after a handshake it waits for sn 0, which this data starts long after
    CHECK(kcpMessages(frames, true).empty());
}

TEST_CASE("Every test data message opens with the session key and has its id") {
    std::vector<Kcp::Message> messages = kcpMessages(readFrames(DATA / "frames.bin"));
    std::ifstream file(DATA / "ids.txt");
    std::vector<uint16_t> ids;
    for (int id; file >> id;)
        ids.push_back(uint16_t(id));
    REQUIRE(ids.size() == 685);
    REQUIRE(messages.size() == ids.size());

    std::string key = testKey();
    for (size_t i = 0; i < messages.size(); i++) {
        Messages::Message opened;
        INFO("message " << i);
        REQUIRE(Messages::Open(key, messages[i].data, messages[i].direction, opened));
        CHECK(opened.id == ids[i]);
    }
    Messages::Message opened;
    CHECK_FALSE(Messages::Open(SessionKey::KeyFromSeed(1), messages[0].data, messages[0].direction, opened));
}

TEST_CASE("A frame missed in the middle stops KCP for good") {
    std::vector<Frame> frames = readFrames(DATA / "frames.bin");
    frames.erase(frames.begin() + 100);
    bool broken = false;
    std::vector<Kcp::Message> messages = kcpMessages(frames, false, &broken);
    CHECK(broken);
    CHECK(messages.size() < 685);
}

TEST_CASE("The first search pass finds the test data's key from the first message") {
    std::vector<std::string> first = firstData(kcpMessages(readFrames(DATA / "frames.bin")), 1);
    std::atomic<bool> cancel = false;
    SessionKey::Found found;
    REQUIRE(SessionKey::SearchBetween(TEST_TIME_SEED - 60000, TEST_TIME_SEED + 60000, 5, { 1, TEST_SERVER_SEED }, first, cancel, found));
    CHECK(found.timeSeed == TEST_TIME_SEED);
    CHECK(found.number == 1);
    CHECK(found.serverSeed == TEST_SERVER_SEED);
    CHECK(found.key == testKey());
    CHECK_FALSE(SessionKey::SearchBetween(TEST_TIME_SEED - 60000, TEST_TIME_SEED + 60000, 5, { TEST_SERVER_SEED + 1 }, first,
        cancel, found));
    CHECK_FALSE(SessionKey::SearchBetween(TEST_TIME_SEED + 1, TEST_TIME_SEED + 60000, 5, { TEST_SERVER_SEED }, first, cancel, found));
    cancel = true;
    CHECK_FALSE(SessionKey::SearchBetween(TEST_TIME_SEED - 60000, TEST_TIME_SEED + 60000, 5, { TEST_SERVER_SEED }, first, cancel, found));
}

TEST_CASE("The quick search only tries the numbers it's given") {
    std::vector<std::string> check = firstData(kcpMessages(readFrames(DATA / "frames.bin")), 4);
    SessionKey::Found found;
    REQUIRE(SessionKey::SearchNear(TEST_TIME_SEED + 40, 3000, 1, 5, { TEST_SERVER_SEED }, check, found));
    CHECK(found.timeSeed == TEST_TIME_SEED);
    CHECK(found.number == 1);
    CHECK_FALSE(SessionKey::SearchNear(TEST_TIME_SEED + 40, 3000, 2, 5, { TEST_SERVER_SEED }, check, found));
    CHECK_FALSE(SessionKey::SearchNear(TEST_TIME_SEED + 40, 40, 1, 5, { TEST_SERVER_SEED }, check, found));
}

TEST_CASE("The second search pass finds the test data's key with no time") {
    std::vector<std::string> check = firstData(kcpMessages(readFrames(DATA / "frames.bin")), 4);
    std::atomic<bool> cancel = false;
    SessionKey::Found found;
    REQUIRE(SessionKey::SearchSamples(TEST_SAMPLE - 2000000, TEST_SAMPLE + 2000000, { TEST_SERVER_SEED }, check, cancel, found));
    CHECK(found.sample == TEST_SAMPLE);
    CHECK(found.number == 0);
    CHECK(found.key == testKey());
    std::atomic<uint64_t> done = 0;
    CHECK_FALSE(SessionKey::SearchSamples(TEST_SAMPLE + 1, TEST_SAMPLE + 2000000, { TEST_SERVER_SEED }, check, cancel, found, &done));
    CHECK(done == 2000000);
}
