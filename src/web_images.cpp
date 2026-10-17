#include "web_images.hpp"
#include "net.hpp"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <SOIL2/SOIL2.h>

namespace WebImages {
    static constexpr int WORKER_COUNT = 2;
    static constexpr int MAX_SIZE = 640;
    static constexpr int UPLOADS_PER_FRAME = 4;
    static constexpr uint64_t FRAMES_BEFORE_FREE = 300;

    struct Entry {
        Image image;
        uint64_t lastUsed = 0;
    };

    struct Finished {
        std::string url;
        cv::Mat image;  // empty if it failed
    };

    // main thread only
    static std::map<std::string, Entry> images;
    static std::set<std::string> pending;
    static uint64_t frame = 0;

    // shared with the workers
    static std::mutex mutex;
    static std::condition_variable wake;
    static std::deque<std::string> queue;
    static std::vector<Finished> finished;
    static bool stopping = false;
    static std::vector<std::thread> workers;

    static cv::Mat download(const std::string& url) {
        Net::Response response;
        if (!Net::get(url, response) || response.status != 200) return cv::Mat();
        std::vector<uchar> bytes(response.body.begin(), response.body.end());
        cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
        if (image.empty()) {
            // our OpenCV only reads png and webp, SOIL2 does the jpegs
            int width = 0, height = 0, channels = 0;
            unsigned char* pixels = SOIL_load_image_from_memory(bytes.data(), (int)bytes.size(), &width, &height, &channels, SOIL_LOAD_RGBA);
            if (!pixels) return image;
            cv::cvtColor(cv::Mat(height, width, CV_8UC4, pixels), image, cv::COLOR_RGBA2BGRA);
            SOIL_free_image_data(pixels);
        }
        if (image.channels() == 1) cv::cvtColor(image, image, cv::COLOR_GRAY2BGRA);
        else if (image.channels() == 3) cv::cvtColor(image, image, cv::COLOR_BGR2BGRA);
        if (image.depth() != CV_8U) image.convertTo(image, CV_8U, 1.0 / 256);

        int longest = (std::max)(image.cols, image.rows);
        if (longest > MAX_SIZE) {
            double scale = (double)MAX_SIZE / longest;
            cv::resize(image, image, cv::Size((std::max)(1, (int)(image.cols * scale)), (std::max)(1, (int)(image.rows * scale))), 0, 0, cv::INTER_AREA);
        }
        return image;
    }

    static void work() {
        for (;;) {
            std::string url;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [] { return stopping || !queue.empty(); });
                if (stopping) return;
                url = std::move(queue.front());
                queue.pop_front();
            }
            cv::Mat image = download(url);
            std::lock_guard<std::mutex> lock(mutex);
            finished.push_back({ std::move(url), std::move(image) });
        }
    }

    void Init() {
        stopping = false;
        for (int i = 0; i < WORKER_COUNT; i++) workers.emplace_back(work);
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto& worker : workers) worker.join();
        workers.clear();
        for (auto& [url, entry] : images) {
            if (entry.image.texture != 0) glDeleteTextures(1, &entry.image.texture);
        }
        images.clear();
    }

    Image Get(const std::string& url) {
        if (url.empty()) return Image();
        auto it = images.find(url);
        if (it != images.end()) {
            it->second.lastUsed = frame;
            return it->second.image;
        }
        if (pending.insert(url).second) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                queue.push_back(url);
            }
            wake.notify_one();
        }
        return Image();
    }

    static GLuint upload(const cv::Mat& image) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.cols, image.rows, 0, GL_BGRA, GL_UNSIGNED_BYTE, image.data);
        glBindTexture(GL_TEXTURE_2D, 0);
        return texture;
    }

    void EndFrame() {
        std::vector<Finished> done;
        {
            std::lock_guard<std::mutex> lock(mutex);
            size_t count = (std::min)(finished.size(), (size_t)UPLOADS_PER_FRAME);
            done.assign(std::make_move_iterator(finished.begin()), std::make_move_iterator(finished.begin() + count));
            finished.erase(finished.begin(), finished.begin() + count);
        }

        for (auto& result : done) {
            pending.erase(result.url);
            // a failed one stays empty until it's freed, then it's tried again
            Entry entry;
            entry.lastUsed = frame;
            if (!result.image.empty()) {
                entry.image = { upload(result.image), result.image.cols, result.image.rows };
            }
            images[result.url] = entry;
        }

        uint64_t now = ++frame;
        for (auto it = images.begin(); it != images.end();) {
            if (now - it->second.lastUsed > FRAMES_BEFORE_FREE) {
                if (it->second.image.texture != 0) glDeleteTextures(1, &it->second.image.texture);
                it = images.erase(it);
            }
            else {
                ++it;
            }
        }
    }
}
