#include "test_map.hpp"
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
}
