#include "reader.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>

namespace Packets {
    // a key has to fit this many messages after the login
    static constexpr size_t CHECK_MESSAGES = 4;
    // or as many as came this long after the first
    static constexpr uint64_t CHECK_WAIT_MS = 2000;
    // the quick search: time seeds around the login and numbers of each
    static constexpr int NEAR_WINDOW = 3000;
    static constexpr int NEAR_COUNT = 5;
    // the first pass: numbers per time seed, how far back at most
    static constexpr int BETWEEN_DEPTH = 32;
    static constexpr uint64_t BETWEEN_MAX_MS = 24ull * 60 * 60 * 1000;
    static constexpr size_t MAX_WAITING = 20000;
    // data this long with no login seen
    static constexpr uint64_t LATE_AFTER_MS = 5000;
    // the same handshake from more than one adapter
    static constexpr uint64_t HANDSHAKE_COPY_MS = 1000;
    // In the recordings the server talks at least every 3.5 s, and a new
    // server came 9 s or more after the old one went quiet. One this quiet
    // can be taken over by another, for a handshake that was missed.
    static constexpr uint64_t SERVER_QUIET_MS = 5000;
    // a pass has to run this long before it says how long it has left
    static constexpr int64_t ESTIMATE_AFTER_MS = 2000;

    static int64_t steadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    const char* StateCode(State state) {
        switch (state) {
        case State::WaitingForLogin: return "waiting_for_login";
        case State::Searching: return "searching";
        case State::Reading: return "reading";
        case State::NeedsRelog: return "needs_relog";
        case State::NoKey: return "no_key";
        }
        return "";
    }

    const char* ReasonCode(Reason reason) {
        switch (reason) {
        case Reason::None: return "";
        case Reason::CompressedLogin: return "compressed_login";
        case Reason::StartedLate: return "started_late";
        case Reason::MissedPackets: return "missed_packets";
        case Reason::KeyStoppedFitting: return "key_stopped_fitting";
        case Reason::UnknownKeyId: return "unknown_key_id";
        case Reason::NotFound: return "not_found";
        }
        return "";
    }

    struct Reader::Search {
        std::atomic<bool> cancel = false;
        std::atomic<bool> done = false;
        std::atomic<int> pass = 0;
        // the pass that runs: what it has tried, out of how many, since when
        std::atomic<uint64_t> tried = 0, total = 0;
        std::atomic<int64_t> passStart = 0;
        bool found = false;

        void startPass(int number, uint64_t count) {
            tried = 0;
            total = count;
            passStart = steadyMs();
            pass = number;
        }
        SessionKey::Found key;
        std::thread thread;

        ~Search() {
            cancel = true;
            if (thread.joinable())
                thread.join();
        }
    };

    Reader::Reader(const SessionKey::Keys& keys) : m_dispatchKeys(keys.dispatch), m_rsa(keys.rsaPem) {}

    Reader::~Reader() = default;

    State Reader::GetState() const {
        // the quick search is short, the menu only hears about the long ones.
        // Searching until the found key is used, not just until the search ends
        if (m_phase == Phase::Searching && m_search && m_search->pass > 0)
            return State::Searching;
        return m_state;
    }

    int Reader::SearchPass() const {
        return GetState() == State::Searching ? m_search->pass.load() : 0;
    }

    int Reader::SecondsLeft() const {
        if (GetState() != State::Searching)
            return -1;
        int64_t elapsed = steadyMs() - m_search->passStart;
        uint64_t done = m_search->tried, total = m_search->total;
        if (elapsed < ESTIMATE_AFTER_MS || done == 0 || done >= total)
            return -1;
        return int(double(elapsed) / done * (total - done) / 1000 + 0.5);
    }

    void Reader::Feed(const uint8_t* frame, size_t size, Frames::Start start, uint64_t ms, std::vector<Messages::Message>& out) {
        Update(out);
        Frames::Packet packet = Frames::Read(frame, size, start);
        if (packet.kind == Frames::Kind::None)
            return;
        // A handshake picks the server, else the first one seen does. The old
        // server's last packets, and anyone else's, are left out.
        if (packet.kind != Frames::Kind::Handshake) {
            if (m_hasServer && packet.server != m_server && ms < m_serverSeen + SERVER_QUIET_MS)
                return;
            m_hasServer = true;
            m_server = packet.server;
        }
        m_serverSeen = ms;
        switch (packet.kind) {
        case Frames::Kind::None:
            return;
        case Frames::Kind::Handshake:
            if (m_lastHandshake && ms < m_lastHandshake + HANDSHAKE_COPY_MS)
                return;
            m_lastHandshake = ms;
            m_hasServer = true;
            m_server = packet.server;
            login(ms, true);
            return;
        case Frames::Kind::Disconnect:
            // the old connection's disconnect, copied from another adapter
            if (m_lastHandshake && ms < m_lastHandshake + HANDSHAKE_COPY_MS)
                return;
            m_hasServer = false;
            m_search.reset();
            m_waiting.clear();
            m_phase = Phase::Idle;
            m_state = State::WaitingForLogin;
            m_reason = Reason::None;
            m_idleSince = 0;
            return;
        case Frames::Kind::Data:
            break;
        }

        std::vector<Kcp::Message> messages;
        m_kcp.Feed(packet.direction, packet.data, packet.size, ms, messages);
        if (m_kcp.NewConnection())
            login(ms, false);
        if (m_kcp.Broken() && m_phase != Phase::Idle && m_phase != Phase::Stopped) {
            stop(State::NeedsRelog, Reason::MissedPackets);
            return;
        }

        if (m_phase == Phase::Idle) {
            if (!m_idleSince)
                m_idleSince = ms;
            else if (ms >= m_idleSince + LATE_AFTER_MS && m_state == State::WaitingForLogin) {
                m_state = State::NeedsRelog;
                m_reason = Reason::StartedLate;
            }
            return;
        }
        for (Kcp::Message& message : messages)
            handle(message, ms, out);
        if (m_phase == Phase::Collect && ms >= m_collectSince + CHECK_WAIT_MS)
            startSearch();
    }

    void Reader::login(uint64_t ms, bool handshake) {
        m_search.reset();
        m_waiting.clear();
        if (handshake)
            m_kcp.Reset();
        m_phase = Phase::Login;
        m_fromHandshake = handshake;
        m_state = State::WaitingForLogin;
        m_reason = Reason::None;
        m_idleSince = 0;
        m_key.clear();
        m_serverSeeds.clear();
        m_sentMs = 0;
    }

    void Reader::stop(State state, Reason reason) {
        m_search.reset();
        m_waiting.clear();
        m_phase = Phase::Stopped;
        m_state = state;
        m_reason = reason;
    }

    void Reader::handle(Kcp::Message& message, uint64_t ms, std::vector<Messages::Message>& out) {
        switch (m_phase) {
        case Phase::Login:
            loginMessage(message, ms, out);
            return;
        case Phase::Collect:
            m_waiting.push_back(std::move(message));
            if (m_waiting.size() >= CHECK_MESSAGES)
                startSearch();
            return;
        case Phase::Searching:
            if (m_waiting.size() < MAX_WAITING)
                m_waiting.push_back(std::move(message));
            else
                m_dropped++;
            return;
        case Phase::Session:
            sessionMessage(message, out);
            return;
        default:
            return;
        }
    }

    void Reader::loginMessage(Kcp::Message& message, uint64_t ms, std::vector<Messages::Message>& out) {
        const std::string& data = message.data;
        if (data.size() < 4)
            return;
        if (m_key.empty()) {
            auto found = m_dispatchKeys.find(Messages::KeyId(data));
            if (found == m_dispatchKeys.end()) {
                // without the handshake this may not be the login at all
                if (m_fromHandshake)
                    stop(State::NoKey, Reason::UnknownKeyId);
                else
                    stop(State::NeedsRelog, Reason::StartedLate);
                return;
            }
            m_key = found->second;
        }

        Messages::Message opened;
        if (!Messages::Fits(m_key, data)) {
            // the game moved on to the session key
            if (m_serverSeeds.empty()) {
                stop(State::NoKey, Reason::NotFound);
                return;
            }
            m_phase = Phase::Collect;
            m_collectSince = ms;
            m_waiting.push_back(std::move(message));
            return;
        }
        if (!Messages::Open(m_key, data, message.direction, opened)) {
            m_dropped++;
            return;
        }

        if (message.direction == Frames::Direction::Received) {
            uint64_t sentMs = 0, unpackedSize = 0;
            std::string_view head = opened.head;
            Messages::Field field;
            while (Messages::NextField(head, field)) {
                if (field.number == 6 && field.wire == 0)
                    sentMs = field.value;
                if (field.number == 8 && field.wire == 0)
                    unpackedSize = field.value;
            }
            if (unpackedSize) {
                // The login answer came Oodle compressed, which can't be read
                // here. The game's generator was still seeded at this login.
                if (sentMs)
                    m_seededAt.push_back(sentMs);
                out.push_back(std::move(opened));
                stop(State::NeedsRelog, Reason::CompressedLogin);
                return;
            }
            std::vector<uint64_t> seeds = m_rsa.ServerSeeds(opened.body);
            if (!seeds.empty()) {
                m_serverSeeds = std::move(seeds);
                m_sentMs = sentMs;
                if (sentMs)
                    m_clockOffset = int64_t(sentMs) - int64_t(ms);
            }
        }
        out.push_back(std::move(opened));
    }

    void Reader::sessionMessage(const Kcp::Message& message, std::vector<Messages::Message>& out) {
        if (!Messages::Fits(m_key, message.data, false)) {
            stop(State::NeedsRelog, Reason::KeyStoppedFitting);
            return;
        }
        Messages::Message opened;
        if (Messages::Open(m_key, message.data, message.direction, opened))
            out.push_back(std::move(opened));
        else
            m_dropped++;
    }

    // The quick search tries near this login and where the generator was left.
    // If that misses, the first pass tries every time since the game started,
    // the second every number the generator can give.
    void Reader::startSearch() {
        m_phase = Phase::Searching;
        m_search = std::make_unique<Search>();
        Search* search = m_search.get();
        std::vector<std::string> check;
        for (size_t i = 0; i < m_waiting.size() && i < CHECK_MESSAGES; i++)
            check.push_back(m_waiting[i].data);

        uint64_t between[2] = {};
        if (m_gameStart && m_sentMs) {
            uint64_t to = m_sentMs + 1000;
            uint64_t from = uint64_t(int64_t(m_gameStart) + m_clockOffset) - 10000;
            from = std::max(from, to - std::min(to, BETWEEN_MAX_MS));
            if (from < to) {
                between[0] = from;
                between[1] = to;
            }
        }
        search->thread = std::thread([search, check, seeds = m_serverSeeds, sentMs = m_sentMs, generatorSeed = m_generatorSeed,
            generatorNumber = m_generatorNumber, seededAt = m_seededAt, from = between[0], to = between[1],
            samplesFirst = m_samplesFirst, samplesLast = m_samplesLast] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            bool found = false;
            if (generatorSeed)
                found = SessionKey::SearchNear(generatorSeed, 1, generatorNumber + 1, NEAR_COUNT, seeds, check, search->key);
            for (size_t i = 0; i < seededAt.size() && !found; i++)
                found = SessionKey::SearchNear(seededAt[i], NEAR_WINDOW, 1, NEAR_COUNT, seeds, check, search->key);
            if (!found && sentMs)
                found = SessionKey::SearchNear(sentMs, NEAR_WINDOW, 1, NEAR_COUNT, seeds, check, search->key);
            if (!found && to && !search->cancel) {
                search->startPass(1, to - from + 1);
                found = SessionKey::SearchBetween(from, to, BETWEEN_DEPTH, seeds, check, search->cancel, search->key, &search->tried);
            }
            if (!found && !search->cancel) {
                search->startPass(2, uint64_t(int64_t(samplesLast) - samplesFirst + 1));
                found = SessionKey::SearchSamples(samplesFirst, samplesLast, seeds, check, search->cancel, search->key, &search->tried);
            }
            search->found = found;
            search->done = true;
        });
    }

    void Reader::Update(std::vector<Messages::Message>& out) {
        if (m_phase == Phase::Searching && m_search && m_search->done)
            searchDone(out);
    }

    bool Reader::Finish(std::vector<Messages::Message>& out) {
        if (m_phase != Phase::Searching || !m_search)
            return false;
        searchDone(out);
        return true;
    }

    void Reader::searchDone(std::vector<Messages::Message>& out) {
        if (m_search->thread.joinable())
            m_search->thread.join();
        m_lastPass = m_search->pass;
        if (!m_search->found) {
            stop(State::NoKey, Reason::NotFound);
            return;
        }
        m_found = std::move(m_search->key);
        m_search.reset();
        // the kept position covers them from here
        m_seededAt.clear();
        m_key = m_found.key;
        if (m_found.timeSeed) {
            m_generatorSeed = m_found.timeSeed;
            m_generatorNumber = m_found.number;
        }
        else {
            m_generatorSeed = 0;
            m_generatorNumber = 0;
        }
        m_phase = Phase::Session;
        m_state = State::Reading;
        m_reason = Reason::None;
        std::vector<Kcp::Message> waiting = std::move(m_waiting);
        m_waiting.clear();
        for (const Kcp::Message& message : waiting) {
            sessionMessage(message, out);
            if (m_phase != Phase::Session)
                return;
        }
    }
}
