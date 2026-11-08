#include <catch2/catch_test_macros.hpp>
#include "game_traffic_text.hpp"

using GameTraffic::State;
using GameTraffic::Status;
using PacketCapture::Problem;
using Packets::Reason;

static Status cantListen(Problem problem, bool keysMissing = false, bool keysDamaged = false) {
    Status s;
    s.state = State::CantListen;
    s.problem = problem;
    s.keysMissing = keysMissing;
    s.keysDamaged = keysDamaged;
    return s;
}

static Status listening(Packets::State read, Reason reason = Reason::None) {
    Status s;
    s.state = State::Listening;
    s.read = read;
    s.reason = reason;
    s.way = PacketCapture::Way::Npcap;
    return s;
}

static std::string text(const Status& s) {
    return GameTrafficText::For(s).text;
}

static const std::string RELOG = " Log out to the door and back in once.";

TEST_CASE("Game traffic: every state and reason has the menu's sentence") {
    CHECK(text(Status()) == "Off");
    CHECK(GameTrafficText::For(Status()).quiet);

    CHECK(text(listening(Packets::State::WaitingForLogin))
        == "Listening. If you're logged in already, log out to the door and back in once.");
    CHECK(text(listening(Packets::State::Reading)) == "Working (through Npcap)");
    Status admin = listening(Packets::State::Reading);
    admin.way = PacketCapture::Way::RawSockets;
    CHECK(text(admin) == "Working (as admin, without Npcap)");

    CHECK(text(listening(Packets::State::NeedsRelog, Reason::StartedLate)) == "The game was already logged in." + RELOG);
    CHECK(text(listening(Packets::State::NeedsRelog, Reason::CompressedLogin)) == "Couldn't read this login." + RELOG);
    CHECK(text(listening(Packets::State::NeedsRelog, Reason::MissedPackets)) == "Lost some of the game's traffic." + RELOG);
    CHECK(text(listening(Packets::State::NeedsRelog, Reason::KeyStoppedFitting)) == "The game's key changed." + RELOG);
    CHECK(text(listening(Packets::State::NoKey, Reason::UnknownKeyId))
        == "This game version is new. Its key can take a few hours to come. Log in again later.");
    CHECK(text(listening(Packets::State::NoKey, Reason::NotFound))
        == "Couldn't find the game's key. Restart the game, then log in.");

    CHECK(text(cantListen(Problem::NoAdapter)) == "Couldn't listen to the network. Check that you're online, then restart the overlay.");
    CHECK(text(cantListen(Problem::None, true))
        == "The packets folder next to the overlay is missing. Install the overlay again.");
    CHECK(text(cantListen(Problem::None, false, true)) == "The overlay's key files are damaged. Install the overlay again.");
    CHECK(text(cantListen(Problem::NpcapTooOld)) == "Npcap is too old. Install the new one.");
    CHECK(text(cantListen(Problem::NpcapAdminOnly)) == "Npcap is set to admins only. Run the overlay as admin, "
                                                      "or install Npcap again without \"Restrict to administrators\" "
                                                      "(then any program on this PC can read network traffic).");
    CHECK(text(cantListen(Problem::NpcapMissing))
        == "Needs Npcap, a free program that lets the overlay read the game's traffic without running as admin.");
}

TEST_CASE("Game traffic: the Download Npcap button only where Npcap is the fix") {
    GameTrafficText::Line missing = GameTrafficText::For(cantListen(Problem::NpcapMissing));
    CHECK(missing.downloadNpcap);
    CHECK(missing.orAdmin);
    CHECK(GameTrafficText::For(cantListen(Problem::NpcapTooOld)).downloadNpcap);
    CHECK_FALSE(GameTrafficText::For(cantListen(Problem::NpcapTooOld)).orAdmin);
    CHECK(GameTrafficText::For(cantListen(Problem::NpcapAdminOnly)).downloadNpcap);
    CHECK_FALSE(GameTrafficText::For(cantListen(Problem::NoAdapter)).downloadNpcap);
    CHECK_FALSE(GameTrafficText::For(cantListen(Problem::None, true)).downloadNpcap);
    CHECK_FALSE(GameTrafficText::For(cantListen(Problem::None, false, true)).downloadNpcap);
    CHECK_FALSE(GameTrafficText::For(listening(Packets::State::Reading)).downloadNpcap);
}

TEST_CASE("Game traffic: the search says the time left once it knows it") {
    Status s = listening(Packets::State::Searching);
    s.secondsLeft = -1;
    CHECK(text(s) == "Finding the game's key...");
    s.secondsLeft = 130;
    CHECK(text(s) == "Finding the game's key, about 2 min 10 s left");
    s.secondsLeft = 40;
    CHECK(text(s) == "Finding the game's key, about 40 s left");
    CHECK(GameTrafficText::TimeLeft(0) == "0 s");
    CHECK(GameTrafficText::TimeLeft(60) == "1 min 0 s");
}

TEST_CASE("Game traffic: the note on screen is only for a relog") {
    CHECK(GameTrafficText::NeedsRelog(listening(Packets::State::NeedsRelog, Reason::StartedLate)));
    CHECK(GameTrafficText::NeedsRelog(listening(Packets::State::NeedsRelog, Reason::MissedPackets)));
    CHECK_FALSE(GameTrafficText::NeedsRelog(listening(Packets::State::WaitingForLogin)));
    CHECK_FALSE(GameTrafficText::NeedsRelog(listening(Packets::State::NoKey, Reason::NotFound)));
    CHECK_FALSE(GameTrafficText::NeedsRelog(cantListen(Problem::NpcapMissing)));
    CHECK_FALSE(GameTrafficText::NeedsRelog(Status()));
}

#include "npcap_download.hpp"

TEST_CASE("Npcap download: the installer's name from npcap.com's page") {
    // the download list on npcap.com, 2026-11-07
    std::string page =
        "<li><a href=\"dist/npcap-1.90.exe\">Npcap 1.90 installer</a> for Windows 7/2008R2, 8/2012, 8.1/2012R2, 10/2016, 2019, 11 (x86, x64, and ARM64).</li>\n"
        "<li><a href=\"dist/npcap-sdk-1.17.zip\">Npcap SDK 1.17</a> (ZIP).</li>\n"
        "<li><a href=\"dist/npcap-1.90-DebugSymbols.zip\">Npcap 1.90 debug symbols</a> (ZIP).</li>\n";
    CHECK(NpcapDownload::InstallerName(page) == "npcap-1.90.exe");
    // the sdk and the symbols aren't the installer
    CHECK(NpcapDownload::InstallerName(page.substr(page.find("\n") + 1)).empty());
    CHECK(NpcapDownload::InstallerName("").empty());
}

TEST_CASE("Npcap download: only an installer signed by Npcap's makers is opened") {
    // npcap-1.90.exe is signed by Nmap Software LLC, older ones by Insecure.Com LLC
    CHECK(NpcapDownload::NpcapSigner(L"Nmap Software LLC"));
    CHECK(NpcapDownload::NpcapSigner(L"Insecure.Com LLC"));
    CHECK_FALSE(NpcapDownload::NpcapSigner(L"Nmap Software"));
    CHECK_FALSE(NpcapDownload::NpcapSigner(L""));
}

#include <sol/sol.hpp>
#include "lua_functions.hpp"
#include "settings.hpp"

TEST_CASE("Lua: GetGameTraffic gives the switch and the codes, no sentences") {
    sol::state lua;
    lua.open_libraries(sol::lib::base);
    lua.set_function("GetGameTraffic", LuaFunctions::GetGameTraffic);
    Settings::readGameTraffic = false;
    sol::table off = lua.safe_script("return GetGameTraffic()");
    CHECK(off.get<bool>("on") == false);
    CHECK(off.get<std::string>("state") == "off");
    CHECK(off.get<std::string>("reason").empty());
    // the switch is the player's, the state is the worker's
    Settings::readGameTraffic = true;
    sol::table on = lua.safe_script("return GetGameTraffic()");
    CHECK(on.get<bool>("on") == true);
    CHECK(on.get<std::string>("state") == "off");
    Settings::readGameTraffic = false;
}
