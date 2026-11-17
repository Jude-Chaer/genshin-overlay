#include "web_images.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace WebImages {
    std::function<bool(const std::string&, Net::Response&, Net::Stopper*, const Net::Limits&)> fetch =
        [](const std::string& url, Net::Response& out, Net::Stopper* stopper, const Net::Limits& limits) {
            return Net::get(url, out, false, nullptr, stopper, limits);
        };
    std::function<GLuint(const Helpers::Pixels&)> make = [](const Helpers::Pixels& pixels) {
        int width = 0, height = 0;
        return Helpers::textureFromPixels(pixels, width, height);
    };
    std::function<void(GLuint)> release = [](GLuint texture) { glDeleteTextures(1, &texture); };
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };

    // One picture on its way. The thread writes only into its own fields and
    // sets finished last, the draw thread reads them once it's set.
    struct Download {
        const void* owner = nullptr;
        std::string url, function;
        Net::Response response;
        Helpers::Pixels pixels;
        bool ok = false;
        Net::Stopper stopper;
        std::atomic<bool> finished{ false };
        std::thread thread;

        ~Download() {
            stopper.stop();
            if (thread.joinable()) thread.join();
        }
    };

    struct Entry {
        State state = State::Loading;
        double handle = 0;
        GLuint texture = 0;
        int width = 0, height = 0;
        std::chrono::steady_clock::time_point lastUsed;
        std::chrono::steady_clock::time_point failedAt;
    };

    typedef std::pair<const void*, std::string> Key;

    // draw thread only
    static std::map<Key, Entry> entries;
    static std::map<double, Key> handles;
    static std::vector<std::unique_ptr<Download>> downloads;
    static double nextHandle = FirstHandle;

    void shrink(Helpers::Pixels& pixels) {
        int longest = (std::max)(pixels.width, pixels.height);
        if (longest <= MaxSide || pixels.rgba.empty()) return;
        double scale = (double)MaxSide / longest;
        int width = (std::max)(1, (int)(pixels.width * scale));
        int height = (std::max)(1, (int)(pixels.height * scale));
        cv::Mat from(pixels.height, pixels.width, CV_8UC4, pixels.rgba.data());
        cv::Mat to;
        cv::resize(from, to, cv::Size(width, height), 0, 0, cv::INTER_AREA);
        pixels.rgba.assign(to.data, to.data + (size_t)width * height * 4);
        pixels.width = width;
        pixels.height = height;
    }

    int downloading(const void* owner) {
        return (int)std::count_if(downloads.begin(), downloads.end(), [owner](const auto& each) { return each->owner == owner; });
    }

    size_t kept(const void* owner) {
        size_t count = 0;
        for (const auto& [key, entry] : entries) {
            if (key.first == owner) count++;
        }
        return count;
    }

    bool isHandle(double value) {
        return value >= FirstHandle;
    }

    static void drop(std::map<Key, Entry>::iterator it);

    Image get(const void* owner, const std::string& url, const Net::Limits& limits, const std::string& function) {
        Image image;
        if (!owner || url.empty()) return image;
        auto found = entries.find(Key(owner, url));
        if (found != entries.end() && found->second.state == State::Failed && now() - found->second.failedAt >= RetryFailed) {
            drop(found);
            entries.erase(found);
            found = entries.end();
        }
        if (found != entries.end()) {
            Entry& entry = found->second;
            entry.lastUsed = now();
            image.state = entry.state;
            image.handle = entry.handle;
            image.width = entry.width;
            image.height = entry.height;
            return image;
        }
        if (kept(owner) >= MaxKept) return image;
        image.state = State::Loading;
        if (downloading(owner) >= MaxWaiting) return image;

        entries[Key(owner, url)].lastUsed = now();
        auto download = std::make_unique<Download>();
        download->owner = owner;
        download->url = url;
        download->function = function;
        Download* at = download.get();
        download->thread = std::thread([at, limits]() {
            at->ok = fetch(at->url, at->response, &at->stopper, limits);
            if (at->ok && at->response.status >= 200 && at->response.status < 300 && Helpers::decodePicture(at->response.body, at->url, at->pixels)) {
                shrink(at->pixels);
            }
            else at->pixels = {};
            at->response.body.clear();
            at->finished = true;
        });
        downloads.push_back(std::move(download));
        return image;
    }

    GLuint texture(const void* owner, double handle) {
        auto found = handles.find(handle);
        if (found == handles.end() || found->second.first != owner) return 0;
        Entry& entry = entries[found->second];
        entry.lastUsed = now();
        return entry.texture;
    }

    static void drop(std::map<Key, Entry>::iterator it) {
        if (it->second.texture != 0) release(it->second.texture);
        if (it->second.handle != 0) handles.erase(it->second.handle);
    }

    void endFrame(const std::function<void(const Done&)>& done) {
        int made = 0;
        for (auto it = downloads.begin(); it != downloads.end() && made < UploadsPerFrame;) {
            Download& download = **it;
            if (!download.finished) {
                ++it;
                continue;
            }
            download.thread.join();
            auto found = entries.find(Key(download.owner, download.url));
            if (found != entries.end()) {
                Entry& entry = found->second;
                GLuint texture = download.pixels.rgba.empty() ? 0 : make(download.pixels);
                made++;
                if (texture != 0) {
                    entry.state = State::Ready;
                    entry.texture = texture;
                    entry.width = download.pixels.width;
                    entry.height = download.pixels.height;
                    entry.handle = nextHandle++;
                    handles[entry.handle] = found->first;
                }
                else {
                    entry.state = State::Failed;
                    entry.failedAt = now();
                }
            }
            if (done) {
                Done result;
                result.owner = download.owner;
                result.function = download.function;
                result.response = std::move(download.response);
                result.ok = download.ok;
                done(result);
            }
            it = downloads.erase(it);
        }

        auto time = now();
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.state != State::Loading && time - it->second.lastUsed > FreeAfter) {
                drop(it);
                it = entries.erase(it);
            }
            else ++it;
        }
    }

    void forget(const void* owner) {
        downloads.erase(std::remove_if(downloads.begin(), downloads.end(), [owner](const auto& each) { return each->owner == owner; }), downloads.end());
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->first.first == owner) {
                drop(it);
                it = entries.erase(it);
            }
            else ++it;
        }
    }

    void clear() {
        downloads.clear();
        for (auto it = entries.begin(); it != entries.end(); ++it) drop(it);
        entries.clear();
        handles.clear();
    }
}
