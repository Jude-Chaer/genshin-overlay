#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/packet_capture.hpp"
#include "packets/frames.hpp"
#include "packets/game_traffic.hpp"
#include "packets/kcp.hpp"
#include "packets/messages.hpp"
#include "packets/reader.hpp"
#include "packets/session_key.hpp"

// tests/data/traffic is a cut of a real 7.2 session, see its answers.md. The
// whole recordings with their logins are only on the PC they were made on, the
// tests that need them are skipped without them.

using Packets::Reason;
using Packets::State;

namespace {
    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data" / "traffic";
    const std::filesystem::path KEYS = std::filesystem::path(__FILE__).parent_path().parent_path() / "packets";
    const std::filesystem::path RECORDINGS = "D:\\dev\\genshin-overlay\\packets";

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

    std::vector<Frame> recording(const std::string& name) {
        return readFrames(RECORDINGS / name / "frames.bin");
    }

    bool haveRecordings() {
        return std::filesystem::exists(RECORDINGS / "2026-11-06_7.2-cpu" / "frames.bin");
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

    struct Change {
        // seconds from the first frame, below 0 when the answer gives no time
        double at = -1;
        State state = State::WaitingForLogin;
        Reason reason = Reason::None;
    };

    struct Key {
        uint64_t timeSeed;
        int number;
        uint64_t serverSeed;
    };

    struct Replay {
        std::vector<Change> changes;
        std::vector<SessionKey::Found> keys;
        size_t messages = 0;
        // messages that came broken, none in a whole recording
        size_t dropped = 0;
    };

    // Feeds every frame and waits for each key search right where it starts,
    // so a search takes no time on the recording's clock.
    Replay replay(Packets::Reader& reader, const std::vector<Frame>& frames, Frames::Start start = Frames::Start::Ethernet,
        size_t skip = 0) {
        Replay out;
        State state = reader.GetState();
        Reason reason = reader.GetReason();
        std::vector<Messages::Message> messages;
        uint64_t first = frames.empty() ? 0 : frames[0].ms;
        for (const Frame& frame : frames) {
            reader.Feed(frame.data.data() + skip, frame.data.size() - skip, start, frame.ms, messages);
            double at = (frame.ms - first) / 1000.0;
            if (reader.Finish(messages)) {
                if (reader.LastSearchPass() > 0)
                    out.changes.push_back({ at, State::Searching, Reason::None });
                if (reader.GetState() == State::Reading)
                    out.keys.push_back(reader.Key());
            }
            if (reader.GetState() != state || reader.GetReason() != reason) {
                state = reader.GetState();
                reason = reader.GetReason();
                out.changes.push_back({ at, state, reason });
            }
        }
        out.messages = messages.size();
        out.dropped = reader.Dropped();
        return out;
    }

    // Many time seeds and numbers give the same sample and so the same key,
    // the first pass can find any of them
    int32_t sampleOf(uint64_t timeSeed, int number) {
        SessionKey::CsRandom random{ int32_t(uint32_t(timeSeed)) };
        for (int i = 1; i < number; i++)
            random.NextDouble();
        return int32_t(std::llround(random.NextDouble() * INT32_MAX));
    }

    int32_t sampleOf(const SessionKey::Found& found) {
        return found.number ? sampleOf(found.timeSeed, found.number) : found.sample;
    }

    void checkReplay(const Replay& got, const std::vector<Change>& want, const std::vector<Key>& keys) {
        std::string seen;
        for (const Change& change : got.changes)
            seen += std::to_string(change.at) + " " + Packets::StateCode(change.state) + " " + Packets::ReasonCode(change.reason) + ", ";
        INFO("got " << seen);
        REQUIRE(got.changes.size() == want.size());
        for (size_t i = 0; i < want.size(); i++) {
            INFO("change " << i << " at " << got.changes[i].at << " s");
            CHECK(got.changes[i].state == want[i].state);
            CHECK(got.changes[i].reason == want[i].reason);
            if (want[i].at >= 0)
                CHECK(std::abs(got.changes[i].at - want[i].at) < 1.0);
        }
        REQUIRE(got.keys.size() == keys.size());
        for (size_t i = 0; i < keys.size(); i++) {
            INFO("key " << i);
            INFO("found " << got.keys[i].timeSeed << " no. " << got.keys[i].number);
            CHECK(sampleOf(got.keys[i]) == sampleOf(keys[i].timeSeed, keys[i].number));
            CHECK(got.keys[i].serverSeed == keys[i].serverSeed);
        }
        if (!want.empty() && want.back().state == State::Reading)
            CHECK(got.messages > 0);
        CHECK(got.dropped == 0);
    }

    struct Answer {
        std::string name;
        // the game process's start, 0 when the quick search is enough
        uint64_t gameStart;
        std::vector<Change> changes;
        std::vector<Key> keys;
    };

    void checkAnswer(const Answer& answer) {
        if (!haveRecordings())
            SKIP("the recordings aren't on this PC");
        INFO(answer.name);
        Packets::Reader reader(keys());
        reader.SetGameStart(answer.gameStart);
        // none of these needs the second pass
        reader.SetSamples(1, 0);
        checkReplay(replay(reader, recording(answer.name)), answer.changes, answer.keys);
    }

    const State WAIT = State::WaitingForLogin, SEARCH = State::Searching, READ = State::Reading, RELOG = State::NeedsRelog;
    const Reason NONE = Reason::None, LATE = Reason::StartedLate, COMPRESSED = Reason::CompressedLogin;
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

TEST_CASE("The state codes are the ones extensions get") {
    CHECK(std::string(Packets::StateCode(State::WaitingForLogin)) == "waiting_for_login");
    CHECK(std::string(Packets::StateCode(State::NeedsRelog)) == "needs_relog");
    CHECK(std::string(Packets::StateCode(State::NoKey)) == "no_key");
    CHECK(std::string(Packets::ReasonCode(Reason::KeyStoppedFitting)) == "key_stopped_fitting");
    CHECK(std::string(Packets::ReasonCode(Reason::UnknownKeyId)) == "unknown_key_id");
    CHECK(std::string(Packets::ReasonCode(Reason::None)).empty());
}

TEST_CASE("The game traffic's codes are the reader's and the capture's") {
    using GameTraffic::ReasonCode;
    using GameTraffic::StateCode;
    CHECK(std::string(StateCode(GameTraffic::State::Off)) == "off");
    CHECK(std::string(StateCode(GameTraffic::State::CantListen)) == "cant_listen");
    CHECK(std::string(StateCode(GameTraffic::State::WaitingForLogin)) == "waiting_for_login");
    CHECK(std::string(StateCode(GameTraffic::State::NoKey)) == "no_key");
    CHECK(std::string(ReasonCode(GameTraffic::Reason::NpcapMissing)) == "npcap_missing");
    CHECK(std::string(ReasonCode(GameTraffic::Reason::KeysMissing)) == "keys_missing");
    CHECK(std::string(ReasonCode(GameTraffic::Reason::MissedPackets)) == "missed_packets");
    CHECK(std::string(ReasonCode(GameTraffic::Reason::NotFound)) == "not_found");
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

TEST_CASE("Raw sockets keep only the game's udp") {
    std::vector<Frame> frames = readFrames(DATA / "frames.bin");
    REQUIRE(!frames.empty());
    std::vector<uint8_t> ip(frames[0].data.begin() + 14, frames[0].data.end());
    CHECK(PacketCapture::GamePacket(ip.data(), ip.size()));
    // the same packet from another port, the server's port is the source
    size_t udp = size_t(ip[0] & 15) * 4;
    ip[udp] = 0x01;
    ip[udp + 1] = 0xbb;
    CHECK_FALSE(PacketCapture::GamePacket(ip.data(), ip.size()));
    CHECK_FALSE(PacketCapture::GamePacket(ip.data(), 10));
}

TEST_CASE("The capture's queue is capped by bytes and says what it threw away") {
    PacketCapture::Queue queue(1000);
    auto frame = [](size_t size) {
        PacketCapture::Frame f;
        f.data.resize(size);
        return f;
    };
    queue.Push(frame(400));
    queue.Push(frame(400));
    queue.Push(frame(400));
    CHECK(queue.Bytes() == 800);
    CHECK(queue.Missed() == 1);
    std::vector<PacketCapture::Frame> out;
    queue.Take(out, 0);
    CHECK(out.size() == 2);
    CHECK(queue.Bytes() == 0);
    // room again, the count stays
    queue.Push(frame(400));
    CHECK(queue.Bytes() == 400);
    CHECK(queue.Missed() == 1);
    queue.Take(out, 0);
    queue.Take(out, 0);
    CHECK(out.empty());
}

TEST_CASE("Why nothing listens") {
    using PacketCapture::Npcap;
    using PacketCapture::Problem;
    using PacketCapture::PickProblem;
    // installing Npcap is the fix that needs no admin, so it's said first
    CHECK(PickProblem(Npcap::Missing, false, false) == Problem::NpcapMissing);
    CHECK(PickProblem(Npcap::Missing, false, true) == Problem::NpcapMissing);
    CHECK(PickProblem(Npcap::TooOld, false, false) == Problem::NpcapTooOld);
    CHECK(PickProblem(Npcap::Loaded, true, false) == Problem::NpcapAdminOnly);
    CHECK(PickProblem(Npcap::Loaded, true, true) == Problem::NoAdapter);
    CHECK(PickProblem(Npcap::Loaded, false, false) == Problem::NoAdapter);
    CHECK(std::string(PacketCapture::ProblemCode(Problem::NpcapAdminOnly)) == "npcap_admin_only");
    CHECK(std::string(PacketCapture::ProblemCode(Problem::None)).empty());
}

// on a real PC: which way it listens, run by hand with [live]
TEST_CASE("The capture opens this PC's adapters", "[.][live]") {
    PacketCapture::Listener listener;
    bool started = listener.Start();
    INFO("problem " << PacketCapture::ProblemCode(listener.GetProblem()));
    REQUIRE(started);
    WARN("way " << int(listener.GetWay()) << ", " << listener.Live() << " adapters");
    for (const std::string& name : listener.Adapters())
        WARN(name);
    std::vector<PacketCapture::Frame> frames;
    listener.Take(frames, 2000);
    listener.Refresh();
    CHECK(listener.Live() > 0);
    listener.Stop();
    CHECK(listener.Live() == 0);
}

// on a real PC: the menu's switch, off doesn't wait for the worker. Run by hand with [live]
TEST_CASE("Turning game traffic off doesn't wait", "[.][live]") {
    GameTraffic::Start();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto before = std::chrono::steady_clock::now();
    GameTraffic::Stop(false);
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count();
    WARN("Stop(false) took " << took << " ms");
    CHECK(took < 50);
    CHECK(GameTraffic::GetStatus().state == GameTraffic::State::Off);
    // on again at once, then off for good
    GameTraffic::Start();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(GameTraffic::GetStatus().state != GameTraffic::State::Off);
    GameTraffic::Stop();
    CHECK(GameTraffic::GetStatus().state == GameTraffic::State::Off);
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

namespace {
    // The packets below are made by the tests, not caught: a real frame of the
    // test data changed into what a stray or a crafted packet would be.
    // Ethernet and ipv4, like all the test data.
    size_t payloadAt(const Frame& frame) {
        REQUIRE(frame.data.size() > 34);
        REQUIRE(frame.data[12] == 0x08);
        REQUIRE(frame.data[13] == 0x00);
        return 14 + size_t(frame.data[14] & 15) * 4 + 8;
    }

    // the same packet, as if another host had sent it
    Frame fromElsewhere(Frame frame) {
        payloadAt(frame);
        frame.data[26] = 192;
        frame.data[27] = 168;
        frame.data[28] = 1;
        frame.data[29] = 66;
        return frame;
    }

    Frame withNewConv(Frame frame) {
        frame.data[payloadAt(frame)] ^= 0x5a;
        return frame;
    }

    // cut to a 20 byte control packet: 404 is the disconnect, 0xFF the connect
    Frame asControl(Frame frame, uint32_t code) {
        size_t at = payloadAt(frame);
        frame.data.resize(at + 20);
        std::fill(frame.data.begin() + at, frame.data.end(), uint8_t(0));
        for (int b = 0; b < 4; b++)
            frame.data[at + b] = uint8_t(code >> (24 - 8 * b));
        frame.data[at - 4] = 0;
        frame.data[at - 3] = 28;
        size_t total = frame.data.size() - 14;
        frame.data[16] = uint8_t(total >> 8);
        frame.data[17] = uint8_t(total);
        return frame;
    }

    // one of the game's kcp push segments: conv, cmd 81, frg, sn, una 0, the data
    void addSegment(std::vector<uint8_t>& out, uint32_t conv, uint8_t frg, uint32_t sn, const std::string& data) {
        size_t at = out.size();
        out.resize(at + Kcp::HEADER, 0);
        auto le32 = [&](size_t offset, uint32_t value) {
            for (int b = 0; b < 4; b++)
                out[at + offset + b] = uint8_t(value >> (8 * b));
        };
        le32(0, conv);
        out[at + 8] = 81;
        out[at + 9] = frg;
        le32(16, sn);
        le32(24, uint32_t(data.size()));
        out.insert(out.end(), data.begin(), data.end());
    }

    // the strays put in at frame `at`: another host's data with a new conv,
    // its disconnect and a connect coming in
    std::vector<Frame> withStrays(std::vector<Frame> frames, size_t at) {
        Frame model = frames[at];
        std::vector<Frame> strays = { fromElsewhere(withNewConv(model)), fromElsewhere(asControl(model, 404)),
            asControl(model, 0xFF) };
        frames.insert(frames.begin() + at, strays.begin(), strays.end());
        return frames;
    }
}

TEST_CASE("A message whose parts don't count down to 0 stops KCP") {
    // made by the test: frg 1 then 5, the second should have been 0
    std::vector<uint8_t> bad;
    addSegment(bad, 7, 1, 0, "a");
    addSegment(bad, 7, 5, 1, "b");
    Kcp::Reader kcp;
    std::vector<Kcp::Message> out;
    kcp.Feed(Frames::Direction::Received, bad.data(), bad.size(), 1000, out);
    CHECK(out.empty());
    CHECK(kcp.Broken());

    std::vector<uint8_t> good;
    addSegment(good, 7, 1, 0, "a");
    addSegment(good, 7, 0, 1, "b");
    Kcp::Reader fine;
    fine.Feed(Frames::Direction::Received, good.data(), good.size(), 1000, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "ab");
    CHECK_FALSE(fine.Broken());
}

TEST_CASE("A new conv that isn't a game segment doesn't restart KCP") {
    std::vector<Frame> frames = readFrames(DATA / "frames.bin");
    Frame stray = withNewConv(frames[100]);
    stray.data[payloadAt(stray) + 8] = 0;
    frames.insert(frames.begin() + 100, stray);
    bool broken = true;
    CHECK(kcpMessages(frames, false, &broken).size() == 685);
    CHECK_FALSE(broken);
}

TEST_CASE("Frames says which server a packet is from") {
    std::vector<Frame> frames = readFrames(DATA / "frames.bin");
    Frames::Packet first = Frames::Read(frames[0].data.data(), frames[0].data.size(), Frames::Start::Ethernet);
    CHECK(first.server.size == 4);
    CHECK(first.server.port == 22101);
    for (const Frame& frame : frames)
        CHECK(Frames::Read(frame.data.data(), frame.data.size(), Frames::Start::Ethernet).server == first.server);
    Frame other = fromElsewhere(frames[0]);
    CHECK(Frames::Read(other.data.data(), other.data.size(), Frames::Start::Ethernet).server != first.server);
    // the game sends the connect, a connect coming in is nothing
    Frame connect = asControl(frames[0], 0xFF);
    CHECK(Frames::Read(connect.data.data(), connect.data.size(), Frames::Start::Ethernet).kind == Frames::Kind::None);
    Frame disconnect = asControl(frames[0], 404);
    CHECK(Frames::Read(disconnect.data.data(), disconnect.data.size(), Frames::Start::Ethernet).kind == Frames::Kind::Disconnect);
}

TEST_CASE("Packets from another host don't change what the reader says") {
    Packets::Reader reader(keys());
    Replay got = replay(reader, withStrays(readFrames(DATA / "frames.bin"), 800));
    checkReplay(got, { { 5, RELOG, LATE } }, {});
}

TEST_CASE("Recordings: packets from another host in a session change nothing") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    std::vector<Frame> frames = recording("2026-11-06_7.2-cpu");
    size_t at = frames.size() / 2;
    for (;; at++) {
        REQUIRE(at < frames.size());
        Frames::Packet packet = Frames::Read(frames[at].data.data(), frames[at].data.size(), Frames::Start::Ethernet);
        if (packet.kind == Frames::Kind::Data && packet.direction == Frames::Direction::Received)
            break;
    }
    Packets::Reader clean(keys());
    clean.SetSamples(1, 0);
    Replay want = replay(clean, frames);
    Packets::Reader reader(keys());
    reader.SetSamples(1, 0);
    Replay got = replay(reader, withStrays(frames, at));
    checkReplay(got, { { 2.5, READ, NONE } }, { { 1793968363276, 1, 10378004310333966569ull } });
    CHECK(got.messages == want.messages);
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

TEST_CASE("Data with no login says it started late") {
    Packets::Reader reader(keys());
    Replay got = replay(reader, readFrames(DATA / "frames.bin"));
    checkReplay(got, { { 5, RELOG, LATE } }, {});
    CHECK(got.messages == 0);
}

// The answers written for each recording in game-traffic-evidence.md. After a
// find the generator's position is kept, so a later login of the same seed
// is found without a search.

TEST_CASE("Recordings: logins found by the quick search") {
    checkAnswer({ "2026-10-17_19-52", 0, { { 2.6, READ, NONE } }, { { 1792263203681, 1, 14860595775189267988ull } } });
    checkAnswer({ "2026-10-17_20-47", 0, { { 2.6, READ, NONE } }, { { 1792266452264, 1, 6927735276182946501ull } } });
    checkAnswer({ "2026-10-19_stonepile", 0, { { 2.5, READ, NONE } }, { { 1792417318115, 1, 5059795377144074262ull } } });
    checkAnswer({ "2026-11-06_7.2-cpu", 0, { { 2.5, READ, NONE } }, { { 1793968363276, 1, 10378004310333966569ull } } });
    checkAnswer({ "2026-10-18_relog", 0, { { 2.5, READ, NONE }, { 76.9, WAIT, NONE }, { 93.9, READ, NONE } },
        { { 1792319064426, 1, 17523539364873015369ull }, { 1792319064426, 2, 11498923107378565284ull } } });
}

TEST_CASE("Recordings: started in the middle of a session") {
    checkAnswer({ "2026-10-17_20-01", 0, { { 5, RELOG, LATE }, { 10.5, WAIT, NONE }, { 65.2, READ, NONE } },
        { { 1792263759254, 1, 5229061421258750016ull } } });
    checkAnswer({ "2026-10-17_21-00", 0, { { 5, RELOG, LATE }, { 10.6, WAIT, NONE }, { 56.9, READ, NONE } },
        { { 1792267275435, 1, 15789168572167865852ull } } });
    checkAnswer({ "2026-10-18_puzzles", 0,
        { { 5, RELOG, LATE }, { 1080, WAIT, NONE }, { 1096.6, READ, NONE }, { 1403.5, WAIT, NONE }, { 1616.9, READ, NONE } },
        { { 1792350982095, 1, 2645883461685284087ull }, { 1792351502511, 1, 12434495618717912831ull } } });
    checkAnswer({ "2026-10-19_walk2", 0, { { 5, RELOG, LATE } }, {} });
    checkAnswer({ "2026-10-19_walk3", 0, { { 5, RELOG, LATE } }, {} });
}

TEST_CASE("Recordings: compressed logins") {
    checkAnswer({ "2026-10-18_live", 0, { { 5, RELOG, LATE }, { 33.3, WAIT, NONE }, { 47.1, RELOG, COMPRESSED } }, {} });
    checkAnswer({ "2026-10-18_pickups", 0, { { 1.2, RELOG, COMPRESSED } }, {} });
}

TEST_CASE("Recordings: short first passes") {
    checkAnswer({ "2026-11-06_7.2-npcap", 1793966745000, { { 2.4, SEARCH, NONE }, { 2.4, READ, NONE } },
        { { 1793967083884, 2, 10272110719125014212ull } } });
    checkAnswer({ "2026-10-18_pickups2", 1792325446502,
        { { 5, RELOG, LATE }, { 129.4, WAIT, NONE }, { -1, SEARCH, NONE }, { -1, READ, NONE } },
        { { 1792329106632, 2, 1684740102340000188ull } } });
    checkAnswer({ "2026-10-19_walk", 1792413657985, { { 2.4, SEARCH, NONE }, { 2.4, READ, NONE } },
        { { 1792417318115, 2, 2343263635127016416ull } } });
}

// the first pass over hours, about a minute each

TEST_CASE("Recordings: 2026-10-17_18-39, first pass over hours", "[.][slow]") {
    checkAnswer({ "2026-10-17_18-39", 1792244396382, { { 1.8, SEARCH, NONE }, { 1.8, READ, NONE } },
        { { 1792258392710, 2, 11420005278042793181ull } } });
}

TEST_CASE("Recordings: 2026-10-19_puzzles, first pass over hours", "[.][slow]") {
    checkAnswer({ "2026-10-19_puzzles", 1792383749493, { { 2.4, SEARCH, NONE }, { 2.4, READ, NONE } },
        { { 1792397311877, 2, 13903965404270019893ull } } });
}

TEST_CASE("Recordings: 2026-10-19_puzzles-live, first pass over hours", "[.][slow]") {
    checkAnswer({ "2026-10-19_puzzles-live", 1792413657985,
        { { 1.3, RELOG, COMPRESSED }, { 14.2, WAIT, NONE }, { -1, SEARCH, NONE }, { -1, READ, NONE } },
        { { 1792422049578, 7, 3434965444695839439ull } } });
}

TEST_CASE("Recordings: 2026-10-19_puzzles-live-2, first pass over hours", "[.][slow]") {
    checkAnswer({ "2026-10-19_puzzles-live-2", 1792413657985,
        { { 5, RELOG, LATE }, { 36, WAIT, NONE }, { -1, RELOG, COMPRESSED }, { 75.9, WAIT, NONE }, { -1, SEARCH, NONE },
          { -1, READ, NONE }, { 696.5, WAIT, NONE }, { -1, READ, NONE } },
        { { 1792422049578, 9, 15765164060080232710ull }, { 1792422049578, 10, 2806597959625890762ull } } });
}

// The compressed third login took no. 9 of the first login's seed, so the
// fourth is no. 10 of it, found without a search. The answer's no. 28 of
// another seed is the same sample.
TEST_CASE("Recordings: worlds, first pass over hours", "[.][slow]") {
    checkAnswer({ "worlds", 1792311014238,
        { { -1, SEARCH, NONE }, { -1, READ, NONE }, { 14.2, WAIT, NONE }, { -1, READ, NONE }, { 34.7, WAIT, NONE },
          { -1, RELOG, COMPRESSED }, { 59.3, WAIT, NONE }, { -1, READ, NONE } },
        { { 1792322783324, 7, 3159408224249358615ull }, { 1792322783324, 8, 17927823738797336879ull },
          { 1792317979569, 28, 8423759968352905564ull } } });
}

TEST_CASE("Recordings: raw socket frames read from the ip header") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    Packets::Reader reader(keys());
    reader.SetGameStart(1793966745000);
    reader.SetSamples(1, 0);
    // the recorder put a blank ethernet header in front of each ip packet
    checkReplay(replay(reader, recording("2026-11-06_7.2-raw"), Frames::Start::Ip, 14),
        { { 2.4, SEARCH, NONE }, { 2.4, READ, NONE } }, { { 1793967083884, 2, 10272110719125014212ull } });
}

TEST_CASE("Recordings: a minute with nothing from the game goes back to waiting") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    Packets::Reader reader(keys());
    reader.SetGameStart(1793966745000);
    reader.SetSamples(1, 0);
    std::vector<Frame> frames = recording("2026-11-06_7.2-raw");
    replay(reader, frames, Frames::Start::Ip, 14);
    REQUIRE(reader.GetState() == State::Reading);
    uint64_t last = frames.back().ms;
    reader.Tick(last + 59999);
    CHECK(reader.GetState() == State::Reading);
    reader.Tick(last + 60000);
    CHECK(reader.GetState() == State::WaitingForLogin);
    CHECK(reader.GetReason() == Reason::None);
}

TEST_CASE("Recordings: the game traffic status follows the reader") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    Packets::Reader reader(keys());
    CHECK(GameTraffic::FromReader(reader).state == GameTraffic::State::WaitingForLogin);
    reader.SetGameStart(1793966745000);
    reader.SetSamples(1, 0);
    replay(reader, recording("2026-11-06_7.2-raw"), Frames::Start::Ip, 14);
    GameTraffic::Status status = GameTraffic::FromReader(reader);
    CHECK(status.state == GameTraffic::State::Reading);
    CHECK(status.reason == GameTraffic::Reason::None);
    CHECK(status.searchPass == 0);
    CHECK(status.secondsLeft == -1);
    reader.Missed();
    status = GameTraffic::FromReader(reader);
    CHECK(status.state == GameTraffic::State::NeedsRelog);
    CHECK(status.reason == GameTraffic::Reason::MissedPackets);
}

TEST_CASE("Recordings: frames the capture threw away need a relog, before a login they don't") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    Packets::Reader reader(keys());
    reader.SetGameStart(1793966745000);
    reader.SetSamples(1, 0);
    reader.Missed();
    CHECK(reader.GetState() == State::WaitingForLogin);
    replay(reader, recording("2026-11-06_7.2-raw"), Frames::Start::Ip, 14);
    REQUIRE(reader.GetState() == State::Reading);
    reader.Missed();
    CHECK(reader.GetState() == State::NeedsRelog);
    CHECK(reader.GetReason() == Reason::MissedPackets);
}

TEST_CASE("Recordings: a compressed login still tells the next one where the generator is") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    // one overlay run over both, no game start
    std::vector<Frame> frames = recording("2026-10-18_pickups");
    std::vector<Frame> second = recording("2026-10-18_pickups2");
    frames.insert(frames.end(), second.begin(), second.end());
    Packets::Reader reader(keys());
    reader.SetSamples(1, 0);
    Replay got = replay(reader, frames);
    REQUIRE(got.keys.size() == 1);
    CHECK(got.keys[0].number == 2);
    CHECK(got.keys[0].serverSeed == 1684740102340000188ull);
    CHECK(reader.LastSearchPass() == 0);
}

// 7.2-npcap and then 7.2-cpu without its handshakes, one overlay run
std::vector<Frame> missedHandshake(double& secondAt) {
    std::vector<Frame> frames = recording("2026-11-06_7.2-npcap");
    std::vector<Frame> second = recording("2026-11-06_7.2-cpu");
    secondAt = (second[0].ms - frames[0].ms) / 1000.0;
    for (Frame& frame : second) {
        if (Frames::Read(frame.data.data(), frame.data.size(), Frames::Start::Ethernet).kind != Frames::Kind::Handshake)
            frames.push_back(std::move(frame));
    }
    return frames;
}

TEST_CASE("Recordings: a login whose handshake was missed") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    double at = 0;
    std::vector<Frame> frames = missedHandshake(at);
    Packets::Reader reader(keys());
    reader.SetGameStart(1793966745000);
    reader.SetSamples(1, 0);
    checkReplay(replay(reader, frames),
        { { 2.4, SEARCH, NONE }, { 2.4, READ, NONE }, { at, WAIT, NONE }, { at + 2.5, READ, NONE } },
        { { 1793967083884, 2, 10272110719125014212ull }, { 1793968363276, 1, 10378004310333966569ull } });
}

TEST_CASE("Recordings: no key for a login whose handshake was missed") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    double at = 0;
    std::vector<Frame> frames = missedHandshake(at);
    SessionKey::Keys loaded = keys();
    std::vector<Kcp::Message> messages = kcpMessages(recording("2026-11-06_7.2-cpu"), true);
    REQUIRE(!messages.empty());
    uint16_t id = Messages::KeyId(messages[0].data);
    REQUIRE(loaded.dispatch.erase(id) == 1);

    // both are 7.2, the first login had its handshake, the second may not be a login at all
    Packets::Reader reader(loaded);
    checkReplay(replay(reader, frames),
        { { 0.1, State::NoKey, Reason::UnknownKeyId }, { at, RELOG, LATE } }, {});
}

// The states no recording has, made from real recordings.

TEST_CASE("Recordings: a frame missed after the login") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    std::vector<Frame> frames = recording("2026-11-06_7.2-cpu");
    // the first frame from the server 20 s in
    uint64_t at = frames[0].ms + 20000;
    for (size_t i = 0; i < frames.size(); i++) {
        Frames::Packet packet = Frames::Read(frames[i].data.data(), frames[i].data.size(), Frames::Start::Ethernet);
        if (frames[i].ms >= at && packet.kind == Frames::Kind::Data && packet.direction == Frames::Direction::Received
            && packet.size > Kcp::HEADER + 32) {
            frames.erase(frames.begin() + i);
            break;
        }
    }
    Packets::Reader reader(keys());
    reader.SetSamples(1, 0);
    Replay got = replay(reader, frames);
    REQUIRE(got.changes.size() == 2);
    CHECK(got.changes[0].state == READ);
    CHECK(got.changes[1].state == RELOG);
    CHECK(got.changes[1].reason == Reason::MissedPackets);
    CHECK(got.changes[1].at > 20);
    CHECK(got.changes[1].at < 22);
}

TEST_CASE("Recordings: no key for the game's dispatch key id") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    std::vector<Frame> frames = recording("2026-11-06_7.2-cpu");
    SessionKey::Keys loaded = keys();
    // the id is in the first sent message
    std::vector<Kcp::Message> messages = kcpMessages(frames, true);
    REQUIRE(!messages.empty());
    uint16_t id = Messages::KeyId(messages[0].data);
    REQUIRE(loaded.dispatch.erase(id) == 1);

    Packets::Reader reader(loaded);
    Replay got = replay(reader, frames);
    REQUIRE(got.changes.size() == 1);
    CHECK(got.changes[0].state == State::NoKey);
    CHECK(got.changes[0].reason == Reason::UnknownKeyId);
}

TEST_CASE("Recordings: a wrong game start leaves it to the second pass") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    std::vector<Frame> frames = recording("2026-11-06_7.2-npcap");
    // the key's time seed is 339 s after the real start
    const uint64_t wrongStart = 1793966745000 + 400000;
    int32_t sample = sampleOf(1793967083884, 2);

    {
        Packets::Reader reader(keys());
        reader.SetGameStart(wrongStart);
        reader.SetSamples(sample - 1000000, sample + 1000000);
        Replay got = replay(reader, frames);
        REQUIRE(got.keys.size() == 1);
        CHECK(got.keys[0].sample == sample);
        CHECK(got.keys[0].serverSeed == 10272110719125014212ull);
        CHECK(reader.LastSearchPass() == 2);
        CHECK(reader.GetState() == READ);
    }
    {
        Packets::Reader reader(keys());
        reader.SetGameStart(wrongStart);
        reader.SetSamples(sample + 1, sample + 1000000);
        Replay got = replay(reader, frames);
        CHECK(got.keys.empty());
        CHECK(reader.GetState() == State::NoKey);
        CHECK(reader.GetReason() == Reason::NotFound);
    }
}

TEST_CASE("Recordings: a running pass says how long it has left") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    std::vector<Frame> frames = recording("2026-11-06_7.2-npcap");
    Packets::Reader reader(keys());
    // after the key's time seed, so the first pass misses and the second runs over every sample
    reader.SetGameStart(1793966745000 + 400000);
    std::vector<Messages::Message> messages;
    for (const Frame& frame : frames) {
        reader.Feed(frame.data.data(), frame.data.size(), Frames::Start::Ethernet, frame.ms, messages);
        if (reader.SearchPass() == 2)
            break;
    }
    for (int i = 0; i < 100 && reader.SearchPass() != 2; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(reader.SearchPass() == 2);
    CHECK(reader.SecondsLeft() == -1);
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    int first = reader.SecondsLeft();
    // about 400 s on the PC it was written on
    CHECK(first > 30);
    CHECK(first < 7200);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    int second = reader.SecondsLeft();
    CHECK(second < first);
}

TEST_CASE("Recordings: a message the key doesn't fit") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    std::vector<Frame> frames = recording("2026-11-06_7.2-cpu");
    std::string key = SessionKey::KeyFromSeed(
        SessionKey::CsRandom{ int32_t(uint32_t(1793968363276)) }.NextSafeUInt64() ^ 10378004310333966569ull);
    std::string other = SessionKey::KeyFromSeed(1);
    // A received frame 20 s in that is one whole message in one segment, its
    // bytes encrypted with another key instead
    uint64_t at = frames[0].ms + 20000;
    bool changed = false;
    for (Frame& frame : frames) {
        Frames::Packet packet = Frames::Read(frame.data.data(), frame.data.size(), Frames::Start::Ethernet);
        if (frame.ms < at || packet.kind != Frames::Kind::Data || packet.direction != Frames::Direction::Received)
            continue;
        uint8_t* segment = const_cast<uint8_t*>(packet.data);
        uint32_t length = uint32_t(segment[24]) | uint32_t(segment[25]) << 8 | uint32_t(segment[26]) << 16 | uint32_t(segment[27]) << 24;
        if (segment[8] != 81 || segment[9] != 0 || Kcp::HEADER + length != packet.size)
            continue;
        std::string data((const char*)segment + Kcp::HEADER, length);
        if (!Messages::Fits(key, data))
            continue;
        for (size_t i = 0; i < length; i++)
            segment[Kcp::HEADER + i] ^= uint8_t(key[i % key.size()] ^ other[i % other.size()]);
        changed = true;
        break;
    }
    REQUIRE(changed);

    Packets::Reader reader(keys());
    reader.SetSamples(1, 0);
    Replay got = replay(reader, frames);
    REQUIRE(got.changes.size() == 2);
    CHECK(got.changes[0].state == READ);
    CHECK(got.changes[1].state == RELOG);
    CHECK(got.changes[1].reason == Reason::KeyStoppedFitting);
}

// How long each search pass takes on this PC, run by hand: tests.exe "[measure]"
TEST_CASE("Search pass times", "[.][measure]") {
    if (!haveRecordings())
        SKIP("the recordings aren't on this PC");
    // the server seeds a real login gives, from 7.2-cpu's login answer
    SessionKey::Keys loaded = keys();
    SessionKey::Rsa rsa(loaded.rsaPem);
    std::vector<Kcp::Message> messages = kcpMessages(recording("2026-11-06_7.2-cpu"), true);
    REQUIRE(!messages.empty());
    uint16_t id = Messages::KeyId(messages[0].data);
    const std::string& dispatch = loaded.dispatch.at(id);
    std::vector<uint64_t> seeds;
    std::vector<std::string> check;
    for (const Kcp::Message& message : messages) {
        Messages::Message opened;
        if (Messages::Open(dispatch, message.data, message.direction, opened)) {
            if (seeds.empty() && message.direction == Frames::Direction::Received)
                seeds = rsa.ServerSeeds(opened.body);
        }
        else if (check.size() < 4)
            check.push_back(message.data);
    }
    REQUIRE(!seeds.empty());
    REQUIRE(check.size() == 4);
    // one off, so nothing is found and every candidate is tried
    for (uint64_t& seed : seeds)
        seed++;

    std::atomic<bool> cancel = false;
    SessionKey::Found found;
    auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(SessionKey::SearchBetween(1793968363276 - 3600000, 1793968363276, 32, seeds, check, cancel, found));
    double hour = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    CHECK_FALSE(SessionKey::SearchSamples(SessionKey::SAMPLES_FIRST, SessionKey::SAMPLES_LAST, seeds, check, cancel, found));
    double all = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    WARN(seeds.size() << " server seeds, first pass " << hour << " s per hour, second pass " << all << " s");
}
