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
#include <sstream>
#include <thread>
#include <concrt.h>
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
    // A new zoom is kept once it's still the zoom this many seconds later.
    // The game zooms the minimap over a second or two, and a look in the
    // middle of that finds a zoom that's never seen again. The longest one
    // on the way was followed for 1.4 seconds.
    static constexpr double ZOOM_STAYS = 3.0;
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
    static constexpr int SMALL = MapData::SMALL_BY;
    // Floor pictures are kept shrunk by this for searching, 6 MB for all of
    // them. Less than SMALL, they're searched at zooms where shrunk by 8
    // leaves too little of the minimap.
    static constexpr int FLOOR_SMALL = 4;
    // The zoom steps of searching the floors without knowing the zoom. That
    // search is a few seconds of work, so FLOORS_EVERY seconds apart.
    static constexpr double ZOOM_ROUGH = 1.2;
    static constexpr double FLOORS_EVERY = 30.0;
    // While it's followed, the other layers at the spot get a look this many
    // seconds apart. Diving, the surface can keep passing for the minimap.
    static constexpr double LAYERS_EVERY = 1.0;
    static constexpr double LAYER_DROP = 0.08;
    // Looking through every floor, or trying every zoom on another layer, is
    // a lot of chances to fit by accident, so those have to fit this well.
    // The real ones are 0.6 and up.
    static constexpr double FOUND_ANY = 0.5;
    // Floor pictures kept in memory, up to 2.6 MB each: this many for a
    // search, and for the looks as many as there can be layers at one spot.
    static constexpr size_t FLOORS_KEPT = 3, FLOORS_LOOKED = 6;
    // Floor pictures that aren't on disk are downloaded this many at a time
    // while the minimap is on screen and not found, FETCH_EVERY seconds apart.
    static constexpr int FETCH = 20;
    static constexpr double FETCH_EVERY = 2.0;

    // the minimap is looked at this often
    static constexpr auto EVERY = std::chrono::milliseconds(50);
    // GetPlayer wasn't called for this long: nobody needs it, stop looking
    static constexpr double ASKED_FOR = 2.0;
    // Seconds between two whole map searches while the minimap isn't found.
    // Doubles every time up to SEARCH_LAST: in a domain it fits nowhere for
    // minutes, and every one of them is seconds of a cpu core.
    static constexpr double SEARCH_FIRST = 1.0, SEARCH_LAST = 30.0;
    // Lost with the minimap on screen all the time, the player is taken to
    // be near the last spot for this long. Then the whole map gets searched.
    static constexpr double NEAR_FOR = 1.0;
    // a download that failed, or a picture that isn't there, is tried again after this many seconds
    static constexpr double RETRY = 5.0;
    // the same for making a small whole map, which is a lot of downloads
    static constexpr double SMALL_RETRY = 60.0;
    // From this zoom on the area the big map showed is searched drawn from
    // the tiles, the minimap AREA_MINI pixels wide, if that picture is
    // AREA_SIDE pixels a side at most.
    static constexpr double AREA_ZOOM = 1.5;
    static constexpr int AREA_MINI = 43, AREA_SIDE = 3000;
    // This many places it was followed at are remembered. Closer together
    // than PLACE_APART map units is the same place.
    static constexpr size_t PLACES = 4;
    static constexpr double PLACE_APART = 100.0;
    // They're written next to the map data this often, for the next start.
    static constexpr double PLACES_EVERY = 10.0;

    using MapData::Grid;
    using MapData::Floor;

    static std::map<int, Grid> grids;
    // Underground and under water the minimap shows a floor's picture
    // instead of the tiles.
    static std::vector<Floor> floors;
    // each one whole, shrunk by FLOOR_SMALL, read when it's first searched
    static std::vector<cv::Mat> floorSmalls;
    // Floor pictures in memory, shrunk by SHRINK, by their place in that
    // list.
    struct FloorPictures {
        explicit FloorPictures(size_t kept) : m_kept(kept) {}
        // empty if it isn't there
        cv::Mat Get(int index) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_pictures.find(index);
            if (it == m_pictures.end()) return {};
            m_order.erase(std::find(m_order.begin(), m_order.end(), index));
            m_order.push_back(index);
            return it->second;
        }
        // the one that wasn't used for longest makes room
        void Put(int index, const cv::Mat& picture) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pictures.count(index)) return;
            if (m_order.size() >= m_kept) {
                m_pictures.erase(m_order.front());
                m_order.pop_front();
            }
            m_order.push_back(index);
            m_pictures[index] = picture;
        }
        void Clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pictures.clear();
            m_order.clear();
        }

    private:
        size_t m_kept;
        std::mutex m_mutex;
        std::map<int, cv::Mat> m_pictures;
        std::deque<int> m_order;
    };
    // The ones the looks of a Watcher with threads follow on, its loader
    // puts them there. A search goes through a lot of floors and would push
    // these out, so every other thread has its own.
    static FloorPictures floorsLooked(FLOORS_LOOKED);
    static thread_local FloorPictures floorsOwn(FLOORS_KEPT);
    static std::filesystem::path folder;
    static std::thread worker;
    static std::mutex mutex;
    static std::condition_variable wake;
    // guarded by mutex
    static bool stopping = false;
    static Spot player;
    // when GetPlayer was last called, steady_clock seconds
    static std::atomic<double> askedAt{ -1000.0 };

    static double now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Work a look can't wait for: downloads, and reading big pictures. Done
    // on two threads of its own, so one long download doesn't hold up the
    // rest.
    class Loader {
    public:
        // quit is set when the work should stop early
        typedef std::function<bool(const std::atomic<bool>& quit)> Work;

        Loader() {
            for (std::thread& thread : m_threads) thread = std::thread(&Loader::run, this);
        }
        ~Loader() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_quit = true;
            }
            m_wake.notify_all();
            for (std::thread& thread : m_threads) thread.join();
        }
        // Work is done once, however often it's asked for under the same
        // name. If it comes back false (offline, the file isn't there, or
        // there was nothing to get) it can be asked for again after retry
        // seconds.
        void Ask(const std::string& name, Work work, double retry = RETRY) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto asked = m_asked.find(name);
            if (asked != m_asked.end() && now() < asked->second) return;
            m_asked[name] = 1e300;
            m_work.push_back({ name, std::move(work), retry });
            m_wake.notify_one();
        }
        // how many came back true so far, to see that something new is there
        int Done() const { return m_done; }

    private:
        struct Job {
            std::string name;
            Work work;
            double retry = RETRY;
        };

        void run() {
            OneThread alone;
            std::unique_lock<std::mutex> lock(m_mutex);
            while (true) {
                m_wake.wait(lock, [this]() { return m_quit || !m_work.empty(); });
                if (m_quit) return;
                Job job = std::move(m_work.front());
                m_work.pop_front();
                lock.unlock();
                bool done = false;
                try {
                    done = job.work(m_quit);
                }
                catch (...) {
                    // a picture that can't be read, tried again later
                }
                lock.lock();
                if (done) {
                    m_asked.erase(job.name);
                    ++m_done;
                }
                else m_asked[job.name] = now() + job.retry;
            }
        }

        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::deque<Job> m_work;
        // The names waiting or being worked on, and the ones that failed
        // with when they can be asked for again.
        std::map<std::string, double> m_asked;
        std::atomic<bool> m_quit{ false };
        std::atomic<int> m_done{ 0 };
        std::thread m_threads[2];
    };

    // The loader of the look that's running on this thread. Whatever isn't
    // in memory then is asked of it and the look goes on without. Nothing
    // on any other thread, and in a Watcher without threads: those read and
    // download what they need right there.
    static thread_local Loader* later = nullptr;

    // A tile that isn't on disk. The app only downloads the tiles the big
    // map shows, so the ones missing here get fetched.
    static bool fetchTile(int mapId, const Grid& grid, int x, int y) {
        if (later) {
            later->Ask("tile " + MapData::tilePath(grid, x, y).string(), [mapId, grid, x, y](const std::atomic<bool>&) { return MapData::fetchTile(mapId, grid, x, y); });
            return false;
        }
        // offline, not every tile of a search has to find that out by itself
        static std::atomic<double> againAt{ 0.0 };
        if (now() < againAt) return false;
        if (MapData::fetchTile(mapId, grid, x, y)) return true;
        againAt = now() + RETRY;
        return false;
    }

    // one for every thread that draws
    static thread_local MapData::Tiles tiles(TILES_KEPT, fetchTile);

    // makes the small whole maps that aren't on disk
    static bool makeSmallMaps(const std::atomic<bool>& quit) {
        bool all = true;
        for (const auto& [mapId, grid] : grids) {
            if (!std::filesystem::exists(MapData::wholeMapPath(folder, grid)) && !MapData::makeWholeMap(folder, mapId, grid, &quit)) all = false;
        }
        return all;
    }

    // A whole map shrunk by SMALL, for searching all of it. They come with
    // the map index. One that didn't is made from the tiles, which downloads
    // the tiles that are missing: for Teyvat that's all of them, once.
    // Empty until it's there.
    static cv::Mat wholeMap(int mapId, const Grid& grid, Loader* loader, const std::atomic<bool>* dropped) {
        std::filesystem::path path = MapData::wholeMapPath(folder, grid);
        cv::Mat whole = MapData::readGray(path);
        if (!whole.empty()) return whole;
        if (loader) {
            loader->Ask("small maps", makeSmallMaps, SMALL_RETRY);
            return {};
        }
        if (!MapData::makeWholeMap(folder, mapId, grid, dropped)) return {};
        return MapData::readGray(path);
    }

    // reads a floor's picture into memory, false if it isn't on disk
    static bool readFloor(int index, FloorPictures& kept) {
        cv::Mat image = MapData::readGray(floors[index].path, SHRINK);
        if (image.empty()) return false;
        kept.Put(index, image);
        return true;
    }

    // one floor's picture shrunk by SHRINK, empty while it isn't there
    static cv::Mat floorPicture(int index) {
        if (later) {
            cv::Mat image = floorsLooked.Get(index);
            if (image.empty()) later->Ask("floor " + std::to_string(index), [index](const std::atomic<bool>&) { return readFloor(index, floorsLooked); });
            return image;
        }
        cv::Mat image = floorsOwn.Get(index);
        if (image.empty() && readFloor(index, floorsOwn)) image = floorsOwn.Get(index);
        return image;
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
    // the two is enough, a marker can sit on the N.
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
        floorsLooked.Clear();
        floorsOwn.Clear();

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
        cv::Mat image = floorPicture(index);
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
        // A look compares one minimap with a lot of places, its details are
        // only worked out for the first. The picture is held on to, so its
        // memory can't have become another picture's by the next compare.
        struct Kept {
            cv::Mat of, details;
            double detail = 0.0;
        };
        static thread_local Kept kept;
        if (kept.of.data != grayMini.data) {
            kept.of = grayMini;
            kept.details = details(grayMini, SIZE);
            kept.detail = contrast(kept.details, mask);
        }
        const cv::Mat& mini = kept.details;
        double detail = kept.detail;
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

    // What a search over many places came up with: the best one, and how
    // well the next best one somewhere else fit. Searching a whole map,
    // something always fits best. It's only the place when it stands out
    // from the rest by SHARP, like it has to from what's around it.
    struct Best {
        Spot spot;
        double next = 0.0;

        void take(const Spot& one) {
            // the same place come by another way, or the floor right under it
            bool same = spot.mapId == one.mapId
                && std::hypot(spot.lat - one.lat, spot.lng - one.lng) < UNITS_ACROSS / std::max(spot.zoom, one.zoom) / 2.0;
            if (better(one, spot)) {
                if (!same) next = std::max(next, spot.score);
                spot = one;
            }
            else if (!same) next = std::max(next, one.score);
        }
        void take(const Best& many) {
            next = std::max(next, many.next);
            take(many.spot);
        }
        Spot alone() const {
            Spot one = spot;
            if (one.score - next < SHARP) one.found = false;
            return one;
        }
    };

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
    static std::vector<Spot> floorTries(const cv::Mat& mini, const std::vector<double>& zooms, const Area* area = nullptr, const std::atomic<bool>* dropped = nullptr) {
        std::vector<Spot> tries;
        for (size_t i = 0; i < floors.size(); ++i) {
            if (dropped && *dropped) return {};
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
                if (n < 8) continue;
                // Half a minimap of nothing on every side. Some pictures are
                // smaller than what the minimap shows, and near a picture's
                // edge the minimap hangs over it.
                int over = n / 2;
                cv::copyMakeBorder(whole, whole, over, over, over, over, cv::BORDER_CONSTANT, cv::Scalar(0));

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
                kept.lng = floor.left + (at.x - over + n / 2.0) * shrunkBy;
                kept.lat = floor.top + (at.y - over + n / 2.0) * shrunkBy;
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
    // pass. The floor fits better then. The same for a zoom within SAME of
    // the one it was found at: one kept in the dark desert passed on the
    // surface at 0.55 where the zoom next to it fit at 0.85.
    static Spot bestLayer(const cv::Mat& mini, const Spot& found, const std::vector<double>& zooms) {
        Spot best = found;
        for (double zoom : zooms) {
            if (zoom == found.zoom || std::abs(zoom / found.zoom - 1.0) > SAME) continue;
            Spot spot = nearbyOn(mini, found, false, zoom);
            if (spot.found && spot.score > best.score) best = spot;
        }
        Spot layer = otherLayer(mini, found, zooms);
        return layer.found && layer.score > best.score ? layer : best;
    }

    // looks for it on every floor picture that's on disk
    static Best anywhereFloors(const cv::Mat& mini, double zoom, const std::atomic<bool>* dropped) {
        Best best;
        for (const Spot& where : floorTries(mini, { zoom }, nullptr, dropped)) {
            Spot spot = nearbyOn(mini, where, false, zoom);
            // open sea fits some floor or other at 0.35
            if (spot.found && spot.score < FOUND_ANY) spot.found = false;
            best.take(spot);
        }
        return best;
    }

    // Downloads floor pictures that aren't on disk, the ones closest to a
    // spot first. Most players never go under most of them, so they're only
    // fetched once the minimap is on screen and the tiles don't have it.
    // True if any came in.
    static bool fetchFloors(int mapId, double lat, double lng, const std::atomic<bool>& quit) {
        std::vector<std::pair<double, const Floor*>> missing;
        for (const Floor& floor : floors) {
            if (floor.url.empty() || std::filesystem::exists(floor.path)) continue;
            missing.push_back({ away(floor, mapId, lat, lng), &floor });
        }
        std::sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        size_t got = 0;
        while (got < missing.size() && got < (size_t)FETCH && !quit && MapData::fetch(missing[got].second->url, missing[got].second->path)) ++got;
        return got > 0;
    }

    // The best TRIES places of a rough search, checked against the tiles,
    // which tells for sure. The result's first sample is a minimap of n
    // pixels with its corner at left, top.
    static Best bestOf(cv::Mat& result, const cv::Mat& mini, int mapId, double zoom, int n, double left, double top, double perPixel) {
        Best best;
        for (int i = 0; i < TRIES; ++i) {
            double score = 0.0;
            cv::Point at;
            cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
            if (score <= 0.0) break;
            Spot spot = nearby(mini, mapId, top + (at.y + n / 2.0) * perPixel, left + (at.x + n / 2.0) * perPixel, false, zoom);
            best.take(spot);
            cv::circle(result, at, n, cv::Scalar(-1.0), cv::FILLED);
        }
        return best;
    }

    // Looks for the minimap on a whole map shrunk small, or only on a part
    // of it, in map units.
    static Best onMap(const cv::Mat& mini, const cv::Mat& whole, int mapId, const Grid& grid, double zoom, const Area* area = nullptr) {
        Best best;
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

    // Dropped, if given, is looked at between two maps: when it's set the
    // search isn't wanted anymore and stops.
    static Spot anywhereUntil(const cv::Mat& mini, double zoom, Loader* loader, const std::atomic<bool>* dropped) {
        Best best;
        for (const auto& [mapId, grid] : grids) {
            if (dropped && *dropped) return Spot();
            // read every time and let go after, it's 11 MB for Teyvat and only needed after a teleport
            best.take(onMap(mini, wholeMap(mapId, grid, loader, dropped), mapId, grid, zoom));
        }
        best.take(anywhereFloors(mini, zoom, dropped));
        return best.alone();
    }

    Spot anywhere(const cv::Mat& mini, double zoom) {
        return anywhereUntil(mini, zoom, nullptr, nullptr);
    }

    static bool samePlace(const Spot& a, const Spot& b) {
        return a.mapId == b.mapId && a.groupId == b.groupId && a.floorId == b.floorId && a.zoom == b.zoom
            && std::hypot(a.lat - b.lat, a.lng - b.lng) < PLACE_APART;
    }

    void Watcher::Remember(const Spot& spot) {
        m_places.erase(std::remove_if(m_places.begin(), m_places.end(), [&spot](const Spot& place) { return samePlace(place, spot); }), m_places.end());
        if (m_places.size() >= PLACES) m_places.erase(m_places.begin());
        m_places.push_back(spot);
    }

    void Watcher::SearchSoon(double seconds) {
        m_searchAt = seconds;
        m_searchWait = SEARCH_FIRST;
        m_floorsAt = seconds;
        m_empty = false;
    }

    void Watcher::SearchFirst(const Area& area) {
        m_area = area;
        m_missed.release();
        m_areaAsked.release();
        drop();
    }

    Watcher::Watcher(Locate locate, bool ownThreads) : m_locate(std::move(locate)) {
        if (!ownThreads) return;
        m_loader = std::make_unique<Loader>();
        m_thread = std::thread(&Watcher::run, this);
    }

    Watcher::~Watcher() {
        if (later == m_loader.get()) later = nullptr;
        if (!m_thread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
            m_dropped = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }

    bool Watcher::Searching() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_busy;
    }

    // Starts a search. Only when none is running.
    void Watcher::ask(Search search) {
        if (!m_thread.joinable()) {
            m_dropped = false;
            m_answer = find(search);
            m_done = true;
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dropped = false;
        m_search = std::move(search);
        m_asked = m_busy = true;
        m_done = false;
        m_wake.notify_all();
    }

    // what the last search found, once
    bool Watcher::answered(Answer& answer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_done) return false;
        m_done = false;
        answer = std::move(m_answer);
        return true;
    }

    // The search that's running, or its answer, isn't wanted anymore. It
    // stops at the next map and lets go of what it kept.
    void Watcher::drop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_done = false;
        if (m_busy) {
            m_dropped = true;
            return;
        }
        m_areaOf = Area();
        m_areaMap.release();
        m_areaDrawn.clear();
    }

    void Watcher::run() {
        OneThread alone;
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            m_wake.wait(lock, [this]() { return m_quit || m_asked; });
            if (m_quit) return;
            Search search = m_search;
            m_asked = false;
            lock.unlock();
            Answer answer;
            try {
                answer = find(search);
            }
            catch (...) {
                // a search that goes wrong found nothing
                answer = Answer();
                answer.search = search;
            }
            lock.lock();
            if (m_dropped) {
                m_areaOf = Area();
                m_areaMap.release();
                m_areaDrawn.clear();
            }
            else {
                m_answer = std::move(answer);
                m_done = true;
            }
            m_busy = false;
        }
    }

    // The likely places first: the part of the map the big map showed, then
    // around the last spot, then everywhere.
    Watcher::Answer Watcher::find(const Search& search) {
        Answer answer;
        answer.search = search;
        Spot found;
        if (search.area.mapId != 0) found = inArea(search);
        if (!found.found && search.around) found = otherLayer(search.mini, search.last, search.zooms);
        if (!found.found && search.around) found = anyZoom(search);
        // the zoomed ones double the work, so only with a minimap on screen
        for (double zoom : search.zooms) {
            if (!found.found && search.everywhere && (search.there || zoom == 1.0)) found = anywhereUntil(search.mini, zoom, m_loader.get(), &m_dropped);
        }
        if (!found.found && search.floors) found = anyFloor(search);
        // the looks go on from there, so they get the floor's picture along with it
        int floor = found.found ? floorOf(found) : -1;
        if (floor >= 0 && m_loader) {
            cv::Mat picture = floorsOwn.Get(floor);
            if (!picture.empty()) floorsLooked.Put(floor, picture);
        }
        answer.spot = found;
        return answer;
    }

    static bool sameArea(const Area& a, const Area& b) {
        return a.mapId == b.mapId && a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }

    // the part of the map the big map showed last, at the zooms we know
    Spot Watcher::inArea(const Search& search) {
        Best best;
        const cv::Mat& mini = search.mini;
        const Area& area = search.area;
        auto grid = grids.find(area.mapId);
        if (grid == grids.end()) return Spot();
        if (!sameArea(area, m_areaOf)) {
            m_areaOf = area;
            m_areaMap.release();
            m_areaDrawn.clear();
        }
        if (m_areaMap.empty()) m_areaMap = wholeMap(grid->first, grid->second, m_loader.get(), &m_dropped);
        for (double zoom : search.zooms) {
            if (m_dropped) return Spot();
            best.take(onMap(mini, m_areaMap, grid->first, grid->second, zoom, &area));
        }
        // Zoomed in, like in a city, the minimap is a few pixels on that
        // small map and the right place doesn't make the best ones. So the
        // area drawn from the tiles, with the minimap as big as it is at zoom 1.
        for (double zoom : search.zooms) {
            if (best.alone().found || zoom < AREA_ZOOM || m_dropped) continue;
            double perPixel = UNITS_ACROSS / zoom / AREA_MINI;
            double across = std::max(area.right - area.left, area.bottom - area.top) + UNITS_ACROSS / zoom;
            int side = (int)std::ceil(across / perPixel);
            if (side > AREA_SIDE) continue;
            double left = (area.left + area.right - across) / 2.0, top = (area.top + area.bottom - across) / 2.0;
            cv::Mat& drawn = m_areaDrawn[zoom];
            if (drawn.empty()) drawn = details(tiles.Draw(grid->first, grid->second, left, top, side, perPixel, SHRINK), AREA_MINI);
            cv::Mat little;
            cv::resize(mini, little, cv::Size(AREA_MINI, AREA_MINI), 0, 0, cv::INTER_AREA);
            cv::Mat result = fit(drawn, details(little, AREA_MINI), ring(AREA_MINI), false);
            best.take(bestOf(result, mini, grid->first, zoom, AREA_MINI, left, top, perPixel));
        }
        // and the floors under it, a teleport can go under ground or water
        for (const Spot& where : floorTries(mini, search.zooms, &area, &m_dropped)) {
            best.take(nearbyOn(mini, where, true, where.zoom));
        }
        return best.alone();
    }

    // Asks the map matching where the minimap is: map, floor and zoom in
    // one go. It finds the big map in a picture just as well, so the look
    // around there has to agree, and a zoom we don't know only counts with a
    // minimap on screen.
    Spot Watcher::locate(const cv::Mat& grab, const cv::Mat& mini, bool there, double seconds) {
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
        if (spot.found) keep(zoom, seconds);
        return spot;
    }

    // A zoom the minimap was found at. One that isn't known yet is new
    // until it stayed for ZOOM_STAYS, see Look.
    void Watcher::keep(double zoom, double seconds) {
        if (std::find(m_zooms.begin(), m_zooms.end(), zoom) != m_zooms.end()) return;
        // the new one before it was on the way to this one
        forgetNew();
        m_zooms.push_back(zoom);
        m_newZoom = zoom;
        m_newZoomAt = seconds;
    }

    void Watcher::forgetNew() {
        m_zooms.erase(std::remove(m_zooms.begin(), m_zooms.end(), m_newZoom), m_zooms.end());
        m_newZoom = 0.0;
    }

    // The minimap at the last spot, at whatever zoom fits. On the other
    // layers too, caves have their own zoom.
    Spot Watcher::anyZoom(const Search& search) {
        Spot best;
        for (double zoom = ZOOM_FIRST; zoom <= ZOOM_LAST; zoom *= ZOOM_STEP) {
            if (m_dropped) return Spot();
            Spot spot = nearbyOn(search.mini, search.last, false, zoom);
            if (spot.found && spot.score > best.score) best = spot;
        }
        if (best.found) return settle(search, best);

        for (const Spot& layer : layersAt(search.last)) {
            for (double zoom = ZOOM_FIRST; zoom <= ZOOM_LAST; zoom *= ZOOM_STEP) {
                if (m_dropped) return Spot();
                Spot spot = nearbyOn(search.mini, layer, false, zoom);
                if (spot.found && spot.score >= FOUND_ANY && spot.score > best.score) best = spot;
            }
        }
        if (!best.found) return best;
        return settle(search, best);
    }

    // a spot found at a zoom that's only about right
    Spot Watcher::settle(const Search& search, Spot best) {
        Spot rough = best;
        // one we know, tried a bit off?
        for (double known : search.zooms) {
            if (std::abs(rough.zoom / known - 1.0) > SAME) continue;
            Spot spot = nearbyOn(search.mini, rough, false, known);
            if (spot.found) return spot;
        }
        // a new one, and what it is exactly
        for (int i = -4; i <= 4; ++i) {
            Spot spot = nearbyOn(search.mini, rough, false, rough.zoom * (1.0 + i * 0.01));
            if (spot.found && spot.score > best.score) best = spot;
        }
        return best;
    }

    // every floor at every zoom, for a cave it was never in before
    Spot Watcher::anyFloor(const Search& search) {
        std::vector<double> zooms;
        for (double zoom = ZOOM_FIRST; zoom <= ZOOM_LAST; zoom *= ZOOM_ROUGH) zooms.push_back(zoom);

        Best best;
        for (const Spot& where : floorTries(search.mini, zooms, nullptr, &m_dropped)) {
            Spot floor;
            for (int i = -6; i <= 6; ++i) {
                Spot spot = nearbyOn(search.mini, where, false, where.zoom * (1.0 + i * 0.01));
                if (spot.found && spot.score < FOUND_ANY) spot.found = false;
                if (better(spot, floor)) floor = spot;
            }
            best.take(floor);
        }
        Spot found = best.alone();
        if (!found.found) return found;
        return settle(search, found);
    }

    // the same picture, give or take the noise of a grab
    static bool same(const cv::Mat& a, const cv::Mat& b) {
        return !a.empty() && !b.empty() && cv::norm(a, b, cv::NORM_INF) < 2.0;
    }

    // Around the last spot: where it was, at the zooms we know and on the
    // other layers there. Loaded says a picture that was missing came in.
    Spot Watcher::around(const cv::Mat& mini, bool sign, double seconds, bool loaded) {
        Spot spot;
        if (m_last.mapId == 0) return spot;
        spot = nearbyOn(mini, m_last, false, m_last.zoom);
        bool there = spot.found || sign;
        // walked into a city or out of one
        if (!spot.found && (there || m_last.found)) {
            for (double zoom : m_zooms) {
                if (zoom == m_last.zoom) continue;
                spot = nearbyOn(mini, m_last, false, zoom);
                if (spot.found) break;
            }
        }
        // Went under ground or water, or came back up. Only the moment it's
        // lost, every look would cost too much. And once more when a floor's
        // picture wasn't in memory then and is now.
        bool justLost = m_last.found || (loaded && seconds < m_nearUntil);
        if (!spot.found && there && justLost) spot = otherLayer(mini, m_last, m_zooms);
        // While it's followed the other layers get a look now and then. And
        // at once when it fits a lot worse than at the last look there:
        // after a teleport under water the game shows the surface first and
        // fades to the floor.
        if (spot.found && (seconds >= m_layersAt || spot.score < m_layerScore - LAYER_DROP)) {
            m_layerScore = spot.score;
            spot = bestLayer(mini, spot, m_zooms);
            m_layersAt = seconds + LAYERS_EVERY;
        }
        return spot;
    }

    // A minimap that just came up (loading screen over, menu closed) is
    // searched for right away. With somewhere to look first the quick places
    // get their turns before that, the minimap can still be fading in.
    void Watcher::cameUp(double seconds) {
        SearchSoon(seconds);
        if (m_area.mapId != 0 || !m_places.empty()) m_searchAt = seconds + SEARCH_FIRST;
    }

    // The quick places, for a minimap that isn't around the last spot:
    // further around it, the places it was followed at, and the map
    // matching. One miss shouldn't cost seconds of searching the whole map.
    Spot Watcher::quick(const cv::Mat& grab, const cv::Mat& mini, double seconds) {
        Spot found;
        if (m_last.mapId != 0) found = nearbyOn(mini, m_last, true, m_last.zoom);
        // Where it was followed before. Leaving a domain puts the player
        // back at its door, and a wrong find in between has taken the last
        // spot's place.
        for (size_t i = m_places.size(); i-- > 0 && !found.found;) {
            if (!samePlace(m_places[i], m_last)) found = nearbyOn(mini, m_places[i], true, m_places[i].zoom);
        }
        // What a search found for a minimap before this one. This one can
        // be the first after it that isn't half faded.
        if (!found.found && m_answered.mapId != 0) {
            found = nearbyOn(mini, m_answered, true, m_answered.zoom);
            if (found.found) keep(found.zoom, seconds);
        }
        if (!found.found) found = locate(grab, mini, true, seconds);
        if (found.found) found = bestLayer(mini, found, m_zooms);
        return found;
    }

    // What a search found, and when the next one is due. Its answer is to
    // the minimap it was asked about, the player went on since.
    Spot Watcher::taken(const Answer& answer, const cv::Mat& mini, bool there, double seconds, bool* searched) {
        Spot found = answer.spot;
        if (found.found && !same(mini, answer.search.mini)) {
            // kept for the quick places, one look that doesn't fit doesn't throw it away
            m_answered = found;
            found = nearbyOn(mini, found, true, found.zoom);
        }
        if (found.found) {
            keep(found.zoom, seconds);
            if (there) found = bestLayer(mini, found, m_zooms);
        }
        if (!answer.search.due) return found;

        if (searched) *searched = true;
        if (answer.search.floors) m_floorsAt = seconds + FLOORS_EVERY;
        if (!answer.search.everywhere) {
            // that was around the last spot, the whole map once it isn't Near anymore
            m_searchAt = m_nearUntil;
        }
        else {
            // counted from now, the search itself can take as long as the wait
            m_searchWait = std::min(m_searchWait * 2.0, SEARCH_LAST);
            m_searchAt = seconds + m_searchWait;
            if (!found.found) m_empty = true;
        }
        return found;
    }

    // which State it is when the minimap isn't found
    State Watcher::lost(bool there, double seconds) const {
        if (!there) return State::None;
        if (seconds < m_nearUntil) return State::Near;
        return m_empty ? State::Nowhere : State::Searching;
    }

    // What to search for now, by the State. Nothing if neither due nor area
    // is set in what comes back.
    Watcher::Search Watcher::wanted(State state, const cv::Mat& mini, bool there, double seconds) const {
        Search search;
        search.mini = mini;
        search.last = m_last;
        search.zooms = m_zooms;
        search.there = there;
        search.due = seconds >= m_searchAt;
        // The part the big map showed, once for every new minimap and with
        // every search that's due. A teleport ends somewhere in there.
        if (there && (search.due || !same(mini, m_areaAsked))) search.area = m_area;
        if (!search.due) return search;

        switch (state) {
        case State::Near:
            search.around = true;
            break;
        case State::Searching:
        case State::Nowhere:
            search.around = m_last.mapId != 0;
            search.everywhere = true;
            search.floors = seconds >= m_floorsAt;
            break;
        case State::None:
            // No N and no Paimon, and they weren't seen going away either.
            // It can still be a minimap, so the whole maps get a look, at
            // the zoom out in the world only.
            search.everywhere = seconds >= m_nearUntil;
            break;
        case State::Found:
            break;
        }
        return search;
    }

    const Spot& Watcher::Look(const cv::Mat& grab, double seconds, bool* searched) {
        if (searched) *searched = false;
        later = m_loader.get();
        cv::Mat mini = shrink(grab);
        bool sign = shown(mini);
        if (sign) m_covered = false;
        else if (m_sign) m_covered = true;
        m_sign = sign;
        if (m_covered) {
            m_there = false;
            drop();
            return unfound(State::None, seconds);
        }
        // nothing moved, not even the arrow
        if (m_last.found && same(mini, m_lastMini)) {
            m_foundAt = seconds;
            return m_last;
        }
        m_lastMini = mini;

        // a picture the looks before went without is in memory now
        bool loaded = m_loader && m_loader->Done() != m_loaded;
        if (loaded) {
            m_loaded = m_loader->Done();
            m_missed.release();
        }

        Spot spot = around(mini, sign, seconds, loaded);
        // on from the look before, not a find out of nowhere
        bool followed = spot.found && m_last.found;
        bool there = spot.found || sign;
        if (there && !m_there) cameUp(seconds);
        m_there = there;

        // every look that shows something new
        if (!spot.found && there && !same(mini, m_missed)) {
            Spot found = quick(grab, mini, seconds);
            if (found.found) spot = found;
            else m_missed = mini;
        }

        if (!spot.found && m_last.found) {
            m_nearUntil = seconds + NEAR_FOR;
            // It went away: a menu, or the big map before its zoom bar is
            // there. Nothing to search for, and a whole map search right
            // then takes the cpu the big map needs to get found.
            if (!there) m_searchAt = std::max(m_searchAt, seconds + SEARCH_FIRST);
        }

        Answer answer;
        if (!spot.found && answered(answer)) {
            Spot found = taken(answer, mini, there, seconds, searched);
            if (found.found) spot = found;
        }

        State state = lost(there, seconds);
        // with a minimap on screen these two were tried as quick places
        if (!spot.found && state == State::None && seconds >= m_searchAt) {
            if (m_last.mapId != 0) spot = nearbyOn(mini, m_last, true, m_last.zoom);
            if (!spot.found) spot = locate(grab, mini, false, seconds);
            if (spot.found && searched) *searched = true;
        }

        // The rest takes long, so it's asked for and Look goes on. One
        // search at a time.
        if (!spot.found && !Searching()) {
            Search search = wanted(state, mini, there, seconds);
            if (search.due || search.area.mapId != 0) {
                if (search.area.mapId != 0) m_areaAsked = mini;
                ask(search);
                // without a thread it's done already
                if (answered(answer)) {
                    Spot found = taken(answer, mini, there, seconds, searched);
                    if (found.found) spot = found;
                }
            }
        }

        if (!spot.found) {
            // maybe it shows a floor we don't have the picture of yet
            if (m_loader && there && seconds >= m_fetchAt) {
                m_fetchAt = seconds + FETCH_EVERY;
                Spot at = m_last;
                m_loader->Ask("floors", [at](const std::atomic<bool>& quit) { return fetchFloors(at.mapId, at.lat, at.lng, quit); });
            }
            m_last.score = spot.score;
            return unfound(lost(there, seconds), seconds);
        }

        // A new zoom stays once it lasted. When another zoom took over
        // before that, the game was still zooming.
        if (m_newZoom != 0.0 && spot.zoom != m_newZoom) forgetNew();
        else if (seconds - m_newZoomAt >= ZOOM_STAYS) m_newZoom = 0.0;
        if (followed && spot.zoom != m_newZoom) Remember(spot);
        m_last = spot;
        m_last.state = State::Found;
        m_last.age = 0.0;
        m_foundAt = seconds;
        m_searchWait = SEARCH_FIRST;
        m_searchAt = 0.0;
        m_floorsAt = 0.0;
        m_empty = false;
        m_area = Area();
        m_answered = Spot();
        m_missed.release();
        m_areaAsked.release();
        drop();
        return m_last;
    }

    // not found at this look, the spot stays where it was last seen
    const Spot& Watcher::unfound(State state, double seconds) {
        m_last.found = false;
        m_last.state = state;
        m_last.age = m_last.mapId != 0 ? seconds - m_foundAt : 0.0;
        return m_last;
    }

    // OpenCV runs its loops on the scheduler of the thread that calls it
    OneThread::OneThread() {
        Concurrency::CurrentScheduler::Create(Concurrency::SchedulerPolicy(2, Concurrency::MinConcurrency, 1, Concurrency::MaxConcurrency, 1));
    }

    OneThread::~OneThread() {
        Concurrency::CurrentScheduler::Detach();
    }

    // The places of the last run. The game starts the player where they
    // logged out, so mostly one of them is right and the first look has it.
    static std::vector<Spot> readPlaces() {
        std::vector<Spot> places;
        std::ifstream file(folder / "places.tsv");
        Spot place;
        while (file >> place.mapId >> place.groupId >> place.floorId >> place.lat >> place.lng >> place.zoom) places.push_back(place);
        return places;
    }

    static std::string placesText(const std::vector<Spot>& places) {
        std::ostringstream text;
        text.precision(10);
        for (const Spot& place : places) {
            text << place.mapId << "\t" << place.groupId << "\t" << place.floorId << "\t" << place.lat << "\t" << place.lng << "\t" << place.zoom << "\n";
        }
        return text.str();
    }

    // when the player was last found, guarded by mutex like player
    static double seenAt = 0.0;

    // no minimap to read: the big map is open, or another window is in front
    static void hide(Spot& spot) {
        spot.found = false;
        spot.state = State::None;
        if (spot.mapId != 0) spot.age = now() - seenAt;
    }

    static void loop() {
        OneThread alone;
        Watcher watcher(MapTracking::Locate, true);
        for (const Spot& place : readPlaces()) watcher.Remember(place);
        std::string written = placesText(watcher.Places());
        bool wasOpen = false;
        double placesAt = 0.0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (wake.wait_for(lock, EVERY, []() { return stopping; })) return;
            }
            double seconds = now();
            if (seconds - askedAt.load() > ASKED_FOR) continue;
            // The map data is still being checked, or downloaded on the
            // first start. A whole map made now would be made for nothing.
            if (MapTracking::GetStatus() == MapTracking::Status::Downloading) continue;
            if (grids.empty()) Load(folder);
            if (grids.empty()) continue;
            // in another window there's nothing to draw over
            if (!MapTracking::GameInFront()) {
                std::lock_guard<std::mutex> lock(mutex);
                hide(player);
                continue;
            }
            // the big map covers the minimap, no spot until it closes
            if (MapTracking::IsMapOpen()) {
                wasOpen = true;
                std::lock_guard<std::mutex> lock(mutex);
                hide(player);
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
            bool looked = false;
            Spot spot;
            if (game && !IsIconic(game) && Capture::getClientRectOnScreen(game, client) && Capture::grabGame(game, rect(client), grab) && !grab.empty()) {
                try {
                    spot = watcher.Look(grab, seconds);
                    looked = true;
                }
                catch (...) {
                    // a look that goes wrong is skipped
                }
            }
            // nothing was looked at, the spot is the last look's and can be old
            if (!looked) {
                std::lock_guard<std::mutex> lock(mutex);
                hide(player);
                continue;
            }
            if (spot.found && seconds >= placesAt) {
                placesAt = seconds + PLACES_EVERY;
                std::string text = placesText(watcher.Places());
                if (text != written) MapData::writeFile(folder / "places.tsv", text);
                written = text;
            }
            spot.square = rect(client);
            std::lock_guard<std::mutex> lock(mutex);
            player = spot;
            if (spot.found) seenAt = seconds;
        }
    }

    void Start(const std::filesystem::path& dataFolder) {
        if (worker.joinable()) return;
        // loaded by the thread, once the map data was checked
        folder = dataFolder;
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
        if (MapTracking::IsMapOpen() || !MapTracking::GameInFront()) hide(player);
        return player;
    }
}
