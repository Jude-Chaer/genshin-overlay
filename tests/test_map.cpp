#include "test_map.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace TestMap {
    cv::Mat make(int width, int height) {
        cv::Mat noise(height, width, CV_8U), map;
        cv::RNG random(1234);
        random.fill(noise, cv::RNG::UNIFORM, 0, 256);
        // blobs of a few pixels, like the details on the real map
        cv::GaussianBlur(noise, map, cv::Size(0, 0), 3.0);
        cv::normalize(map, map, 0, 255, cv::NORM_MINMAX);
        return map;
    }

    cv::Mat screen(const cv::Mat& map, double x, double y, double perPixel) {
        int w = GAME.right - GAME.left, h = GAME.bottom - GAME.top;
        // screen pixel to map pixel
        cv::Matx23d to(perPixel, 0.0, x - w / 2.0 * perPixel, 0.0, perPixel, y - h / 2.0 * perPixel);
        cv::Mat gray, bgr;
        cv::warpAffine(map, gray, to, cv::Size(w, h), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar(0));
        cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
        return bgr;
    }

    void save(const cv::Mat& map, const std::filesystem::path& folder, int mapId, int tile) {
        int cols = map.cols / tile, rows = map.rows / tile;
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder / "tiles");
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                std::vector<uchar> bytes;
                cv::imencode(".webp", map(cv::Rect(x * tile, y * tile, tile, tile)), bytes, { cv::IMWRITE_WEBP_QUALITY, 101 });
                std::ofstream file(folder / "tiles" / (std::to_string(x) + "_" + std::to_string(y) + ".webp"), std::ios::binary);
                file.write((const char*)bytes.data(), bytes.size());
            }
        }
        std::ofstream manifest(folder / "manifest.tsv");
        manifest << "grid\t" << mapId << "\t" << cols << "\t" << rows << "\t" << tile << "\t0\t0\ttiles\n";
    }
}
