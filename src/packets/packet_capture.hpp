#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "frames.hpp"

// Catches the game's udp packets. Npcap when it's installed, it needs no
// admin. Without it Windows' own raw sockets, but only when the overlay runs
// as admin. Npcap isn't bundled (its license doesn't allow it), wpcap.dll is
// loaded at Start so the overlay runs fine without it. Nothing is ever sent.

namespace PacketCapture {
    enum class Way { None, Npcap, RawSockets };
    enum class Problem { None, NpcapMissing, NpcapTooOld, NpcapAdminOnly, NoAdapter };
    enum class Npcap { Missing, TooOld, Loaded };

    // the codes extensions get, npcap_missing and so on
    extern const char* ProblemCode(Problem problem);

    // Why nothing listens, from what Start found. Npcap missing wins over the
    // rest because installing it is the fix that needs no admin.
    extern Problem PickProblem(Npcap npcap, bool adminOnly, bool admin);

    // true for the game's udp, what raw sockets keep of everything they see
    extern bool GamePacket(const uint8_t* ip, size_t size);

    struct Frame {
        // unix ms when it was caught
        uint64_t ms = 0;
        Frames::Start start = Frames::Start::Ethernet;
        std::vector<uint8_t> data;
    };

    // The frames the capture threads caught and nobody took yet, capped by
    // bytes. A frame that doesn't fit is thrown away and counted: the game's
    // stream can't be read past a missing frame, so it has to be said.
    class Queue {
    public:
        explicit Queue(size_t maxBytes) : m_maxBytes(maxBytes) {}

        void Push(Frame&& frame);
        // waits up to milliseconds for a frame, then hands over all, oldest first
        void Take(std::vector<Frame>& out, int milliseconds);
        void Clear();
        // frames thrown away since the queue was made
        uint64_t Missed() const;
        size_t Bytes() const;

    private:
        size_t m_maxBytes;
        mutable std::mutex m_mutex;
        std::condition_variable m_wake;
        std::deque<Frame> m_frames;
        size_t m_bytes = 0;
        uint64_t m_missed = 0;
    };

    // Owns the capture threads, one per adapter. Start, Stop, Refresh and Take
    // are for one thread, the worker's.
    class Listener {
    public:
        Listener();
        ~Listener();
        Listener(const Listener&) = delete;
        Listener& operator=(const Listener&) = delete;

        // opens every adapter it can, false when none opened, GetProblem says why
        bool Start();
        void Stop();
        // Lists the adapters again (a Wi-Fi switch, a VPN) and starts over
        // when they changed or a thread stopped. Throws away what's waiting.
        void Refresh();

        Way GetWay() const { return m_way; }
        Problem GetProblem() const { return m_problem; }
        // the adapters' names, for the console
        const std::vector<std::string>& Adapters() const { return m_names; }
        // capture threads still catching, 0 when every adapter went away
        int Live() const { return m_live; }

        void Take(std::vector<Frame>& out, int milliseconds) { m_queue.Take(out, milliseconds); }
        uint64_t Missed() const { return m_queue.Missed(); }

    private:
        struct Opened;
        bool startNpcap(std::vector<Opened>& opened);
        bool startRaw(std::vector<Opened>& opened);
        std::vector<std::string> list() const;

        Queue m_queue;
        Way m_way = Way::None;
        Problem m_problem = Problem::None;
        std::vector<std::string> m_names;
        // what Refresh compares: npcap device names or raw socket addresses
        std::vector<std::string> m_keys;
        // how many devices Npcap listed, opened or not
        size_t m_listed = 0;
        std::vector<std::thread> m_threads;
        std::atomic<bool> m_stopping = false;
        std::atomic<int> m_live = 0;
        bool m_winsock = false;
    };
}
