#include "minimap.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "capture.hpp"
#include "map_data.hpp"
#include "map_tracking.hpp"
#include "minimap_corner.hpp"

namespace MiniMap {
    // Where the minimap is in a 1080 high game: the square around its circle.
    // It grows with the game's height.
    static constexpr double LEFT = 64.0, TOP = 21.0, SIDE = 208.0, HEIGHT = 1080.0;
    // The minimap is searched this many compared pixels each way around the
    // last spot, WIDE when that didn't find it.
    static constexpr int REACH = 16, WIDE = 64;
    // It's the minimap on the tiles when they look this much alike (-1 to 1).
    // The game's art isn't quite HoYoLAB's, the right place can get as little as 0.45.
    static constexpr double FOUND = 0.35;
    // A place only counts when it beats everything further than AWAY pixels
    // from it by SHARP. One road or border over open sea fits anywhere along
    // itself.
    static constexpr int AWAY = 6;
    static constexpr double SHARP = 0.1;
    // Zooms tried when none of the known ones fit, each ZOOM_STEP times the
    // one before. A zoom within SAME of a known one is that one.
    static constexpr double ZOOM_FIRST = 0.5, ZOOM_LAST = 5.0, ZOOM_STEP = 1.05, SAME = 0.04;
    // the N at the top of the minimap is there when that spot looks this much like one
    static constexpr double NORTH = 0.45;
    // Only the details are compared, what's left after taking away the picture
    // blurred this much (a part of its side). The plain pictures look alike
    // on any menu that gets lighter to one side.
    static constexpr double BLUR = 1.0 / 26.0;
    // Details stronger than this many gray levels are cut off there. The
    // tiles have thick borders and a dark edge of the map the game draws
    // faintly or not at all, uncut they outweigh everything else.
    static constexpr int CUT = 10;
    // a picture with less detail than this is flat (sea, a black screen), nothing to compare
    static constexpr double FLAT = 1.0;
    // The tiles need at least this much of the minimap's detail. Open sea has
    // next to none, and what little it has fits anything.
    static constexpr double DETAIL = 0.5;
    // tiles are shrunk by this before they're drawn at the minimap's size
    static constexpr int SHRINK = 2;
    // decoded tiles kept in memory, 16 KB each
    static constexpr size_t TILES_KEPT = 40;
    // spots tried from the whole map search, best first
    static constexpr int TRIES = 8;
    // how well Paimon's corner has to fit: 0.93 and up on minimaps, 0.43 at most on anything else
    static constexpr double CORNER = 0.7;
    // a whole map is searched shrunk by this
    static constexpr int SMALL = 8;
    // Floor pictures are kept shrunk by this for searching, 6 MB for all of
    // them. Less than SMALL, they're searched at zooms where shrunk by 8
    // leaves too little of the minimap.
    static constexpr int FLOOR_SMALL = 4;
    // The zoom steps of searching the floors without knowing the zoom. That
    // search is a few seconds of work, so FLOORS_EVERY seconds apart.
    static constexpr double ZOOM_ROUGH = 1.1;
    static constexpr double FLOORS_EVERY = 30.0;
    // While it's followed, the other layers at the spot get a look this many
    // seconds apart. Diving, the surface can keep passing for the minimap.
    static constexpr double LAYERS_EVERY = 1.0;
    static constexpr double LAYER_DROP = 0.08;
    // Looking through every floor, or trying every zoom on another layer, is
    // a lot of chances to fit by accident, so those have to fit this well.
    // The real ones are 0.6 and up.
    static constexpr double FOUND_ANY = 0.5;
    // floor pictures kept in memory for following, up to 2.6 MB each
    static constexpr size_t FLOORS_KEPT = 3;
    // Floor pictures that aren't on disk are downloaded this many at a time
    // while the minimap is on screen and not found, FETCH_EVERY seconds apart.
    static constexpr int FETCH = 20;
    static constexpr double FETCH_EVERY = 2.0;

    // the minimap is looked at this often
    static constexpr auto EVERY = std::chrono::milliseconds(50);
    // GetPlayer wasn't called for this long: nobody needs it, stop looking
    static constexpr double ASKED_FOR = 2.0;
    // Seconds between two whole map searches while the minimap isn't found.
    // Doubles every time up to SEARCH_LAST, a menu or a cutscene can stay for long.
    static constexpr double SEARCH_FIRST = 1.0, SEARCH_LAST = 10.0;
    // From this zoom on the area the big map showed is searched drawn from
    // the tiles, the minimap AREA_MINI pixels wide, if that picture is
    // AREA_SIDE pixels a side at most.
    static constexpr double AREA_ZOOM = 1.5;
    static constexpr int AREA_MINI = 43, AREA_SIDE = 3000;

    using MapData::Grid;
    using MapData::Floor;

    static std::map<int, Grid> grids;
    // Underground and under water the minimap shows a floor's picture
    // instead of the tiles.
    static std::vector<Floor> floors;
    // each one whole, shrunk by FLOOR_SMALL, read when it's first searched
    static std::vector<cv::Mat> floorSmalls;
    // floors by their place in that list, shrunk by SHRINK
    static std::map<int, cv::Mat> floorPictures;
    static std::deque<int> floorOrder;
    static std::filesystem::path folder;
    static std::thread worker;
    static std::mutex mutex;
    static std::condition_variable wake;
    // guarded by mutex
    static bool stopping = false;
    static Spot player;
    // when GetPlayer was last called, steady_clock seconds
    static std::atomic<double> askedAt{ -1000.0 };

    // The app only downloads the tiles the big map shows, so the ones
    // missing here get fetched.
    static MapData::Tiles tiles(TILES_KEPT, true);

    static double now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // A whole map shrunk by SMALL, for searching all of it. Made from the
    // tiles the first time and kept next to them, Teyvat takes a few seconds.
    static cv::Mat wholeMap(int mapId, const Grid& grid) {
        std::filesystem::path path = folder / (L"small_" + grid.dir.filename().wstring() + L".png");
        cv::Mat whole = MapData::readGray(path);
        // a black one was made before the tiles were there
        if (!whole.empty() && cv::countNonZero(whole) > 0) return whole;
        static std::set<int> made;
        if (!made.insert(mapId).second) return whole;

        int size = grid.tileSize / SMALL;
        whole = cv::Mat(grid.rows * size, grid.cols * size, CV_8U, cv::Scalar(0));
        for (int y = 0; y < grid.rows; ++y) {
            for (int x = 0; x < grid.cols; ++x) {
                cv::Mat image = MapData::readGray(MapData::tilePath(grid, x, y));
                if (image.empty()) continue;
                cv::resize(image, whole(cv::Rect(x * size, y * size, size, size)), cv::Size(size, size), 0, 0, cv::INTER_AREA);
            }
        }
        std::vector<uchar> bytes;
        if (cv::imencode(".png", whole, bytes)) {
            std::ofstream file(path, std::ios::binary);
            file.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        }
        return whole;
    }

    // one floor's picture shrunk by SHRINK, empty while it isn't on disk
    static const cv::Mat& floorPicture(int index) {
        auto it = floorPictures.find(index);
        if (it != floorPictures.end()) return it->second;

        static const cv::Mat nothing;
        cv::Mat image = MapData::readGray(floors[index].path, SHRINK);
        if (image.empty()) return nothing;

        if (floorOrder.size() >= FLOORS_KEPT) {
            floorPictures.erase(floorOrder.front());
            floorOrder.pop_front();
        }
        floorOrder.push_back(index);
        return floorPictures[index] = image;
    }

    // which floor of the list a spot is on, -1 on the surface
    static int floorOf(const Spot& spot) {
        if (spot.floorId == 0) return -1;
        for (size_t i = 0; i < floors.size(); ++i) {
            if (floors[i].mapId == spot.mapId && floors[i].groupId == spot.groupId && floors[i].floorId == spot.floorId) return (int)i;
        }
        return -1;
    }

    // how far a spot is from a floor's picture in map units, 0 on it
    static double away(const Floor& floor, int mapId, double lat, double lng) {
        if (floor.mapId != mapId) return 1e9;
        double x = std::max({ floor.left - lng, lng - floor.right, 0.0 });
        double y = std::max({ floor.top - lat, lat - floor.bottom, 0.0 });
        return std::hypot(x, y);
    }

    // What counts of an n x n minimap: the circle without its rim, where the
    // game fades it out and puts icons, and without the middle, where the
    // player's arrow is.
    static cv::Mat ring(int n) {
        cv::Mat mask(n, n, CV_8U, cv::Scalar(0));
        cv::Point middle(n / 2, n / 2);
        cv::circle(mask, middle, (int)(n * 0.43), cv::Scalar(255), cv::FILLED);
        cv::circle(mask, middle, (int)(n * 0.125), cv::Scalar(0), cv::FILLED);
        return mask;
    }

    // Where in picture the minimap fits best and how well, both as details.
    // Flat spots give numbers that mean nothing, they count as no fit.
    //
    // Exact leaves what's outside the mask out of it. Otherwise the minimap is
    // painted flat there: nearly 3 times faster, but right and wrong places
    // score closer together.
    static cv::Mat fit(const cv::Mat& picture, const cv::Mat& mini, const cv::Mat& mask, bool exact) {
        cv::Mat result;
        if (exact) {
            cv::matchTemplate(picture, mini, result, cv::TM_CCOEFF_NORMED, mask);
        }
        else if (!mask.empty()) {
            cv::Mat masked(mini.size(), CV_8U, cv::Scalar(128));
            mini.copyTo(masked, mask);
            cv::matchTemplate(picture, masked, result, cv::TM_CCOEFF_NORMED);
        }
        else cv::matchTemplate(picture, mini, result, cv::TM_CCOEFF_NORMED);
        cv::patchNaNs(result, 0.0);
        result.setTo(0.0, (result < -1.0) | (result > 1.0));
        return result;
    }

    // the details of a picture that's compared side pixels wide, around gray 128
    static cv::Mat details(const cv::Mat& gray, int side) {
        cv::Mat blurred, out;
        cv::GaussianBlur(gray, blurred, cv::Size(0, 0), side * BLUR);
        cv::addWeighted(gray, 1.0, blurred, -1.0, 128.0, out);
        cv::min(out, 128 + CUT, out);
        cv::max(out, 128 - CUT, out);
        return out;
    }

    static double contrast(const cv::Mat& gray, const cv::Mat& mask) {
        cv::Scalar mean, deviation;
        cv::meanStdDev(gray, mean, deviation, mask);
        return deviation[0];
    }

    // how far the top of a peak is from its middle sample, from the two next to it
    static double top(float before, float peak, float after) {
        double bend = before - 2.0 * peak + after;
        return bend < 0.0 ? 0.5 * (before - after) / bend : 0.0;
    }

    // the N the game draws at the top of the minimap, about the size it has there
    static cv::Mat north() {
        cv::Mat big(88, 96, CV_8U, cv::Scalar(90)), letter;
        int below = 0;
        cv::Size text = cv::getTextSize("N", cv::FONT_HERSHEY_SIMPLEX, 3.0, 5, &below);
        cv::putText(big, "N", cv::Point((big.cols - text.width) / 2, (big.rows + text.height) / 2), cv::FONT_HERSHEY_SIMPLEX, 3.0, cv::Scalar(200), 5, cv::LINE_AA);
        cv::resize(big, letter, cv::Size(12, 11), 0, 0, cv::INTER_AREA);
        return letter;
    }

    // Is there a minimap in this picture at all? Anything else that's ever
    // in that corner has no N at the top and no Paimon next to it. One of
    // the two is enough: a marker can sit on the N, and Paimon isn't there
    // in co-op.
    static bool shown(const cv::Mat& mini) {
        static const cv::Mat letter = north();
        cv::Mat result = fit(mini(cv::Rect(42, 1, 20, 15)), letter, cv::Mat(), false);
        double best = 0.0;
        cv::minMaxLoc(result, nullptr, &best);
        if (best >= NORTH) return true;

        // a pixel of slack each way
        static const cv::Mat paimon(CORNER_HEIGHT, CORNER_WIDTH, CV_8U, (void*)CORNER_PIXELS);
        cv::Mat around;
        cv::copyMakeBorder(mini(cv::Rect(0, 0, CORNER_WIDTH + 1, CORNER_HEIGHT + 1)), around, 1, 0, 1, 0, cv::BORDER_REPLICATE);
        cv::matchTemplate(around, paimon, result, cv::TM_CCOEFF_NORMED);
        cv::minMaxLoc(result, nullptr, &best);
        return best >= CORNER;
    }

    RECT rect(const RECT& game) {
        double scale = (game.bottom - game.top) / HEIGHT;
        RECT out;
        out.left = game.left + (LONG)std::lround(LEFT * scale);
        out.top = game.top + (LONG)std::lround(TOP * scale);
        out.right = out.left + (LONG)std::lround(SIDE * scale);
        out.bottom = out.top + (LONG)std::lround(SIDE * scale);
        return out;
    }

    cv::Mat shrink(const cv::Mat& grab) {
        cv::Mat gray, mini;
        if (grab.channels() == 4) cv::cvtColor(grab, gray, cv::COLOR_BGRA2GRAY);
        else if (grab.channels() == 3) cv::cvtColor(grab, gray, cv::COLOR_BGR2GRAY);
        else gray = grab;
        cv::resize(gray, mini, cv::Size(SIZE, SIZE), 0, 0, cv::INTER_AREA);
        return mini;
    }

    void Load(const std::filesystem::path& dataFolder) {
        folder = dataFolder;
        tiles.Clear();
        floorPictures.clear();
        floorOrder.clear();

        MapData::Manifest manifest = MapData::readManifest(dataFolder);
        grids = manifest.grids;
        floors.clear();
        for (const Floor& floor : manifest.floors) {
            if (floor.right > floor.left && floor.bottom > floor.top) floors.push_back(floor);
        }
        floorSmalls.assign(floors.size(), cv::Mat());
    }

    // A floor's picture around a spot, side pixels each way with perPixel map
    // units to the pixel. Empty while it isn't on disk.
    static cv::Mat drawFloor(int index, double lat, double lng, int side, double perPixel) {
        const cv::Mat& image = floorPicture(index);
        if (image.empty()) return {};
        const Floor& floor = floors[index];
        // map units to one pixel of the picture as it's kept
        double kept = (floor.right - floor.left) / image.cols;
        double a = perPixel / kept;
        double bx = (lng - floor.left + (0.5 - side / 2.0) * perPixel) / kept - 0.5;
        double by = (lat - floor.top + (0.5 - side / 2.0) * perPixel) / kept - 0.5;
        cv::Mat drawn;
        cv::Matx23d to(a, 0.0, bx, 0.0, a, by);
        cv::warpAffine(image, drawn, to, cv::Size(side, side), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar(0));
        return drawn;
    }

    // Looks for the minimap around where, on the layer where is on: the
    // tiles, or a floor's picture. Comes back as where, moved to the spot if
    // it's found.
    static Spot nearbyOn(const cv::Mat& grayMini, Spot where, bool wide, double zoom) {
        Spot spot = where;
        spot.found = false;
        spot.score = 0.0;
        spot.zoom = zoom;
        double lat = where.lat, lng = where.lng;
        if (grayMini.cols != SIZE || grayMini.rows != SIZE || zoom <= 0.0) return spot;
        static const cv::Mat mask = ring(SIZE);
        cv::Mat mini = details(grayMini, SIZE);
        double detail = contrast(mini, mask);
        if (detail < FLAT) return spot;

        // drawn at the minimap's size: SIZE plus the reach on every side
        int reach = wide ? WIDE : REACH;
        int side = SIZE + 2 * reach;
        double perPixel = UNITS_ACROSS / zoom / SIZE;
        cv::Mat drawn;
        if (where.floorId != 0) {
            int index = floorOf(where);
            if (index >= 0) drawn = drawFloor(index, lat, lng, side, perPixel);
        }
        else {
            auto found = grids.find(where.mapId);
            if (found != grids.end()) drawn = tiles.Draw(where.mapId, found->second, lng - side / 2.0 * perPixel, lat - side / 2.0 * perPixel, side, perPixel, SHRINK);
        }
        if (drawn.empty()) return spot;
        drawn = details(drawn, SIZE);

        cv::Mat result = fit(drawn, mini, mask, true);
        double best = 0.0;
        cv::Point at;
        cv::minMaxLoc(result, nullptr, &best, nullptr, &at);
        if (contrast(drawn(cv::Rect(at.x, at.y, SIZE, SIZE)), mask) < std::max(FLAT, detail * DETAIL)) return spot;

        // the result is one sample per pixel, the real top is in between
        double dx = 0.0, dy = 0.0;
        if (at.x > 0 && at.x < result.cols - 1) dx = top(result.at<float>(at.y, at.x - 1), (float)best, result.at<float>(at.y, at.x + 1));
        if (at.y > 0 && at.y < result.rows - 1) dy = top(result.at<float>(at.y - 1, at.x), (float)best, result.at<float>(at.y + 1, at.x));

        double other = 0.0;
        cv::circle(result, at, AWAY, cv::Scalar(-1.0), cv::FILLED);
        cv::minMaxLoc(result, nullptr, &other);

        spot.score = best;
        spot.found = best >= FOUND && best - other >= SHARP;
        if (spot.found) {
            spot.lng = lng + (at.x + dx - reach) * perPixel;
            spot.lat = lat + (at.y + dy - reach) * perPixel;
        }
        return spot;
    }

    Spot nearby(const cv::Mat& mini, int mapId, double lat, double lng, bool wide, double zoom) {
        Spot where;
        where.mapId = mapId;
        where.lat = lat;
        where.lng = lng;
        return nearbyOn(mini, where, wide, zoom);
    }

    static bool better(const Spot& spot, const Spot& than) {
        return spot.found > than.found || (spot.found == than.found && spot.score > than.score);
    }

    // The other layers there are at a spot: the floors under the surface, or
    // the surface and the other floors when it's on one.
    static std::vector<Spot> layersAt(const Spot& last) {
        std::vector<Spot> layers;
        Spot surface = last;
        surface.groupId = surface.floorId = 0;
        if (last.floorId != 0) layers.push_back(surface);
        for (const Floor& floor : floors) {
            if (away(floor, last.mapId, last.lat, last.lng) > 0.0 || floor.floorId == last.floorId) continue;
            Spot layer = surface;
            layer.groupId = floor.groupId;
            layer.floorId = floor.floorId;
            layers.push_back(layer);
        }
        return layers;
    }

    // the same spot on the other layers
    static Spot otherLayer(const cv::Mat& mini, const Spot& last, const std::vector<double>& zooms) {
        Spot best;
        for (const Spot& layer : layersAt(last)) {
            for (double zoom : zooms) {
                Spot spot = nearbyOn(mini, layer, false, zoom);
                if (better(spot, best)) best = spot;
            }
        }
        return best;
    }

    // Where the minimap fits best on each floor picture that's on disk, at
    // the zoom that fits it best. Rough, the pictures are small. The best
    // TRIES floors, best first, the score in the spot. With an area only the
    // floors that reach into it.
    static std::vector<Spot> floorTries(const cv::Mat& mini, const std::vector<double>& zooms, const Area* area = nullptr) {
        std::vector<Spot> tries;
        for (size_t i = 0; i < floors.size(); ++i) {
            const Floor& floor = floors[i];
            if (area && (floor.mapId != area->mapId || floor.right < area->left || floor.left > area->right || floor.bottom < area->top || floor.top > area->bottom)) continue;
            if (floorSmalls[i].empty()) floorSmalls[i] = MapData::readGray(floor.path, FLOOR_SMALL);
            if (floorSmalls[i].empty()) continue;
            Spot kept;
            for (double zoom : zooms) {
                // shrunk more for the wide zooms, so the minimap is about as big every time
                double more = std::max(1.0, (double)SMALL / FLOOR_SMALL / zoom);
                cv::Mat whole = floorSmalls[i];
                if (more > 1.0) cv::resize(whole, whole, cv::Size(std::max(1, (int)(whole.cols / more)), std::max(1, (int)(whole.rows / more))), 0, 0, cv::INTER_AREA);
                double shrunkBy = (floor.right - floor.left) / whole.cols;
                int n = (int)std::lround(UNITS_ACROSS / zoom / shrunkBy);
                if (n < 8 || whole.cols <= n || whole.rows <= n) continue;

                cv::Mat little;
                cv::resize(mini, little, cv::Size(n, n), 0, 0, cv::INTER_AREA);
                cv::Mat result = fit(details(whole, n), details(little, n), ring(n), false);
                double score = 0.0;
                cv::Point at;
                cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
                if (kept.mapId != 0 && kept.score >= score) continue;
                kept.mapId = floor.mapId;
                kept.groupId = floor.groupId;
                kept.floorId = floor.floorId;
                kept.lng = floor.left + (at.x + n / 2.0) * shrunkBy;
                kept.lat = floor.top + (at.y + n / 2.0) * shrunkBy;
                kept.zoom = zoom;
                kept.score = score;
            }
            if (kept.mapId != 0) tries.push_back(kept);
        }
        std::sort(tries.begin(), tries.end(), [](const Spot& a, const Spot& b) { return a.score > b.score; });
        if (tries.size() > (size_t)TRIES) tries.resize(TRIES);
        return tries;
    }

    // Under ground or water the surface can look enough like the minimap to
    // pass. The floor fits better then.
    static Spot bestLayer(const cv::Mat& mini, const Spot& found, const std::vector<double>& zooms) {
        Spot layer = otherLayer(mini, found, zooms);
        return layer.found && layer.score > found.score ? layer : found;
    }

    // looks for it on every floor picture that's on disk
    static Spot anywhereFloors(const cv::Mat& mini, double zoom) {
        Spot best;
        for (const Spot& where : floorTries(mini, { zoom })) {
            Spot spot = nearbyOn(mini, where, false, zoom);
            // open sea fits some floor or other at 0.35
            if (spot.found && spot.score < FOUND_ANY) spot.found = false;
            if (better(spot, best)) best = spot;
        }
        return best;
    }

    // Downloads floor pictures that aren't on disk, the ones closest to a
    // spot first. Most players never go under most of them, so they're only
    // fetched once the minimap is on screen and the tiles don't have it.
    static void fetchFloors(int mapId, double lat, double lng) {
        std::vector<std::pair<double, const Floor*>> missing;
        for (const Floor& floor : floors) {
            if (floor.url.empty() || std::filesystem::exists(floor.path)) continue;
            missing.push_back({ away(floor, mapId, lat, lng), &floor });
        }
        std::sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        for (size_t i = 0; i < missing.size() && i < (size_t)FETCH; ++i) {
            // offline, try again later
            if (!MapData::fetch(missing[i].second->url, missing[i].second->path)) return;
        }
    }

    // The best TRIES places of a rough search, checked against the tiles,
    // which tells for sure. The result's first sample is a minimap of n
    // pixels with its corner at left, top.
    static Spot bestOf(cv::Mat& result, const cv::Mat& mini, int mapId, double zoom, int n, double left, double top, double perPixel) {
        Spot best;
        for (int i = 0; i < TRIES; ++i) {
            double score = 0.0;
            cv::Point at;
            cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
            if (score <= 0.0) break;
            Spot spot = nearby(mini, mapId, top + (at.y + n / 2.0) * perPixel, left + (at.x + n / 2.0) * perPixel, false, zoom);
            if (better(spot, best)) best = spot;
            cv::circle(result, at, n, cv::Scalar(-1.0), cv::FILLED);
        }
        return best;
    }

    // Looks for the minimap on a whole map shrunk small, or only on a part
    // of it, in map units.
    static Spot onMap(const cv::Mat& mini, const cv::Mat& whole, int mapId, const Grid& grid, double zoom, const Area* area = nullptr) {
        Spot best;
        if (whole.empty() || mini.cols != SIZE || mini.rows != SIZE || zoom <= 0.0) return best;
        double shrunkBy = (double)grid.cols * grid.tileSize / whole.cols;
        int n = (int)std::lround(UNITS_ACROSS / zoom / shrunkBy);
        cv::Rect part(0, 0, whole.cols, whole.rows);
        if (area) {
            // half a minimap more on every side, the spot is its middle
            cv::Point from((int)((area->left + grid.originX) / shrunkBy) - n, (int)((area->top + grid.originY) / shrunkBy) - n);
            cv::Point to((int)((area->right + grid.originX) / shrunkBy) + n, (int)((area->bottom + grid.originY) / shrunkBy) + n);
            part &= cv::Rect(from, to);
        }
        if (n < 8 || part.width <= n || part.height <= n) return best;

        cv::Mat little;
        cv::resize(mini, little, cv::Size(n, n), 0, 0, cv::INTER_AREA);
        cv::Mat result = fit(details(whole(part), n), details(little, n), ring(n), false);
        // this small a lot of places look about right
        return bestOf(result, mini, mapId, zoom, n, part.x * shrunkBy - grid.originX, part.y * shrunkBy - grid.originY, shrunkBy);
    }

    Spot anywhere(const cv::Mat& mini, double zoom) {
        Spot best;
        for (const auto& [mapId, grid] : grids) {
            // read every time and let go after, it's 11 MB for Teyvat and only needed after a teleport
            Spot spot = onMap(mini, wholeMap(mapId, grid), mapId, grid, zoom);
            if (better(spot, best)) best = spot;
        }
        Spot floor = anywhereFloors(mini, zoom);
        if (better(floor, best)) best = floor;
        return best;
    }

    void Watcher::SearchSoon(double seconds) {
        m_searchAt = seconds;
        m_searchWait = SEARCH_FIRST;
        m_floorsAt = seconds;
    }

    // the part of the map the big map showed last, at the zooms we know
    Spot Watcher::inArea(const cv::Mat& mini) {
        Spot best;
        auto grid = grids.find(m_area.mapId);
        if (grid == grids.end()) return best;
        if (m_areaMap.empty()) m_areaMap = wholeMap(grid->first, grid->second);
        for (double zoom : m_zooms) {
            Spot spot = onMap(mini, m_areaMap, grid->first, grid->second, zoom, &m_area);
            if (better(spot, best)) best = spot;
        }
        // Zoomed in, like in a city, the minimap is a few pixels on that
        // small map and the right place doesn't make the best ones. So the
        // area drawn from the tiles, with the minimap as big as it is at zoom 1.
        for (double zoom : m_zooms) {
            if (best.found || zoom < AREA_ZOOM) continue;
            double perPixel = UNITS_ACROSS / zoom / AREA_MINI;
            double across = std::max(m_area.right - m_area.left, m_area.bottom - m_area.top) + UNITS_ACROSS / zoom;
            int side = (int)std::ceil(across / perPixel);
            if (side > AREA_SIDE) continue;
            double left = (m_area.left + m_area.right - across) / 2.0, top = (m_area.top + m_area.bottom - across) / 2.0;
            cv::Mat& drawn = m_areaDrawn[zoom];
            if (drawn.empty()) drawn = details(tiles.Draw(grid->first, grid->second, left, top, side, perPixel, SHRINK), AREA_MINI);
            cv::Mat little;
            cv::resize(mini, little, cv::Size(AREA_MINI, AREA_MINI), 0, 0, cv::INTER_AREA);
            cv::Mat result = fit(drawn, details(little, AREA_MINI), ring(AREA_MINI), false);
            Spot spot = bestOf(result, mini, grid->first, zoom, AREA_MINI, left, top, perPixel);
            if (better(spot, best)) best = spot;
        }
        // and the floors under it, a teleport can go under ground or water
        for (const Spot& where : floorTries(mini, m_zooms, &m_area)) {
            Spot spot = nearbyOn(mini, where, true, where.zoom);
            if (better(spot, best)) best = spot;
        }
        return best;
    }

    // Asks the map matching where the minimap is: map, floor and zoom in
    // one go. It finds the big map in a picture just as well, so the look
    // around there has to agree, and a zoom we don't know only counts with a
    // minimap on screen.
    Spot Watcher::locate(const cv::Mat& grab, const cv::Mat& mini, bool there) {
        Spot spot;
        if (!m_locate) return spot;
        MapLocator::Result result = m_locate(grab);
        if (!result.found || result.unitsPerPixel <= 0.0) return spot;

        double zoom = UNITS_ACROSS / (result.unitsPerPixel * grab.cols);
        if (zoom < ZOOM_FIRST || zoom > ZOOM_LAST) return spot;
        bool isNew = true;
        for (double known : m_zooms) {
            if (std::abs(zoom / known - 1.0) > SAME) continue;
            zoom = known;
            isNew = false;
        }
        if (isNew && !there) return spot;
        spot.mapId = result.mapId;
        spot.groupId = result.groupId;
        spot.floorId = result.floorId;
        spot.lat = result.lat;
        spot.lng = result.lng;
        spot = nearbyOn(mini, spot, false, zoom);
        if (spot.found && isNew) m_zooms.push_back(zoom);
        return spot;
    }

    // The minimap at the last spot, at whatever zoom fits. On the other
    // layers too, caves have their own zoom.
    Spot Watcher::anyZoom(const cv::Mat& mini) {
        Spot best;
        for (double zoom = ZOOM_FIRST; zoom <= ZOOM_LAST; zoom *= ZOOM_STEP) {
            Spot spot = nearbyOn(mini, m_last, false, zoom);
            if (spot.found && spot.score > best.score) best = spot;
        }
        if (best.found) return settle(mini, best);

        for (const Spot& layer : layersAt(m_last)) {
            for (double zoom = ZOOM_FIRST; zoom <= ZOOM_LAST; zoom *= ZOOM_STEP) {
                Spot spot = nearbyOn(mini, layer, false, zoom);
                if (spot.found && spot.score >= FOUND_ANY && spot.score > best.score) best = spot;
            }
        }
        if (!best.found) return best;
        return settle(mini, best);
    }

    // a spot found at a zoom that's only about right
    Spot Watcher::settle(const cv::Mat& mini, Spot best) {
        Spot rough = best;
        // one we know, tried a bit off?
        for (double known : m_zooms) {
            if (std::abs(rough.zoom / known - 1.0) > SAME) continue;
            Spot spot = nearbyOn(mini, rough, false, known);
            if (spot.found) return spot;
        }
        // a new one, and what it is exactly
        for (int i = -4; i <= 4; ++i) {
            Spot spot = nearbyOn(mini, rough, false, rough.zoom * (1.0 + i * 0.01));
            if (spot.found && spot.score > best.score) best = spot;
        }
        m_zooms.push_back(best.zoom);
        return best;
    }

    // every floor at every zoom, for a cave it was never in before
    Spot Watcher::anyFloor(const cv::Mat& mini) {
        std::vector<double> zooms;
        for (double zoom = ZOOM_FIRST; zoom <= ZOOM_LAST; zoom *= ZOOM_ROUGH) zooms.push_back(zoom);

        Spot best;
        for (const Spot& where : floorTries(mini, zooms)) {
            for (int i = -6; i <= 6; ++i) {
                Spot spot = nearbyOn(mini, where, false, where.zoom * (1.0 + i * 0.01));
                if (spot.found && spot.score >= FOUND_ANY && spot.score > best.score) best = spot;
            }
        }
        if (!best.found) return best;
        return settle(mini, best);
    }

    const Spot& Watcher::Look(const cv::Mat& grab, double seconds, bool* searched) {
        if (searched) *searched = false;
        cv::Mat mini = shrink(grab);
        bool sign = shown(mini);
        if (sign) m_covered = false;
        else if (m_sign) m_covered = true;
        m_sign = sign;
        if (m_covered) {
            m_last.found = false;
            m_shown = false;
            return m_last;
        }
        // nothing moved, not even the arrow
        if (m_last.found && !m_lastMini.empty() && cv::norm(mini, m_lastMini, cv::NORM_INF) < 2.0) return m_last;
        m_lastMini = mini;

        Spot spot;
        if (m_last.mapId != 0) spot = nearbyOn(mini, m_last, false, m_last.zoom);
        bool there = spot.found || sign;
        // walked into a city or out of one
        if (!spot.found && (there || m_last.found) && m_last.mapId != 0) {
            for (double zoom : m_zooms) {
                if (zoom == m_last.zoom) continue;
                spot = nearbyOn(mini, m_last, false, zoom);
                if (spot.found) break;
            }
        }
        // Went under ground or water, or came back up. Only the moment it's
        // lost, reading floor pictures every look would cost too much.
        if (!spot.found && there && m_last.found) spot = otherLayer(mini, m_last, m_zooms);
        // And at once when it fits a lot worse than at the last look there:
        // after a teleport under water the game shows the surface first and
        // fades to the floor.
        if (spot.found && (seconds >= m_layersAt || spot.score < m_layerScore - LAYER_DROP)) {
            m_layerScore = spot.score;
            spot = bestLayer(mini, spot, m_zooms);
            m_layersAt = seconds + LAYERS_EVERY;
        }
        // A minimap that just came up (loading screen over, menu closed) is
        // searched for right away. After a teleport the quick places below
        // get their turns first, no look happens while the whole map is
        // searched.
        if (there && !m_shown) {
            SearchSoon(seconds);
            if (m_area.mapId != 0) m_searchAt = seconds + SEARCH_FIRST;
        }
        m_shown = there;

        // Lost with a minimap on screen: the quick places, every look. One
        // miss shouldn't cost seconds of searching the whole map.
        if (!spot.found && there) {
            Spot found;
            if (m_last.mapId != 0) found = nearbyOn(mini, m_last, true, m_last.zoom);
            if (!found.found) found = inArea(mini);
            if (!found.found) found = locate(grab, mini, true);
            if (found.found) spot = bestLayer(mini, found, m_zooms);
        }

        // It just went away: a menu, or the big map before its zoom bar is
        // there. Nothing to search for, and a whole map search right then
        // takes the cpu the big map needs to get found.
        if (!spot.found && !there && m_last.found) m_searchAt = std::max(m_searchAt, seconds + SEARCH_FIRST);
        // The same with the N still on screen, the big map fades in over the
        // minimap. Around the last spot it's looked for at once, the whole
        // map waits.
        if (!spot.found && m_last.found) m_farAt = seconds + SEARCH_FIRST;

        if (!spot.found && seconds >= m_searchAt) {
            Spot found;
            // with a minimap on screen these two were tried just above
            if (!there && m_last.mapId != 0) found = nearbyOn(mini, m_last, true, m_last.zoom);
            if (!found.found && !there) found = locate(grab, mini, false);
            if (!found.found && there && m_last.mapId != 0) found = otherLayer(mini, m_last, m_zooms);
            if (!found.found && there && m_last.mapId != 0) found = anyZoom(mini);
            // the zoomed ones double the work, so only with a minimap on screen
            for (double zoom : m_zooms) {
                if (!found.found && seconds >= m_farAt && (there || zoom == 1.0)) found = anywhere(mini, zoom);
            }
            if (!found.found && there && seconds >= m_floorsAt && seconds >= m_farAt) {
                found = anyFloor(mini);
                m_floorsAt = seconds + FLOORS_EVERY;
            }
            if (found.found) spot = there ? bestLayer(mini, found, m_zooms) : found;
            if (searched) *searched = true;
            if (seconds < m_farAt) {
                // the rest of it as soon as that's over
                m_searchAt = m_farAt;
            }
            else {
                m_searchWait = std::min(m_searchWait * 2.0, SEARCH_LAST);
                m_searchAt = seconds + m_searchWait;
            }
        }

        if (spot.found) {
            m_last = spot;
            m_searchWait = SEARCH_FIRST;
            m_searchAt = 0.0;
            m_floorsAt = 0.0;
            m_area = Area();
            m_areaMap.release();
            m_areaDrawn.clear();
        }
        else {
            // stays where it was last seen
            m_last.found = false;
            m_last.score = spot.score;
        }
        return m_last;
    }

    static void loop() {
        Watcher watcher(MapTracking::Locate);
        bool wasOpen = false;
        double fetchAt = 0.0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (wake.wait_for(lock, EVERY, []() { return stopping; })) return;
            }
            double seconds = now();
            if (seconds - askedAt.load() > ASKED_FOR) continue;
            // on the first start the map data is still downloading
            if (grids.empty() && MapTracking::GetStatus() == MapTracking::Status::Ready) Load(folder);
            if (grids.empty()) continue;
            // in another window there's nothing to draw over
            if (!MapTracking::GameInFront()) {
                std::lock_guard<std::mutex> lock(mutex);
                player.found = false;
                continue;
            }
            // the big map covers the minimap, no spot until it closes
            if (MapTracking::IsMapOpen()) {
                wasOpen = true;
                std::lock_guard<std::mutex> lock(mutex);
                player.found = false;
                continue;
            }
            // teleports start on the big map, so look everywhere soon after it closes
            if (wasOpen) {
                wasOpen = false;
                watcher.SearchSoon(seconds);
                MapTracking::MapView view = MapTracking::LastView();
                double halfWide = (view.gameRect.right - view.gameRect.left) / 2.0 * view.result.unitsPerPixel;
                double halfHigh = (view.gameRect.bottom - view.gameRect.top) / 2.0 * view.result.unitsPerPixel;
                if (view.result.found && halfWide > 0.0) {
                    watcher.SearchFirst({ view.result.mapId, view.result.lng - halfWide, view.result.lat - halfHigh, view.result.lng + halfWide, view.result.lat + halfHigh });
                }
            }

            HWND game = Capture::findGameWindow();
            RECT client;
            cv::Mat grab;
            if (!game || IsIconic(game) || !Capture::getClientRectOnScreen(game, client)) continue;
            if (!Capture::grabGame(game, rect(client), grab) || grab.empty()) continue;

            Spot spot;
            try {
                spot = watcher.Look(grab, seconds);
            }
            catch (...) {
                // a look that goes wrong is skipped
                continue;
            }
            // maybe it shows a floor we don't have the picture of yet
            if (watcher.Lost() && seconds >= fetchAt) {
                fetchFloors(spot.mapId, spot.lat, spot.lng);
                fetchAt = now() + FETCH_EVERY;
            }
            spot.square = rect(client);
            std::lock_guard<std::mutex> lock(mutex);
            player = spot;
        }
    }

    void Start(const std::filesystem::path& dataFolder) {
        if (worker.joinable()) return;
        Load(dataFolder);
        stopping = false;
        worker = std::thread(loop);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    bool Wanted() {
        return now() - askedAt.load() <= ASKED_FOR;
    }

    Spot GetPlayer() {
        askedAt = now();
        std::lock_guard<std::mutex> lock(mutex);
        // right away, the thread may be in the middle of a search
        if (MapTracking::IsMapOpen() || !MapTracking::GameInFront()) player.found = false;
        return player;
    }
}
