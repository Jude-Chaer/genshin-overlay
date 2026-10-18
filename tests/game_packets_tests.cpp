#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "packets/game_packets.hpp"

using namespace GamePackets;

static void put16(std::string& s, uint16_t v) { s += char(v >> 8); s += char(v); }
static void put32(std::string& s, uint32_t v) { put16(s, uint16_t(v >> 16)); put16(s, uint16_t(v)); }
static void putLe32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s += char(v >> (8 * i)); }

// ethernet, ipv4, udp from the game's server to us
static std::vector<uint8_t> frame(const std::string& payload) {
    std::string f(12, '\x11');
    put16(f, 0x0800);
    f += '\x45';
    f += '\0';
    put16(f, uint16_t(20 + 8 + payload.size()));
    f += std::string(5, '\0');
    f += char(17);
    f += std::string(10, '\0');
    put16(f, 22101);
    put16(f, 50000);
    put16(f, uint16_t(8 + payload.size()));
    put16(f, 0);
    f += payload;
    return std::vector<uint8_t>(f.begin(), f.end());
}

// one segment in the game's KCP, with its 8 extra header bytes
static std::string segment(uint32_t sn, uint8_t frg, const std::string& data) {
    std::string s;
    putLe32(s, 7);
    putLe32(s, 0);
    s += char(81);
    s += char(frg);
    s += std::string(6, '\0');
    putLe32(s, sn);
    putLe32(s, 0);
    putLe32(s, uint32_t(data.size()));
    putLe32(s, 0);
    return s + data;
}

static std::string message(uint16_t id, const std::string& head, const std::string& body) {
    std::string m = "\x45\x67";
    put16(m, id);
    put16(m, uint16_t(head.size()));
    put32(m, uint32_t(body.size()));
    return m + head + body + "\x89\xAB";
}

static std::string encrypt(std::string data, const std::string& key) {
    for (size_t i = 0; i < data.size(); i++) data[i] ^= key[i % key.size()];
    return data;
}

static Keys dispatchKeys(const std::string& key) {
    Keys keys;
    // the game version is in the first two bytes, they decrypt to 45 67
    keys.dispatch[uint16_t(uint8_t(key[0]) << 8 | uint8_t(key[1]))] = key;
    return keys;
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
        CsRandom random(c.seed);
        CHECK(random.NextDouble() == c.first);
        CHECK(random.NextDouble() == c.second);
    }
}

TEST_CASE("SearchKey finds the time and the key a message was encrypted with") {
    const uint64_t time = 1792258392710, serverSeed = 0x1234567890abcdefULL;
    CsRandom random{ int32_t(uint32_t(time)) };
    random.NextSafeUInt64();
    random.NextSafeUInt64();
    // the third number, like after two relogs
    std::string key = KeyFromSeed(random.NextSafeUInt64() ^ serverSeed);
    std::string data = encrypt(message(100, "head", "body"), key);

    uint64_t found = 0;
    std::string foundKey;
    REQUIRE(SearchKey(time + 40, serverSeed, data, 3000, 5, found, foundKey));
    CHECK(found == time);
    CHECK(foundKey == key);
    CHECK_FALSE(SearchKey(time + 40, serverSeed, data, 3000, 2, found, foundKey));
}

TEST_CASE("Sniffer puts fragments back together whatever order they come in") {
    std::string key = KeyFromSeed(1);
    Sniffer sniffer(dispatchKeys(key));
    std::string data = encrypt(message(28616, "\x30\x01", std::string(300, 'x')), key);
    std::string first = data.substr(0, 100), second = data.substr(100);

    std::vector<Command> out;
    auto feed = [&](const std::string& payload) {
        std::vector<uint8_t> f = frame(payload);
        sniffer.Feed(f.data(), f.size(), out);
    };
    feed(segment(1, 0, second));
    CHECK(out.empty());
    feed(segment(0, 1, first));
    feed(segment(0, 1, first));
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 28616);
    CHECK(out[0].direction == Direction::Received);
    CHECK(out[0].head == "\x30\x01");
    CHECK(out[0].body == std::string(300, 'x'));
    CHECK(sniffer.Problem().empty());

    // two messages in one frame
    out.clear();
    feed(segment(2, 0, encrypt(message(1, "", "a"), key)) + segment(3, 0, encrypt(message(2, "", "b"), key)));
    REQUIRE(out.size() == 2);
    CHECK(out[0].body == "a");
    CHECK(out[1].body == "b");
}

TEST_CASE("Sniffer says when there's no key for the game version") {
    std::string key = KeyFromSeed(1);
    Sniffer sniffer(dispatchKeys(KeyFromSeed(2)));
    std::vector<Command> out;
    std::vector<uint8_t> f = frame(segment(0, 0, encrypt(message(1, "", "a"), key)));
    sniffer.Feed(f.data(), f.size(), out);
    CHECK(out.empty());
    CHECK(sniffer.Problem().find("game version") != std::string::npos);
}

TEST_CASE("Sniffer sees the login handshake") {
    Sniffer sniffer(Keys{});
    std::string payload;
    put32(payload, 0xFF);
    payload += std::string(16, '\0');
    std::vector<uint8_t> f = frame(payload);
    std::vector<Command> out;
    CHECK(sniffer.Feed(f.data(), f.size(), out) == Event::Handshake);
}

TEST_CASE("NextField reads protobuf without knowing the message") {
    // 1: 150, 2: "hi", 3: fixed32 1
    std::string data("\x08\x96\x01\x12\x02hi\x1d\x01\x00\x00\x00", 12);
    std::string_view rest = data;
    Field field;
    REQUIRE(NextField(rest, field));
    CHECK((field.number == 1 && field.wire == 0 && field.value == 150));
    REQUIRE(NextField(rest, field));
    CHECK((field.number == 2 && field.bytes == "hi"));
    REQUIRE(NextField(rest, field));
    CHECK((field.number == 3 && field.wire == 5 && field.value == 1));
    CHECK_FALSE(NextField(rest, field));

    // a length past the end
    std::string broken = "\x12\x09hi";
    rest = broken;
    CHECK_FALSE(NextField(rest, field));
}
