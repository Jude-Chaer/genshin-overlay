#include "game_ui.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

// All numbers are in a 2000x1125 picture, the UI scales with the height and
// the right side is anchored to the right edge. Measured on screenshots of
// the big map at 3840x2160.
namespace GameUi {
    static constexpr double HEIGHT = 1125.0, WIDTH = 2000.0;
    // smaller than this the game's UI is too small to read, a minimised window
    static constexpr int SMALLEST = 360;

    static bool readable(const Picture& picture) {
        if (picture.size.height < SMALLEST || picture.parts.empty() || picture.parts.size() != picture.at.size()) return false;
        for (const cv::Mat& part : picture.parts) {
            if (part.empty() || part.depth() != CV_8U || (part.channels() != 3 && part.channels() != 4)) return false;
        }
        return true;
    }

    Picture Whole(const cv::Mat& frame) {
        Picture picture;
        picture.size = frame.size();
        picture.parts.push_back(frame);
        picture.at.push_back({ 0, 0 });
        return picture;
    }

    // An x in 2000x1125 units, kept from the right edge, in the pixels of a
    // picture width wide at scale s. And back.
    static double toPixels(double width, double s, double x) {
        return width - (WIDTH - x) * s;
    }
    static double toUnits(double width, double s, double x) {
        return WIDTH - (width - x) / s;
    }

    // Where a part given in 2000x1125 units is in a picture that big
    static cv::Rect place(cv::Size size, double x0, double y0, double x1, double y1) {
        double s = size.height / HEIGHT;
        int w = (int)std::lround(x1 - x0), h = (int)std::lround(y1 - y0);
        return { (int)std::floor(toPixels(size.width, s, x0)), (int)std::floor(y0 * s),
            (int)std::lround(w * s), (int)std::lround(h * s) };
    }

    // A part of the picture given in 2000x1125 units, scaled to that size,
    // so the numbers below work at any resolution. Black where the picture
    // has nothing.
    static cv::Mat cut(const Picture& picture, double x0, double y0, double x1, double y1) {
        double s = picture.size.height / HEIGHT;
        int w = (int)std::lround(x1 - x0), h = (int)std::lround(y1 - y0);
        cv::Rect want = place(picture.size, x0, y0, x1, y1);
        cv::Mat bgr(want.size(), CV_8UC3, cv::Scalar::all(0));
        for (size_t i = 0; i < picture.parts.size(); i++) {
            const cv::Mat& part = picture.parts[i];
            cv::Rect have = want & cv::Rect(picture.at[i], part.size());
            if (have.area() <= 0) continue;
            cv::Mat from = part(have - picture.at[i]);
            cv::Mat target = bgr(have - want.tl());
            if (from.channels() == 4) cv::cvtColor(from, target, cv::COLOR_BGRA2BGR);
            else from.copyTo(target);
        }
        if (bgr.cols == w && bgr.rows == h) return bgr;
        cv::Mat out;
        cv::resize(bgr, out, cv::Size(w, h), 0, 0, s > 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
        return out;
    }

    // a part with a pixel or two round it for the rounding, kept on the picture
    static cv::Rect around(cv::Size size, const cv::Rect& rect, int by) {
        cv::Rect grown(rect.x - by, rect.y - by, rect.width + 2 * by, rect.height + 2 * by);
        return grown & cv::Rect(0, 0, size.width, size.height);
    }

    // Panels on the right: the region list, and the info panels (a point, a
    // waypoint, a custom pin). Each has a plain strip of its own colour right
    // of its left edge.
    // The region list covers the layer switcher, the info panels push it left.
    struct PanelLook {
        PanelKind kind;
        int edge;
        cv::Vec3d colour;
        double off;
        bool hidesSwitcher;
    };
    static const PanelLook PANELS[] = {
        { PanelKind::RegionList, 1338, { 62, 50, 42 }, 15, true },
        { PanelKind::Info, 1488, { 205, 216, 221 }, 30, false },
    };
    // the strip looked at, right of the edge
    static constexpr int STRIP_FROM = 2, STRIP_TO = 8;
    // rows looked at, from 8% to 92% of the height
    static constexpr int ROWS = 30;
    // how much a row's colours may vary across the strip
    static constexpr double FLAT = 6.0;
    // Share of rows in the panel's colour. An info panel with a picture on top
    // still fills 0.8, the map alone stays under 0.17.
    static constexpr double OPEN = 0.6, SHUT = 0.3;

    // where that panel starts in a picture that big
    static int panelLeft(cv::Size size, const PanelLook& look) {
        return (int)std::lround(toPixels(size.width, size.height / HEIGHT, look.edge));
    }

    std::vector<cv::Rect> PanelParts(cv::Size size) {
        std::vector<cv::Rect> parts;
        for (const PanelLook& look : PANELS) {
            parts.push_back(around(size, place(size, look.edge + STRIP_FROM, 0, look.edge + STRIP_TO, HEIGHT), 1));
        }
        return parts;
    }

    Panel ReadPanel(const Picture& frame) {
        Panel none;
        if (!readable(frame)) return none;
        const PanelLook* best = nullptr;
        double bestShare = 0.0;
        for (const PanelLook& look : PANELS) {
            cv::Mat strip = cut(frame, look.edge + STRIP_FROM, 0, look.edge + STRIP_TO, HEIGHT);
            int matching = 0;
            for (int i = 0; i < ROWS; i++) {
                int y = (int)(HEIGHT * (0.08 + 0.84 * i / (ROWS - 1)));
                cv::Scalar mean, spread;
                cv::meanStdDev(strip.row(y), mean, spread);
                bool flat = true, inColour = true;
                for (int c = 0; c < 3; c++) {
                    flat = flat && spread[c] < FLAT;
                    inColour = inColour && std::abs(mean[c] - look.colour[c]) < look.off;
                }
                if (flat && inColour) matching++;
            }
            double share = (double)matching / ROWS;
            if (share > bestShare) {
                best = &look;
                bestShare = share;
            }
        }
        if (!best || bestShare <= SHUT) return none;
        if (bestShare < OPEN) return { Seen::Unsure, PanelKind::None, -1 };
        return { Seen::Read, best->kind, panelLeft(frame.size, *best) };
    }

    // The switcher is a column of round buttons 63.7 apart, kept in the
    // middle of the height whatever their count. Its middle is 80.5 from the
    // right edge, or 63.5 left of an open panel. The region list hides it.
    static constexpr double MIDDLE = 564.0, STEP = 63.7;
    static constexpr double FROM_RIGHT = 80.5, FROM_PANEL = 63.5;
    // the part cut out around it, as far as the buttons can go
    static constexpr double AREA_HALF = 40.0, AREA_TOP = 230.0, AREA_BOTTOM = 900.0;
    // the box over a button's icon
    static constexpr int ICON_HALF_WIDTH = 14, ICON_HALF_HEIGHT = 10;
    // An icon's pixels have blue and green over BRIGHT. White ones have blue
    // about as high as red, cyan ones more than CYAN over it.
    static constexpr int BRIGHT = 150, CYAN = 80;
    // An icon is about 210 of those pixels, a lit floor's all cyan. With
    // nothing lit underground, a cyan ring about 68 wide goes round the top
    // button. The mouse puts a thinner ring round a button, outside the icon.
    static constexpr int ICON_PIXELS = 80, RING_PIXELS = 150;
    static constexpr int RING_OUT = 36, RING_IN = 24;
    // buttons looked for each way from the middle
    static constexpr int MOST = 5;

    struct Spot {
        int icon = 0, cyan = 0;
    };

    static Spot spot(const cv::Mat& area, double x, double y, int halfWidth, int halfHeight) {
        Spot out;
        int x0 = (int)std::floor(x - halfWidth + 0.5), y0 = (int)std::floor(y - halfHeight + 0.5);
        cv::Rect box = cv::Rect(x0, y0, 2 * halfWidth + 1, 2 * halfHeight + 1) & cv::Rect(0, 0, area.cols, area.rows);
        for (int row = box.y; row < box.y + box.height; row++) {
            const cv::Vec3b* p = area.ptr<cv::Vec3b>(row);
            for (int col = box.x; col < box.x + box.width; col++) {
                int b = p[col][0], g = p[col][1], r = p[col][2];
                if (b <= BRIGHT || g <= BRIGHT) continue;
                out.icon++;
                if (b - r > CYAN) out.cyan++;
            }
        }
        return out;
    }

    static bool hidesSwitcher(const Panel& panel) {
        for (const PanelLook& look : PANELS) {
            if (look.kind == panel.kind) return panel.seen == Seen::Read && look.hidesSwitcher;
        }
        return false;
    }

    // the switcher's middle in 2000x1125 units, with that panel open
    static double switcherMiddle(cv::Size size, const Panel& panel) {
        return panel.seen == Seen::Read ? toUnits(size.width, size.height / HEIGHT, panel.left) - FROM_PANEL : WIDTH - FROM_RIGHT;
    }

    static cv::Rect switcherArea(cv::Size size, const Panel& panel) {
        double left = std::floor(switcherMiddle(size, panel) - AREA_HALF);
        return place(size, left, AREA_TOP, left + 2 * AREA_HALF, AREA_BOTTOM);
    }

    // where it is with no panel, and left of each panel that doesn't hide it
    std::vector<cv::Rect> SwitcherParts(cv::Size size) {
        std::vector<cv::Rect> parts = { around(size, switcherArea(size, {}), 2) };
        for (const PanelLook& look : PANELS) {
            if (look.hidesSwitcher) continue;
            Panel open = { Seen::Read, look.kind, panelLeft(size, look) };
            parts.push_back(around(size, switcherArea(size, open), 2));
        }
        return parts;
    }

    Switcher ReadSwitcher(const Picture& frame, const Panel& panel) {
        Switcher none;
        if (!readable(frame)) return none;
        if (panel.seen == Seen::Unsure) return { Seen::Unsure, 0, -1 };
        if (hidesSwitcher(panel)) return { Seen::Unsure, 0, -1 };

        double middleX = switcherMiddle(frame.size, panel);
        double left = std::floor(middleX - AREA_HALF);
        cv::Mat area = cut(frame, left, AREA_TOP, left + 2 * AREA_HALF, AREA_BOTTOM);
        double x = middleX - left;
        auto at = [&](double steps) { return MIDDLE - AREA_TOP + steps * STEP; };
        auto icon = [&](double steps) { return spot(area, x, at(steps), ICON_HALF_WIDTH, ICON_HALF_HEIGHT).icon >= ICON_PIXELS; };

        // An odd count has a button on the middle, an even one half a step
        // off. Buttons count only in pairs round the middle, so a marker just
        // past one end doesn't add one.
        int counts[2] = { 0, 0 };
        for (int even = 0; even < 2; even++) {
            int pairs = 0;
            while (pairs < MOST && icon(pairs + even * 0.5) && icon(-(pairs + even * 0.5))) pairs++;
            counts[even] = even ? 2 * pairs : (pairs ? 2 * pairs - 1 : 0);
        }
        bool odd = counts[0] >= 2, even = counts[1] >= 2;
        if (odd && even) return { Seen::Unsure, 0, -1 };
        if (!odd && !even) return none;
        int count = odd ? counts[0] : counts[1];

        auto button = [&](int i) { return i - (count - 1) / 2.0; };
        int lit = -1;
        for (int i = 1; i < count; i++) {
            Spot here = spot(area, x, at(button(i)), ICON_HALF_WIDTH, ICON_HALF_HEIGHT);
            if (here.cyan < ICON_PIXELS || here.cyan * 2 < here.icon) continue;
            if (lit >= 0) return { Seen::Unsure, count, -1 };
            lit = i;
        }
        if (lit < 0) {
            double top = at(button(0));
            int ring = spot(area, x, top, RING_OUT, RING_OUT).cyan - spot(area, x, top, RING_IN, RING_IN).cyan;
            if (ring < RING_PIXELS) return { Seen::Unsure, count, -1 };
            lit = 0;
        }
        return { Seen::Read, count, lit };
    }

    struct Box {
        double x0, y0, x1, y1;
        bool right;
    };

    // with no panel open
    static constexpr Box ZOOM = { 0, 430, 90, 690, false };
    static constexpr Box MAP_UI[] = {
        { 0, 0, 370, 440, false },        // event banner and list
        ZOOM,                             // zoom slider
        { 0, 1015, 100, 1115, false },    // gear
        { 1030, 15, 2000, 85, true },     // resource bar
        { 1530, 1015, 2000, 1125, true }, // region name
    };
    // The layer switcher's box round its middle, as tall as with 7 floors. It
    // moves with a panel like the switcher does. With a panel open only the
    // zoom slider stays besides it.
    static constexpr double SWITCHER_HALF_WIDTH = 50.0, SWITCHER_HALF_HEIGHT = 282.0;

    static LONG rounded(double v) {
        return (LONG)std::lround(v);
    }

    static RECT toScreen(const Box& box, const RECT& game) {
        double width = game.right - game.left, s = (game.bottom - game.top) / HEIGHT;
        double x0 = box.right ? toPixels(width, s, box.x0) : box.x0 * s;
        double x1 = box.right ? toPixels(width, s, box.x1) : box.x1 * s;
        return { game.left + rounded(x0), game.top + rounded(box.y0 * s), game.left + rounded(x1), game.top + rounded(box.y1 * s) };
    }

    static RECT switcherBox(const RECT& game, const Panel& panel) {
        double middle = switcherMiddle(cv::Size(game.right - game.left, game.bottom - game.top), panel);
        Box box = { middle - SWITCHER_HALF_WIDTH, MIDDLE - SWITCHER_HALF_HEIGHT,
            middle + SWITCHER_HALF_WIDTH, MIDDLE + SWITCHER_HALF_HEIGHT, true };
        return toScreen(box, game);
    }

    std::vector<RECT> Boxes(const RECT& game, const Panel& panel) {
        std::vector<RECT> boxes;
        // not sure there's one: all the boxes, they're generous anyway
        if (panel.seen != Seen::Read || panel.left < 0) {
            for (const Box& box : MAP_UI) boxes.push_back(toScreen(box, game));
            boxes.push_back(switcherBox(game, {}));
            return boxes;
        }
        boxes.push_back(toScreen(ZOOM, game));
        if (!hidesSwitcher(panel)) boxes.push_back(switcherBox(game, panel));
        boxes.push_back({ game.left + panel.left, game.top, game.right, game.bottom });
        return boxes;
    }

    bool SwitcherFloor(const Switcher& read, const std::vector<MapData::Floor>& floors, int mapId,
        int area, int& groupId, int& floorId) {
        if (read.seen != Seen::Read || read.count < 2 || read.lit < 0 || read.lit >= read.count) return false;
        if (read.lit == 0) {
            groupId = floorId = 0;
            return true;
        }
        if (area == 0) return false;
        std::vector<int> own = MapData::groupFloors(floors, mapId, area);
        if (!own.empty() && (int)own.size() + 1 == read.count) {
            groupId = area;
            floorId = own[read.lit - 1];
            return true;
        }
        // Or a smaller group inside SIFT's: with a small floor lit, SIFT can
        // match the big one round it.
        int found = -1;
        std::vector<int> foundFloors, seen;
        for (const MapData::Floor& floor : floors) {
            if (floor.mapId != mapId || floor.groupId == area) continue;
            if (std::find(seen.begin(), seen.end(), floor.groupId) != seen.end()) continue;
            seen.push_back(floor.groupId);
            std::vector<int> inner = MapData::groupFloors(floors, mapId, floor.groupId);
            if ((int)inner.size() + 1 != read.count || !MapData::groupInside(floors, mapId, floor.groupId, area)) continue;
            // two fit, no telling which
            if (found >= 0) return false;
            found = floor.groupId;
            foundFloors = inner;
        }
        if (found < 0) return false;
        groupId = found;
        floorId = foundFloors[read.lit - 1];
        return true;
    }
}
