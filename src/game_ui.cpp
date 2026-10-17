#include "game_ui.hpp"
#include <algorithm>
#include <cmath>

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
}
