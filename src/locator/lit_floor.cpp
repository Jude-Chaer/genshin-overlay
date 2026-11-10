#include "lit_floor.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MapLocator {

    // The screen is compared this wide. A lit floor's art and the screen under
    // it go together at about the same brightness (gain 0.7 to 1); a faded floor
    // stays under 0.45 and one that isn't drawn near 0.
    static constexpr int COMPARE_WIDTH = 480;
    static constexpr double MIN_GAIN = 0.6;
    static constexpr double MIN_CORRELATION = 0.3;
    static constexpr int MIN_PIXELS = 1000;
    static constexpr uchar DRAWN_ALPHA = 200;

    FloorArt MakeFloorArt(const cv::Mat& image, int groupId, int floorId, double left, double top, double right, double bottom) {
        FloorArt art;
        art.groupId = groupId;
        art.floorId = floorId;
        art.left = left;
        art.top = top;
        art.right = right;
        art.bottom = bottom;
        if (image.empty()) return art;

        cv::Mat gray, alpha;
        if (image.channels() == 4) {
            cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
            cv::extractChannel(image, alpha, 3);
        }
        else {
            if (image.channels() == 3) cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
            else gray = image.clone();
            alpha = cv::Mat(gray.size(), CV_8U, cv::Scalar(255));
        }
        double r = (std::min)(1.0, (double)FLOOR_ART_SIZE / (std::max)(gray.cols, gray.rows));
        if (r < 1.0) {
            cv::resize(gray, gray, cv::Size(), r, r, cv::INTER_AREA);
            cv::resize(alpha, alpha, gray.size(), 0, 0, cv::INTER_AREA);
        }
        art.gray = gray;
        art.alpha = alpha;
        return art;
    }

    int LitFloor(const cv::Mat& screenGray, const cv::Mat& mask, const ScreenView& view, const std::vector<const FloorArt*>& arts) {
        if (screenGray.empty() || arts.empty()) return -1;
        double q = (std::min)(1.0, (double)COMPARE_WIDTH / screenGray.cols);
        cv::Mat screen, uiMask;
        cv::resize(screenGray, screen, cv::Size(), q, q, cv::INTER_AREA);
        if (!mask.empty()) cv::resize(mask, uiMask, screen.size(), 0, 0, cv::INTER_NEAREST);
        double units = view.unitsPerPixel / q;

        int lit = -1;
        double litGain = 0.0;
        for (size_t i = 0; i < arts.size(); i++) {
            const FloorArt& art = *arts[i];
            if (art.gray.empty()) continue;
            double sx = (art.right - art.left) / art.gray.cols;
            double sy = (art.bottom - art.top) / art.gray.rows;
            cv::Mat toScreen = (cv::Mat_<double>(2, 3) << sx / units, 0, (art.left - view.left) / units,
                0, sy / units, (art.top - view.top) / units);
            cv::Mat alpha, gray;
            cv::warpAffine(art.alpha, alpha, toScreen, screen.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));
            cv::warpAffine(art.gray, gray, toScreen, screen.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));
            cv::Mat drawn = alpha > DRAWN_ALPHA;
            if (!uiMask.empty()) drawn &= uiMask;

            // a straight line from the art's brightness to the screen's
            double n = 0, sa = 0, ss = 0, saa = 0, sss = 0, sas = 0;
            for (int y = 0; y < screen.rows; y++) {
                const uchar* d = drawn.ptr<uchar>(y);
                const uchar* a = gray.ptr<uchar>(y);
                const uchar* s = screen.ptr<uchar>(y);
                for (int x = 0; x < screen.cols; x++) {
                    if (!d[x]) continue;
                    n++;
                    sa += a[x];
                    ss += s[x];
                    saa += (double)a[x] * a[x];
                    sss += (double)s[x] * s[x];
                    sas += (double)a[x] * s[x];
                }
            }
            if (n < MIN_PIXELS) continue;
            double varA = saa - sa * sa / n, varS = sss - ss * ss / n, cov = sas - sa * ss / n;
            if (varA <= 0.0 || varS <= 0.0) continue;
            double gain = cov / varA;
            double correlation = cov / std::sqrt(varA * varS);
            if (gain >= MIN_GAIN && correlation >= MIN_CORRELATION && gain > litGain) {
                lit = (int)i;
                litGain = gain;
            }
        }
        return lit;
    }

}
