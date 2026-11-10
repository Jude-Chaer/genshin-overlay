#include "game_ui.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

// All numbers are in a 2000x1125 picture. The UI scales by the smaller of
// height / 1125 and width / 2000, the right side is anchored to the right
// edge and each piece to its own top, middle or bottom. Measured on
// screenshots of the big map at 3840x2160 and in a 1600x1200 window.
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

    // the UI's scale in a picture that big
    static double scaleOf(double width, double height) {
        return std::min(height / HEIGHT, width / WIDTH);
    }

    // The edge a piece keeps to top to bottom. Narrower than 16:9 the UI fits
    // the width and the pieces in the middle of a side stay in the middle.
    enum class Edge { Top, Middle, Bottom };
    static double toPixelsY(double height, double s, double y, Edge edge) {
        if (edge == Edge::Top) return y * s;
        if (edge == Edge::Bottom) return height - (HEIGHT - y) * s;
        return height / 2 + (y - HEIGHT / 2) * s;
    }

    // Where a part given in 2000x1125 units is in a picture that big
    static cv::Rect place(cv::Size size, double x0, double y0, double x1, double y1, Edge edge) {
        double s = scaleOf(size.width, size.height);
        int w = (int)std::lround(x1 - x0), h = (int)std::lround(y1 - y0);
        return { (int)std::floor(toPixels(size.width, s, x0)), (int)std::floor(toPixelsY(size.height, s, y0, edge)),
            (int)std::lround(w * s), (int)std::lround(h * s) };
    }

    // A part of the picture given in 2000x1125 units, scaled to that size,
    // so the numbers below work at any resolution. Black where the picture
    // has nothing.
    static cv::Mat cut(const Picture& picture, double x0, double y0, double x1, double y1, Edge edge) {
        double s = scaleOf(picture.size.width, picture.size.height);
        int w = (int)std::lround(x1 - x0), h = (int)std::lround(y1 - y0);
        cv::Rect want = place(picture.size, x0, y0, x1, y1, edge);
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
        return (int)std::lround(toPixels(size.width, scaleOf(size.width, size.height), look.edge));
    }

    static cv::Rect switcherArea(cv::Size size, const Panel& panel);

    // the strips, and the switcher where it is with no panel
    std::vector<cv::Rect> PanelParts(cv::Size size) {
        std::vector<cv::Rect> parts;
        for (const PanelLook& look : PANELS) {
            parts.push_back(around(size, place(size, look.edge + STRIP_FROM, 0, look.edge + STRIP_TO, HEIGHT, Edge::Middle), 1));
        }
        parts.push_back(around(size, switcherArea(size, {}), 2));
        return parts;
    }

    Panel ReadPanel(const Picture& frame) {
        Panel none;
        if (!readable(frame)) return none;
        const PanelLook* best = nullptr;
        double bestShare = 0.0;
        for (const PanelLook& look : PANELS) {
            cv::Mat strip = cut(frame, look.edge + STRIP_FROM, 0, look.edge + STRIP_TO, HEIGHT, Edge::Middle);
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
        if (bestShare < OPEN) {
            // A dark map underground can look like the region list. The
            // switcher read where it is with no panel means none is open.
            if (ReadSwitcher(frame, none).seen == Seen::Read) return none;
            return { Seen::Unsure, PanelKind::None, -1 };
        }
        return { Seen::Read, best->kind, panelLeft(frame.size, *best) };
    }

    // The switcher is a column of round buttons kept in the middle of the
    // height whatever their count. Its middle is 80.5 from the right edge, or
    // 63.5 left of an open panel. The region list hides it.
    static constexpr double MIDDLE = 564.0;
    static constexpr double FROM_RIGHT = 80.5, FROM_PANEL = 63.5;
    // the part cut out around it, as far as the buttons can go
    static constexpr double AREA_HALF = 40.0, AREA_TOP = 230.0, AREA_BOTTOM = 900.0;
    // the strip over the buttons' icons
    static constexpr int ICON_HALF_WIDTH = 14;
    // An icon's pixels have blue and green over BRIGHT. White ones have blue
    // about as high as red, cyan ones more than CYAN over it.
    static constexpr int BRIGHT = 150, CYAN = 80;
    // Rows with ROW_PIXELS white ones, or cyan ones, make a white or a cyan
    // shape, gaps of up to ROW_GAP rows inside it. The player's marker is
    // cyan, so it stays apart from a white icon it touches.
    static constexpr int ROW_PIXELS = 3, ROW_GAP = 2;
    // A floor's icon is about 15 rows and 210 pixels, cyan when lit. Above
    // ground has a white mountain, TALL rows or more.
    static constexpr int ICON_PIXELS = 150, ICON_ROWS = 10, TALL = 24;
    // The icons' middles are 56 to 86 apart, further round the mountain, and
    // spread evenly round ICONS_MIDDLE.
    static constexpr double ICONS_MIDDLE = 559.0, ICONS_OFF = 12.0, GAP_MIN = 56.0, GAP_MAX = 86.0;
    // Above ground lit, a cyan ring goes round the mountain: an arc of at
    // least ARC_PIXELS over it and one under it, within RING_REACH.
    static constexpr int ARC_PIXELS = 50, RING_REACH = 40;

    struct Shape {
        int top = 0, bottom = 0, pixels = 0;
        bool cyan = false;
        double middle() const { return (top + bottom) / 2.0; }
        bool isIcon() const { return pixels >= ICON_PIXELS && bottom - top >= ICON_ROWS; }
    };

    // the white shapes and the cyan ones in the strip, top to bottom
    static std::vector<Shape> shapes(const cv::Mat& area, double x) {
        int x0 = std::max(0, (int)std::floor(x - ICON_HALF_WIDTH + 0.5));
        int x1 = std::min(area.cols, x0 + 2 * ICON_HALF_WIDTH + 1);
        std::vector<int> counts[2] = { std::vector<int>(area.rows, 0), std::vector<int>(area.rows, 0) };
        for (int row = 0; row < area.rows; row++) {
            const cv::Vec3b* p = area.ptr<cv::Vec3b>(row);
            for (int col = x0; col < x1; col++) {
                int b = p[col][0], g = p[col][1], r = p[col][2];
                if (b <= BRIGHT || g <= BRIGHT) continue;
                counts[b - r > CYAN ? 1 : 0][row]++;
            }
        }
        std::vector<Shape> out;
        for (int cyan = 0; cyan < 2; cyan++) {
            const std::vector<int>& count = counts[cyan];
            auto filled = [&](int row) { return row < area.rows && count[row] >= ROW_PIXELS; };
            int row = 0;
            while (row < area.rows) {
                if (!filled(row)) {
                    row++;
                    continue;
                }
                Shape shape;
                shape.top = row;
                shape.cyan = cyan == 1;
                for (;;) {
                    shape.pixels += count[row];
                    row++;
                    bool goesOn = false;
                    for (int next = row; next <= row + ROW_GAP; next++) goesOn = goesOn || filled(next);
                    if (!goesOn) break;
                }
                shape.bottom = row;
                out.push_back(shape);
            }
        }
        std::sort(out.begin(), out.end(), [](const Shape& a, const Shape& b) { return a.top < b.top; });
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
        return panel.seen == Seen::Read ? toUnits(size.width, scaleOf(size.width, size.height), panel.left) - FROM_PANEL : WIDTH - FROM_RIGHT;
    }

    static cv::Rect switcherArea(cv::Size size, const Panel& panel) {
        double left = std::floor(switcherMiddle(size, panel) - AREA_HALF);
        return place(size, left, AREA_TOP, left + 2 * AREA_HALF, AREA_BOTTOM, Edge::Middle);
    }

    // left of each panel that doesn't hide it, PanelParts has it with none
    std::vector<cv::Rect> SwitcherParts(cv::Size size) {
        std::vector<cv::Rect> parts;
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
        cv::Mat area = cut(frame, left, AREA_TOP, left + 2 * AREA_HALF, AREA_BOTTOM, Edge::Middle);
        std::vector<Shape> all = shapes(area, middleX - left);
        std::vector<Shape> icons;
        for (const Shape& shape : all) {
            if (shape.isIcon()) icons.push_back(shape);
        }

        // The longest run of icons the right gap apart that sits evenly round
        // the middle, so a marker just past one end doesn't add one.
        double centre = ICONS_MIDDLE - AREA_TOP;
        size_t first = 0, last = 0;
        bool found = false;
        for (size_t i = 0; i < icons.size(); i++) {
            for (size_t j = i + 1; j < icons.size(); j++) {
                double gap = icons[j].middle() - icons[j - 1].middle();
                if (gap < GAP_MIN || gap > GAP_MAX) break;
                bool even = std::abs((icons[i].middle() + icons[j].middle()) / 2.0 - centre) <= ICONS_OFF;
                if (even && (!found || j - i > last - first)) {
                    first = i;
                    last = j;
                    found = true;
                }
            }
        }
        if (!found) return none;
        std::vector<Shape> buttons(icons.begin() + first, icons.begin() + last + 1);
        int count = (int)buttons.size();

        int ground = -1;
        for (int i = 0; i < count; i++) {
            if (buttons[i].cyan || buttons[i].bottom - buttons[i].top < TALL) continue;
            if (ground >= 0) return { Seen::Unsure, count, -1, -1 };
            ground = i;
        }
        if (ground < 0) return { Seen::Unsure, count, -1, -1 };

        int lit = -1, many = 0;
        for (int i = 0; i < count; i++) {
            if (!buttons[i].cyan) continue;
            lit = i;
            many++;
        }
        // the player's marker sits by a button on one side only
        const Shape& mountain = buttons[ground];
        int over = 0, under = 0;
        for (const Shape& shape : all) {
            if (!shape.cyan || shape.isIcon()) continue;
            if (shape.bottom <= mountain.top + ROW_GAP && mountain.top - shape.top <= RING_REACH) over += shape.pixels;
            if (shape.top >= mountain.bottom - ROW_GAP && shape.bottom - mountain.bottom <= RING_REACH) under += shape.pixels;
        }
        if (std::min(over, under) >= ARC_PIXELS) {
            lit = ground;
            many++;
        }
        if (many != 1) return { Seen::Unsure, count, -1, ground };
        return { Seen::Read, count, lit, ground };
    }

    // x kept to the right edge or the left, y0 and y1 each to its edge
    struct Box {
        double x0, y0, x1, y1;
        bool right;
        Edge top, bottom;
    };

    // with no panel open
    static constexpr Box ZOOM = { 0, 430, 90, 690, false, Edge::Middle, Edge::Middle };
    static constexpr Box MAP_UI[] = {
        { 0, 0, 370, 440, false, Edge::Top, Edge::Middle },          // event banner and list, longer with room
        ZOOM,                                                         // zoom slider
        { 0, 1015, 100, 1115, false, Edge::Bottom, Edge::Bottom },   // gear
        { 1030, 15, 2000, 85, true, Edge::Top, Edge::Top },          // resource bar
        { 1530, 1015, 2000, 1125, true, Edge::Bottom, Edge::Bottom }, // region name
    };
    // The layer switcher's box round its middle, as tall as with 7 floors. It
    // moves with a panel like the switcher does. With a panel open only the
    // zoom slider stays besides it.
    static constexpr double SWITCHER_HALF_WIDTH = 50.0, SWITCHER_HALF_HEIGHT = 282.0;

    static LONG rounded(double v) {
        return (LONG)std::lround(v);
    }

    static RECT toScreen(const Box& box, const RECT& game) {
        double width = game.right - game.left, height = game.bottom - game.top, s = scaleOf(width, height);
        double x0 = box.right ? toPixels(width, s, box.x0) : box.x0 * s;
        double x1 = box.right ? toPixels(width, s, box.x1) : box.x1 * s;
        double y0 = toPixelsY(height, s, box.y0, box.top), y1 = toPixelsY(height, s, box.y1, box.bottom);
        return { game.left + rounded(x0), game.top + rounded(y0), game.left + rounded(x1), game.top + rounded(y1) };
    }

    static RECT switcherBox(const RECT& game, const Panel& panel) {
        double middle = switcherMiddle(cv::Size(game.right - game.left, game.bottom - game.top), panel);
        Box box = { middle - SWITCHER_HALF_WIDTH, MIDDLE - SWITCHER_HALF_HEIGHT,
            middle + SWITCHER_HALF_WIDTH, MIDDLE + SWITCHER_HALF_HEIGHT, true, Edge::Middle, Edge::Middle };
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

    // the gap between the picker and the switcher's box
    static constexpr int BESIDE_GAP = 10;

    bool BesideSwitcher(const RECT& game, const Panel& panel, POINT& out) {
        if (panel.seen == Seen::Unsure || hidesSwitcher(panel)) return false;
        double height = game.bottom - game.top, s = scaleOf(game.right - game.left, height);
        LONG boxLeft = switcherBox(game, panel.left >= 0 ? panel : Panel{}).left;
        out = { boxLeft - rounded(BESIDE_GAP * s), game.top + rounded(toPixelsY(height, s, MIDDLE, Edge::Middle)) };
        return true;
    }

    bool SwitcherFloor(const Switcher& read, const std::vector<MapData::Floor>& floors, int mapId,
        int area, double x, double y, int& groupId, int& floorId) {
        if (read.seen != Seen::Read || read.count < 2 || read.lit < 0 || read.lit >= read.count) return false;
        if (read.ground < 0 || read.ground >= read.count) return false;
        if (read.lit == read.ground) {
            groupId = floorId = 0;
            return true;
        }
        // the floors on the lit one's side of above ground, and its place among them
        bool above = read.lit < read.ground;
        int side = above ? read.ground : read.count - 1 - read.ground;
        int at = above ? read.lit : read.lit - read.ground - 1;
        std::vector<int> own = area ? MapData::groupFloors(floors, mapId, area) : std::vector<int>();
        if (area && (int)own.size() == side) {
            groupId = area;
            floorId = own[at];
            return true;
        }
        // Or a smaller group inside SIFT's: with a small floor lit, SIFT can
        // match the big one round it. With no area, a group under x y: SIFT
        // can take a lit floor for the ground.
        int found = -1;
        std::vector<int> foundFloors, seen;
        for (const MapData::Floor& floor : floors) {
            if (floor.mapId != mapId || floor.groupId == area) continue;
            if (std::find(seen.begin(), seen.end(), floor.groupId) != seen.end()) continue;
            seen.push_back(floor.groupId);
            std::vector<int> inner = MapData::groupFloors(floors, mapId, floor.groupId);
            if ((int)inner.size() != side) continue;
            if (area && !MapData::groupInside(floors, mapId, floor.groupId, area)) continue;
            MapData::Box box;
            bool under = MapData::groupBox(floors, mapId, floor.groupId, box)
                && x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
            if (!area && !under) continue;
            // two fit, no telling which
            if (found >= 0) return false;
            found = floor.groupId;
            foundFloors = inner;
        }
        if (found < 0) return false;
        groupId = found;
        floorId = foundFloors[at];
        return true;
    }
}
