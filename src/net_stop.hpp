#pragma once
#include <atomic>
#include <cstddef>
#include <functional>

// What a download is told and how it's stopped, without windows.h, so a
// header that only passes them on stays light.

namespace Net {
    // told the bytes so far after each piece that comes in, false stops the download
    using Progress = std::function<bool(size_t got)>;

    // Lets another thread cut a get short at once, also while it's waiting on
    // the network. Once stopped, every get given it fails right away.
    class Stopper {
    public:
        Stopper();
        ~Stopper();
        Stopper(const Stopper&) = delete;
        Stopper& operator=(const Stopper&) = delete;

        void stop();
        bool stopped() const { return done.load(); }
        // for code that takes a plain stop flag
        const std::atomic<bool>* flag() const { return &done; }

    private:
        friend struct Call;
        void* event = nullptr;  // set by stop, a get waits on it next to its own steps
        std::atomic<bool> done{ false };
    };
}
