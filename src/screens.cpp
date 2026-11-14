#include "screens.hpp"
#include "map_tracking.hpp"
#include "overlay.hpp"
#include <algorithm>
#include <cmath>

namespace Screens {
    bool Up::any() const {
        for (bool up : on) if (up) return true;
        return false;
    }

    Up now() {
        Up up;
        up.set(Screen::Map, MapTracking::IsMapOpen());
        up.set(Screen::Menu, Overlay::MenuOpen());
        return up;
    }

    bool pictured(Screen screen) {
        for (const Known& known : list) {
            if (known.screen == screen) return known.picture;
        }
        return false;
    }

    bool shownOn(Screen screen, uint64_t frame) {
        switch (screen) {
        case Screen::Map: return MapTracking::MapOpenOn(frame);
        default: return false;
        }
    }

    void blackOut(cv::Mat& picture, Screen screen) {
        if (picture.empty()) return;
        // narrower than 16:9 the game fits its UI to the width
        double s = std::min(picture.rows / 1125.0, picture.cols / 2000.0);
        for (const Hidden& part : hidden) {
            if (part.screen != screen) continue;
            int x0 = (int)std::floor(picture.cols - (2000.0 - part.x0) * s);
            int x1 = (int)std::ceil(picture.cols - (2000.0 - part.x1) * s);
            int y0 = (int)std::floor(picture.rows - (1125.0 - part.y0) * s);
            int y1 = (int)std::ceil(picture.rows - (1125.0 - part.y1) * s);
            cv::Rect box = cv::Rect(x0, y0, x1 - x0, y1 - y0) & cv::Rect(0, 0, picture.cols, picture.rows);
            if (box.area() > 0) picture(box).setTo(cv::Scalar(0, 0, 0, 255));
        }
    }
}
