#include "game_ui.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace GameUi {
    // In a 2000x1125 frame like the locator's UI_RECTS, the UI scales with the
    // screen height. Right ones are anchored to the right edge.
    struct Box {
        int x0, y0, x1, y1;
        bool right;
    };

    // with no panel open
    static constexpr Box MAP_UI[] = {
        { 0, 0, 370, 440, false },        // event banner and list
        { 0, 430, 90, 690, false },       // zoom slider
        { 0, 1015, 100, 1115, false },    // gear
        { 1030, 15, 2000, 85, true },     // resource bar
        { 1530, 1015, 2000, 1125, true }, // region name
        { 1870, 280, 1970, 845, true },   // layer switcher, as tall as with 7 floors
    };
    // With a panel open only the zoom slider stays, and the layer switcher
    // moves to the panel's left side.
    static constexpr Box ZOOM = { 0, 430, 90, 690, false };
    static constexpr int SWITCHER_TOP = 280, SWITCHER_BOTTOM = 845;
    static constexpr int SWITCHER_FROM = 100, SWITCHER_TO = 20;

    // rows looked at, from 8% to 92% of the height
    static constexpr int ROWS = 30;
    // gray levels between two pixels next to each other
    static constexpr int JUMP = 16;
    // Panels are wide: the region list starts at 1335, info panels at
    // 1485-1496. Further right it's the layer switcher's edge.
    static constexpr int FROM = 1150, TO = 1700;

    int FindPanel(const cv::Mat& wide) {
        if (wide.empty() || wide.type() != CV_8U || wide.rows < ROWS || wide.cols < 40) return -1;
        int ys[ROWS];
        for (int i = 0; i < ROWS; i++) ys[i] = (int)(wide.rows * (0.08 + 0.84 * i / (ROWS - 1)));
        int x1 = std::min(wide.cols * TO / 2000, wide.cols - 4);
        for (int x = wide.cols * FROM / 2000; x < x1; x++) {
            int jumps = 0;
            double sum = 0.0, squares = 0.0;
            for (int y : ys) {
                const uchar* row = wide.ptr<uchar>(y);
                if (std::abs(row[x + 1] - row[x]) > JUMP) jumps++;
                double side = (row[x + 2] + row[x + 3]) / 2.0;
                sum += side;
                squares += side * side;
            }
            double mean = sum / ROWS;
            double spread = std::sqrt(std::max(0.0, squares / ROWS - mean * mean));
            // Most rows jump, or half of them and the panel side is flat. Dark
            // sea next to the region list is about the panel's colour.
            if (jumps >= ROWS * 8 / 10 || (jumps >= ROWS * 4 / 10 && spread < 3.0)) return x + 1;
        }
        return -1;
    }

    static RECT toScreen(const Box& box, const RECT& game) {
        double width = game.right - game.left, s = (game.bottom - game.top) / 1125.0;
        double x0 = box.right ? width - (2000 - box.x0) * s : box.x0 * s;
        double x1 = box.right ? width - (2000 - box.x1) * s : box.x1 * s;
        return { game.left + (LONG)x0, game.top + (LONG)(box.y0 * s), game.left + (LONG)x1, game.top + (LONG)(box.y1 * s) };
    }

    std::vector<RECT> Boxes(const RECT& game, long panelLeft) {
        std::vector<RECT> boxes;
        if (panelLeft < 0) {
            for (const Box& box : MAP_UI) boxes.push_back(toScreen(box, game));
            return boxes;
        }
        double s = (game.bottom - game.top) / 1125.0;
        boxes.push_back(toScreen(ZOOM, game));
        boxes.push_back({ panelLeft - (LONG)(SWITCHER_FROM * s), game.top + (LONG)(SWITCHER_TOP * s),
            panelLeft - (LONG)(SWITCHER_TO * s), game.top + (LONG)(SWITCHER_BOTTOM * s) });
        boxes.push_back({ panelLeft, game.top, game.right, game.bottom });
        return boxes;
    }

    // The layer switcher is a pill of round buttons 64 apart, kept in the
    // middle of the height, growing both ways with more floors. Its middle is
    // 80 from the right edge, or 60 to the left of an open panel.
    static constexpr double SWITCHER_MIDDLE = 562.5, SWITCHER_STEP = 63.6;
    static constexpr int SWITCHER_RIGHT = 80, SWITCHER_PANEL = 60;
    // the strip grabbed: wide enough for the panel to be a little off
    static constexpr int STRIP_TOP = 250, STRIP_BOTTOM = 875, STRIP_HALF = 45;
    // A lit floor is a solid cyan icon about 255 big. Lit above ground is a
    // cyan ring about 67 wide around the top button. With the mouse on a
    // button there's a thinner ring around it too, about 58 wide. The little
    // person by the player's layer is cyan too, about 80.
    static constexpr int LIT_AREA = 150;
    static constexpr double SOLID = 0.35;
    static constexpr int TOP_RING = 63;
    // a button's icon, white or lit, near where one should be
    static constexpr int ICON_HALF_WIDTH = 20, ICON_HALF_HEIGHT = 16, ICON_AREA = 40;
    // how far the middle of what was read may be from the switcher's
    static constexpr double MIDDLE_OFF = 10.0;

    RECT SwitcherStrip(const RECT& game, long panelLeft) {
        double s = (game.bottom - game.top) / 1125.0;
        double middle = panelLeft < 0 ? game.right - SWITCHER_RIGHT * s : panelLeft - SWITCHER_PANEL * s;
        return { (LONG)(middle - STRIP_HALF * s), game.top + (LONG)(STRIP_TOP * s),
            (LONG)(middle + STRIP_HALF * s), game.top + (LONG)(STRIP_BOTTOM * s) };
    }

    Switcher ReadSwitcher(const cv::Mat& strip, int gameHeight) {
        Switcher none;
        if (strip.empty() || strip.type() != CV_8UC3 || gameHeight <= 0) return none;
        // in the 1125 high size, where the numbers above are
        double s = gameHeight / 1125.0;
        cv::Mat small;
        cv::Size size((int)std::lround(strip.cols / s), (int)std::lround(strip.rows / s));
        if (size.width < 2 * ICON_HALF_WIDTH || size.height < 2 * ICON_HALF_HEIGHT) return none;
        cv::resize(strip, small, size, 0, 0, s > 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);

        cv::Mat cyan(small.size(), CV_8U), bright(small.size(), CV_8U);
        for (int y = 0; y < small.rows; y++) {
            const cv::Vec3b* row = small.ptr<cv::Vec3b>(y);
            uchar* c = cyan.ptr<uchar>(y);
            uchar* b = bright.ptr<uchar>(y);
            for (int x = 0; x < small.cols; x++) {
                const cv::Vec3b& p = row[x];
                c[x] = p[0] > 200 && p[1] > 160 && p[2] < 160;
                b[x] = c[x] || (p[0] > 170 && p[1] > 170 && p[2] > 170);
            }
        }

        cv::Mat labels, stats, centroids;
        int blobs = cv::connectedComponentsWithStats(cyan, labels, stats, centroids, 8);
        // a lit floor first, else the top button's ring
        int lit = 0, ring = 0;
        for (int i = 1; i < blobs; i++) {
            int area = stats.at<int>(i, cv::CC_STAT_AREA);
            int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
            if (area < LIT_AREA) continue;
            if (area >= SOLID * w * h) {
                if (!lit || area > stats.at<int>(lit, cv::CC_STAT_AREA)) lit = i;
            }
            else if ((std::min)(w, h) >= TOP_RING) {
                if (!ring || area > stats.at<int>(ring, cv::CC_STAT_AREA)) ring = i;
            }
        }
        if (!lit) lit = ring;
        if (!lit) return none;
        double cx = centroids.at<double>(lit, 0), cy = centroids.at<double>(lit, 1);

        auto icon = [&](double y) {
            cv::Rect box((int)cx - ICON_HALF_WIDTH, (int)y - ICON_HALF_HEIGHT, 2 * ICON_HALF_WIDTH, 2 * ICON_HALF_HEIGHT);
            if (box.y < 0 || box.y + box.height > bright.rows) return false;
            box &= cv::Rect(0, 0, bright.cols, bright.rows);
            return cv::countNonZero(bright(box)) >= ICON_AREA;
        };
        int above = 0, below = 0;
        while (icon(cy - (above + 1) * SWITCHER_STEP)) above++;
        while (icon(cy + (below + 1) * SWITCHER_STEP)) below++;
        // a marker just past the end would make it lopsided
        double middle = STRIP_TOP + cy + (below - above) * SWITCHER_STEP / 2.0;
        if (std::abs(middle - SWITCHER_MIDDLE) > MIDDLE_OFF) return none;
        return { above + below + 1, above };
    }

    bool SwitcherLayer(const Switcher& read, const std::vector<MapData::Floor>& floors, int mapId,
        double x, double y, int preferGroup, int& groupId, int& floorId) {
        if (read.count < 2 || read.lit < 0 || read.lit >= read.count) return false;
        // The switcher lists all of a group's floors, or only the ones whose box
        // holds the middle of the map, the game does both. HoYoLAB's boxes are
        // only about right, so a box a little away counts if none holds the spot.
        for (double grow : { 0.0, 0.5 }) {
            std::vector<int> groups;
            int best = -1;
            double bestOff = 0.0;
            std::vector<int> bestFloors;
            for (const MapData::Floor& floor : floors) {
                if (floor.mapId != mapId) continue;
                if (std::find(groups.begin(), groups.end(), floor.groupId) != groups.end()) continue;
                groups.push_back(floor.groupId);
                std::vector<int> here, all;
                double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
                for (const MapData::Floor& other : floors) {
                    if (other.mapId != mapId || other.groupId != floor.groupId) continue;
                    all.push_back(other.floorId);
                    double x0 = (std::min)(other.left, other.right), x1 = (std::max)(other.left, other.right);
                    double y0 = (std::min)(other.top, other.bottom), y1 = (std::max)(other.top, other.bottom);
                    double padX = (x1 - x0) * grow, padY = (y1 - y0) * grow;
                    if (x < x0 - padX || x > x1 + padX || y < y0 - padY || y > y1 + padY) continue;
                    here.push_back(other.floorId);
                    left = (std::min)(left, x0);
                    right = (std::max)(right, x1);
                    top = (std::min)(top, y0);
                    bottom = (std::max)(bottom, y1);
                }
                if (here.empty()) continue;
                if ((int)all.size() + 1 == read.count) here = all;
                else if ((int)here.size() + 1 != read.count) continue;
                // Two groups can fit, their boxes overlap a lot. The one SIFT
                // found, or the one the spot is most in the middle of.
                double off = floor.groupId == preferGroup ? -1.0
                    : std::hypot((x - (left + right) / 2) / (right - left + 1e-9), (y - (top + bottom) / 2) / (bottom - top + 1e-9));
                if (best < 0 || off < bestOff) {
                    best = floor.groupId;
                    bestOff = off;
                    bestFloors = here;
                }
            }
            if (best < 0) continue;
            groupId = read.lit == 0 ? 0 : best;
            floorId = read.lit == 0 ? 0 : bestFloors[read.lit - 1];
            return true;
        }
        return false;
    }
}
