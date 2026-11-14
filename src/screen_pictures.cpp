#include "capture.hpp"
#include "screen_pictures.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace ScreenPictures {
    // The pictures are made on a thread of their own, so the draw loop never
    // waits for the capture or the GPU. It runs while a width was asked for in
    // the last second and hands over the newest picture of each.
    namespace Maker {
        struct Made {
            cv::Mat bgra;
            uint64_t frame = 0;
            std::chrono::steady_clock::time_point asked;
        };
        static std::mutex mutex;
        static std::condition_variable wake;
        static std::map<int, Made> widths;
        static std::thread thread;
        static bool stopping = false;
        // how long a width is made after it was last asked for
        static constexpr auto KEPT = std::chrono::seconds(1);
        // how often a copy ordered from the GPU is looked for
        static constexpr auto POLL = std::chrono::milliseconds(5);

        static void run() {
            std::map<int, Capture::Frame> have;
            while (true) {
                std::vector<int> wanted, expired;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [] { return stopping || !widths.empty(); });
                    if (stopping) break;
                    auto now = std::chrono::steady_clock::now();
                    for (auto it = widths.begin(); it != widths.end();) {
                        if (now - it->second.asked > KEPT) {
                            have.erase(it->first);
                            expired.push_back(it->first);
                            it = widths.erase(it);
                        }
                        else {
                            wanted.push_back(it->first);
                            ++it;
                        }
                    }
                }
                if (wanted.empty()) {
                    Capture::dropScaled();
                    continue;
                }
                for (int width : expired) Capture::dropScaled(width);

                HWND game = Capture::findGameWindow();
                RECT client;
                if (game && Capture::getClientRectOnScreen(game, client)) {
                    for (int width : wanted) {
                        cv::Mat bgra;
                        if (!Capture::grabGameScaled(game, client, width, bgra, have[width])) continue;
                        std::lock_guard<std::mutex> lock(mutex);
                        auto made = widths.find(width);
                        if (made != widths.end()) {
                            made->second.bgra = bgra;
                            made->second.frame = have[width].number;
                        }
                    }
                }
                std::this_thread::sleep_for(POLL);
            }
            Capture::dropScaled();
        }

        static bool take(int width, cv::Mat& bgra, uint64_t& frame) {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return false;
            if (!thread.joinable()) thread = std::thread(run);
            Made& made = widths[width];
            made.asked = std::chrono::steady_clock::now();
            wake.notify_one();
            if (made.bgra.empty() || made.frame == frame) return false;
            bgra = made.bgra;
            frame = made.frame;
            return true;
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(Maker::mutex);
            Maker::stopping = true;
        }
        Maker::wake.notify_all();
        if (Maker::thread.joinable()) Maker::thread.join();
    }

    static GLuint uploadTexture(const cv::Mat& bgra, GLuint texture) {
        if (texture == 0) {
            glGenTextures(1, &texture);
            if (texture == 0) return 0;
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(bgra.step / 4));
        GLint w = 0, h = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        if (w == bgra.cols && h == bgra.rows) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bgra.cols, bgra.rows, GL_BGRA, GL_UNSIGNED_BYTE, bgra.data);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bgra.cols, bgra.rows, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra.data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        return texture;
    }

    std::function<bool(int, cv::Mat&, uint64_t&)> grab = Maker::take;
    std::function<GLuint(const cv::Mat&, GLuint)> upload = uploadTexture;
    std::function<void(GLuint)> release = [](GLuint texture) { glDeleteTextures(1, &texture); };
    std::function<double()> seconds = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    std::function<bool(Screens::Screen, uint64_t)> shownOn = Screens::shownOn;

    // a picture nobody asked for in this long is freed
    static constexpr double KEPT_SECONDS = 1.0;
    // how long a frame waits to be known as one of the screen
    static constexpr double WAIT_SECONDS = 0.5;
    // widths asked for are kept to this
    static constexpr int SMALLEST = 16, LARGEST = 7680;

    struct Entry {
        Screens::Screen screen;
        int asked = 0;
        Picture picture;
        // a frame grabbed, kept until the screen is known to be up on it
        cv::Mat waiting;
        uint64_t waitingFrame = 0;
        double waitingSince = 0.0;
        // the game frame last grabbed, and the turn it was last grabbed in
        uint64_t frame = 0;
        uint64_t turn = 0;
        double lastAsked = 0.0;
        // who asked, and when last
        std::map<const void*, double> askers;
    };

    static std::vector<Entry> entries;
    static std::unordered_set<GLuint> handedOut;
    static uint64_t turn = 1;

    static size_t heldBy(const void* asker, double now) {
        size_t held = 0;
        for (const Entry& entry : entries) {
            auto it = entry.askers.find(asker);
            if (it != entry.askers.end() && now - it->second <= KEPT_SECONDS) held++;
        }
        return held;
    }

    Picture get(Screens::Screen screen, int width, const Screens::Up& up, const void* asker, Refused* refused) {
        if (refused) *refused = Refused::None;
        if (!up[screen] || !Screens::pictured(screen)) return {};
        width = std::clamp(width, SMALLEST, LARGEST);
        double now = seconds();
        auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.screen == screen && e.asked == width; });
        bool mine = it != entries.end() && it->askers.count(asker) > 0;
        if (asker && !mine && heldBy(asker, now) >= MaxPerAsker) {
            if (refused) *refused = Refused::TooManyOwn;
            return {};
        }
        if (it == entries.end()) {
            if (entries.size() >= MaxPictures) {
                if (refused) *refused = Refused::TooMany;
                return {};
            }
            entries.push_back({ screen, width });
            it = entries.end() - 1;
        }
        it->lastAsked = now;
        if (asker) it->askers[asker] = now;

        if (it->turn != turn) {
            it->turn = turn;
            // A grabbed frame waits until the screen is known to be up on it,
            // no newer one is taken meanwhile: it would always be ahead of that
            // check. One that never is (it closed just then) is dropped.
            if (!it->waiting.empty() && now - it->waitingSince > WAIT_SECONDS) it->waiting.release();
            cv::Mat bgra;
            if (it->waiting.empty() && grab(width, bgra, it->frame) && !bgra.empty()) {
                it->waiting = bgra;
                it->waitingFrame = it->frame;
                it->waitingSince = now;
            }
            if (!it->waiting.empty() && shownOn(screen, it->waitingFrame)) {
                Screens::blackOut(it->waiting, screen);
                GLuint texture = upload(it->waiting, it->picture.texture);
                if (texture != 0) {
                    handedOut.insert(texture);
                    it->picture = { texture, it->waiting.cols, it->waiting.rows, it->waitingFrame };
                }
                it->waiting.release();
            }
        }
        // one from an earlier time the screen was up isn't given
        if (it->picture.texture == 0 || !shownOn(screen, it->picture.frame)) return {};
        return it->picture;
    }

    bool owns(GLuint texture) {
        return texture != 0 && handedOut.count(texture) > 0;
    }

    static void freeEntry(const Entry& entry) {
        if (entry.picture.texture == 0) return;
        handedOut.erase(entry.picture.texture);
        release(entry.picture.texture);
    }

    void nextTurn() {
        turn++;
        if (entries.empty()) return;
        double now = seconds();
        auto old = std::stable_partition(entries.begin(), entries.end(), [&](const Entry& e) { return now - e.lastAsked <= KEPT_SECONDS; });
        for (auto it = old; it != entries.end(); ++it) freeEntry(*it);
        entries.erase(old, entries.end());
        for (Entry& entry : entries) {
            for (auto it = entry.askers.begin(); it != entry.askers.end();) {
                if (now - it->second > KEPT_SECONDS) it = entry.askers.erase(it);
                else ++it;
            }
        }
    }

    size_t count() {
        return entries.size();
    }

    void clear() {
        for (const Entry& entry : entries) freeEntry(entry);
        entries.clear();
        handedOut.clear();
    }
}
