#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "frames.hpp"
#include "kcp.hpp"
#include "messages.hpp"
#include "session_key.hpp"

// Frames in, the game's messages out: frames, KCP, the session key and the
// messages put together, with the state the menu shows. The key search runs
// on its own thread, everything else on the caller's.

namespace Packets {
    enum class State { WaitingForLogin, Searching, Reading, NeedsRelog, NoKey };
    enum class Reason { None, CompressedLogin, StartedLate, MissedPackets, KeyStoppedFitting, UnknownKeyId, NotFound };

    // the codes extensions get, waiting_for_login and so on
    extern const char* StateCode(State state);
    extern const char* ReasonCode(Reason reason);

    class Reader {
    public:
        explicit Reader(const SessionKey::Keys& keys);
        ~Reader();
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        // when the game was started, unix ms. The first search pass looks at every time since
        void SetGameStart(uint64_t unixMs) { m_gameStart = unixMs; }
        // the samples the second pass tries, all of them unless a test makes it fewer
        void SetSamples(int32_t first, int32_t last) { m_samplesFirst = first; m_samplesLast = last; }

        // one captured frame at ms (unix), the messages it finishes go to out
        void Feed(const uint8_t* frame, size_t size, Frames::Start start, uint64_t ms, std::vector<Messages::Message>& out);
        // the messages that waited for the key search, once it's done
        void Update(std::vector<Messages::Message>& out);
        // waits for a key search that runs, for replays. False when none ran
        bool Finish(std::vector<Messages::Message>& out);

        State GetState() const;
        Reason GetReason() const { return m_reason; }
        // 1 from the game's start, 2 every sample, 0 when not searching
        int SearchPass() const;
        // Seconds the pass that runs has left, from how fast it went so far, so
        // it follows the PC's load. -1 while not searching or too early to say
        int SecondsLeft() const;
        // the pass the last search ended in, 0 for the quick search
        int LastSearchPass() const { return m_lastPass; }
        // the last key found, number 0 when found by sample
        const SessionKey::Found& Key() const { return m_found; }
        // messages lost while the search ran or that came broken
        size_t Dropped() const { return m_dropped; }

    private:
        enum class Phase { Idle, Login, Collect, Searching, Session, Stopped };
        struct Search;

        void login(uint64_t ms, bool handshake);
        void stop(State state, Reason reason);
        void handle(Kcp::Message& message, uint64_t ms, std::vector<Messages::Message>& out);
        void loginMessage(Kcp::Message& message, uint64_t ms, std::vector<Messages::Message>& out);
        void sessionMessage(const Kcp::Message& message, std::vector<Messages::Message>& out);
        void startSearch();
        void searchDone(std::vector<Messages::Message>& out);

        std::map<uint16_t, std::string> m_dispatchKeys;
        SessionKey::Rsa m_rsa;
        Kcp::Reader m_kcp;
        // The server the game talks to. Once there is one, packets from any
        // other address are left out until it goes quiet.
        bool m_hasServer = false;
        Frames::Peer m_server;
        uint64_t m_serverSeen = 0;

        Phase m_phase = Phase::Idle;
        bool m_fromHandshake = false;
        State m_state = State::WaitingForLogin;
        Reason m_reason = Reason::None;
        uint64_t m_lastHandshake = 0;
        // data with no login since, to say it started late
        uint64_t m_idleSince = 0;

        std::string m_key;
        std::vector<uint64_t> m_serverSeeds;
        uint64_t m_sentMs = 0;
        // the game's clock in the headers minus ours
        int64_t m_clockOffset = 0;
        uint64_t m_gameStart = 0;
        int32_t m_samplesFirst = SessionKey::SAMPLES_FIRST, m_samplesLast = SessionKey::SAMPLES_LAST;
        // where the game's generator is: its time seed and the last number used
        uint64_t m_generatorSeed = 0;
        int m_generatorNumber = 0;
        // compressed logins, the generator may have been seeded at one
        std::vector<uint64_t> m_seededAt;

        uint64_t m_collectSince = 0;
        std::vector<Kcp::Message> m_waiting;
        std::unique_ptr<Search> m_search;
        SessionKey::Found m_found;
        int m_lastPass = 0;
        size_t m_dropped = 0;
    };
}
