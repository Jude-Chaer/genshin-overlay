#include "map_follow.hpp"
#include "settings.hpp"
#include "capture.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MapFollow {
    // a patch whose phase correlation is less sure than this (0 to 1) is dropped
    static constexpr double MIN_CONFIDENCE = 0.06;
    // patches that have to agree, so a panel or a button over one or two doesn't count
    static constexpr int MIN_AGREE = 3;
    // how far off from the others a patch can be and still agree, in compared pixels
    static constexpr double AGREE_PIXELS = 1.5;
    // search: a patch has to look this much like the spot it was found at (0 to 1)
    static constexpr double MIN_FOUND = 0.6;
    // search: patches flatter than this (spread of the gray, 0 to 255) are skipped, sea or fog
    static constexpr double MIN_DETAIL = 4.0;
    // The compare has to land this close to what the search saw, in shrunk
    // pixels. Further off it wrapped around or had nothing left to compare,
    // and the search's answer is taken.
    static constexpr double WIDE_OFF = 2.0;

    // Five squares spread over the map like the dots on a dice. Clear of the
    // buttons and lists around the edges, they don't move with the map.
    std::vector<RECT> patchRects(const RECT& game) {
        int w = game.right - game.left;
        int h = game.bottom - game.top;
        int side = h * 3 / 10;
        int left = game.left + w / 5, right = game.right - w / 5 - side;
        int top = game.top + h * 12 / 100, bottom = game.bottom - h * 12 / 100 - side;
        int middleX = game.left + (w - side) / 2, middleY = game.top + (h - side) / 2;
        std::vector<RECT> rects;
        for (POINT at : { POINT{ left, top }, POINT{ right, top }, POINT{ left, bottom }, POINT{ right, bottom }, POINT{ middleX, middleY } }) {
            rects.push_back({ at.x, at.y, at.x + side, at.y + side });
        }
        return rects;
    }

    // Open sea is nearly one flat color. Phase correlation only finds the fog
    // on it, which stays put on screen, and says the map didn't move. Patches
    // flatter than this (spread of the gray, 0 to 255) are left empty.
    static constexpr double FLAT_PATCH = 8.0;

    Patches shrink(const std::vector<cv::Mat>& gray) {
        // Fades the edges out, what slides in or out there would throw the
        // compare off. Done once here: phaseCorrelate's own window is written
        // into the patches it's given, and a patch is compared more than once.
        static cv::Mat window;
        if (window.empty()) cv::createHanningWindow(window, cv::Size(SIZE, SIZE), CV_32F);
        Patches out;
        for (const cv::Mat& patch : gray) {
            cv::Mat small;
            cv::resize(patch, small, cv::Size(SIZE, SIZE), 0, 0, cv::INTER_AREA);
            cv::Scalar mean, spread;
            cv::meanStdDev(small, mean, spread);
            if (Settings::ignoreSea && spread[0] < FLAT_PATCH) {
                out.push_back(cv::Mat());
                continue;
            }
            small.convertTo(small, CV_32F);
            out.push_back(small.mul(window));
        }
        return out;
    }

    bool grab(HWND window, const RECT& game, Patches& out, cv::Mat& wide, Capture::Frame& frame) {
        std::vector<cv::Mat> gray;
        std::vector<RECT> rects = patchRects(game);
        // the patches get shrunk to SIZE anyway, most of the way on the GPU
        int shrunk = Settings::shrinkOnGpu ? (int)rects.size() : 0;
        if (!Capture::grabGameGray(window, rects, gray, frame, game, WIDE_WIDTH, wide, shrunk, SIZE)) return false;
        out = shrink(gray);
        return true;
    }

    // halved like the GPU does it, so the two look the same to the search
    cv::Mat cutWide(const cv::Mat& bgr) {
        int level = 0;
        while ((bgr.cols >> level) > WIDE_WIDTH) level++;
        cv::Mat gray, wide;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::resize(gray, wide, cv::Size(std::max(1, bgr.cols >> level), std::max(1, bgr.rows >> level)), 0, 0, cv::INTER_AREA);
        return wide;
    }

    Patches cut(const cv::Mat& bgr, const RECT& game) {
        std::vector<cv::Mat> gray;
        for (const RECT& rect : patchRects(game)) {
            cv::Mat patch;
            cv::cvtColor(bgr(cv::Rect(rect.left - game.left, rect.top - game.top, rect.right - rect.left, rect.bottom - rect.top)),
                patch, cv::COLOR_BGR2GRAY);
            gray.push_back(patch);
        }
        return shrink(gray);
    }

    // one patch and how far it moved, in screen pixels
    struct Seen {
        cv::Point2d at;     // middle of the patch, from the middle of the game
        cv::Point2d moved;
    };

    // Least squares of moved = (zoom - 1) * at + slide. A zoom pushes the patches
    // away from where it zooms around, a slide moves them all the same.
    static Motion fit(const std::vector<Seen>& seen) {
        double n = (double)seen.size(), spread = 0.0, along = 0.0;
        cv::Point2d sumAt, sumMoved;
        for (const Seen& s : seen) {
            sumAt += s.at;
            sumMoved += s.moved;
            spread += s.at.dot(s.at);
            along += s.at.dot(s.moved);
        }
        double below = spread - sumAt.dot(sumAt) / n;
        double k = below > 1e-6 ? (along - sumAt.dot(sumMoved) / n) / below : 0.0;
        Motion motion;
        motion.found = true;
        motion.zoom = 1.0 + k;
        motion.moved = (sumMoved - sumAt * k) * (1.0 / n);
        return motion;
    }

    // Every three patches make a guess, the one most patches agree with wins.
    // A panel over two patches says the map didn't move, the other three outvote it.
    static Motion agreed(const std::vector<Seen>& seen, double within) {
        auto agreeing = [&](const Motion& motion) {
            std::vector<Seen> out;
            for (const Seen& s : seen) {
                cv::Point2d expected = s.at * (motion.zoom - 1.0) + motion.moved;
                if (std::hypot(s.moved.x - expected.x, s.moved.y - expected.y) <= within) out.push_back(s);
            }
            return out;
        };
        std::vector<Seen> best;
        for (size_t i = 0; i < seen.size(); ++i) {
            for (size_t j = i + 1; j < seen.size(); ++j) {
                for (size_t k = j + 1; k < seen.size(); ++k) {
                    std::vector<Seen> agree = agreeing(fit({ seen[i], seen[j], seen[k] }));
                    if (agree.size() > best.size()) best = agree;
                }
            }
        }
        if ((int)best.size() < MIN_AGREE) return {};
        Motion motion = fit(best);
        if (agreeing(motion).size() < best.size()) return {};
        return motion;
    }

    Motion compare(const Patches& from, const Patches& to, const RECT& game, const Motion& expected) {
        std::vector<RECT> rects = patchRects(game);
        if (from.size() != rects.size() || to.size() != rects.size()) return {};

        double scale = (double)(rects[0].right - rects[0].left) / SIZE;
        cv::Point2d middle((game.left + game.right) / 2.0, (game.top + game.bottom) / 2.0);
        // Phase correlation only sees a slide up to half a patch, anything
        // further wraps around: 70 pixels to the right comes out as 58 to the
        // left. Under a quarter patch the answer is taken as it is. Above that
        // it's one of the two, and the one closer to expected wins. With
        // nothing expected the patch is dropped.
        auto unwrap = [&](double& shift, double guess) {
            if (std::abs(shift) < SIZE / 4) return true;
            if (!expected.found) return false;
            double other = shift - std::copysign(SIZE, shift);
            if (std::abs(other - guess) < std::abs(shift - guess)) shift = other;
            return true;
        };
        std::vector<Seen> seen;
        for (size_t i = 0; i < rects.size(); ++i) {
            if (from[i].empty() || to[i].empty()) continue;
            double confidence = 0.0;
            cv::Point2d shift = cv::phaseCorrelate(from[i], to[i], cv::noArray(), &confidence);
            if (confidence < MIN_CONFIDENCE) continue;
            cv::Point2d at = cv::Point2d((rects[i].left + rects[i].right) / 2.0, (rects[i].top + rects[i].bottom) / 2.0) - middle;
            cv::Point2d guess = (at * (expected.zoom - 1.0) + expected.moved) * (1.0 / scale);
            if (!unwrap(shift.x, guess.x) || !unwrap(shift.y, guess.y)) continue;
            seen.push_back({ at, shift * scale });
        }

        double within = AGREE_PIXELS * scale;
        auto slideOf = [&](const Seen& a, const Seen& b, Motion& slide) {
            slide.found = true;
            slide.moved = (a.moved + b.moved) * 0.5;
            return std::hypot(a.moved.x - slide.moved.x, a.moved.y - slide.moved.y) <= within;
        };
        // With the flat ones out there can be only two left that see anything.
        // Two can always be fit with some slide and zoom, so they only count
        // if both slid the same.
        auto flat = [](const cv::Mat& patch) { return patch.empty(); };
        bool flatLeft = std::any_of(from.begin(), from.end(), flat) || std::any_of(to.begin(), to.end(), flat);
        Motion slide;
        if (flatLeft && seen.size() == 2 && slideOf(seen[0], seen[1], slide)) return slide;

        Motion motion = agreed(seen, within);
        if (motion.found || seen.size() < 3) return motion;
        // Over the sea some patches only see the fog, which stands still, and
        // a fast drag blurs others. Two that agree on a slide are enough then,
        // as long as it isn't standing still and no other two agree on
        // another slide.
        std::vector<Motion> slides;
        for (size_t i = 0; i < seen.size(); ++i) {
            for (size_t j = i + 1; j < seen.size(); ++j) {
                if (!slideOf(seen[i], seen[j], slide)) continue;
                if (std::hypot(slide.moved.x, slide.moved.y) <= 2 * within) continue;
                int agree = 0;
                for (const Seen& s : seen) {
                    if (std::hypot(s.moved.x - slide.moved.x, s.moved.y - slide.moved.y) <= within) agree++;
                }
                if (agree == 2) slides.push_back(slide);
            }
        }
        return slides.size() == 1 ? slides[0] : Motion();
    }

    Motion search(const cv::Mat& from, const cv::Mat& to, const RECT& game) {
        if (from.empty() || from.size() != to.size()) return {};
        double scale = (double)(game.right - game.left) / from.cols;
        cv::Point2d middle((game.left + game.right) / 2.0, (game.top + game.bottom) / 2.0);
        // the top of a peak between three pixels
        auto between = [](float before, float at, float after) {
            double bend = before - 2.0 * at + after;
            return bend < -1e-6 ? std::clamp(0.5 * (before - after) / bend, -0.5, 0.5) : 0.0;
        };
        std::vector<Seen> seen;
        cv::Mat like;
        for (const RECT& rect : patchRects(game)) {
            cv::Rect in(cvRound((rect.left - game.left) / scale), cvRound((rect.top - game.top) / scale),
                cvRound((rect.right - rect.left) / scale), cvRound((rect.bottom - rect.top) / scale));
            in &= cv::Rect(0, 0, from.cols, from.rows);
            if (in.width < 8 || in.height < 8) continue;
            cv::Mat piece = from(in);
            cv::Scalar mean, detail;
            cv::meanStdDev(piece, mean, detail);
            if (detail[0] < MIN_DETAIL) continue;

            cv::matchTemplate(to, piece, like, cv::TM_CCOEFF_NORMED);
            double best = 0.0;
            cv::Point at;
            cv::minMaxLoc(like, nullptr, &best, nullptr, &at);
            if (best < MIN_FOUND) continue;
            cv::Point2d found = at;
            if (at.x > 0 && at.x < like.cols - 1) found.x += between(like.at<float>(at.y, at.x - 1), (float)best, like.at<float>(at.y, at.x + 1));
            if (at.y > 0 && at.y < like.rows - 1) found.y += between(like.at<float>(at.y - 1, at.x), (float)best, like.at<float>(at.y + 1, at.x));
            cv::Point2d middleOf = cv::Point2d((rect.left + rect.right) / 2.0, (rect.top + rect.bottom) / 2.0) - middle;
            seen.push_back({ middleOf, (found - cv::Point2d(in.tl())) * scale });
        }
        return agreed(seen, AGREE_PIXELS * scale);
    }

    MapLocator::Result moveView(const MapLocator::Result& view, const Motion& motion) {
        // what's in the middle now was moved / zoom away from the middle before
        MapLocator::Result out = view;
        out.lng -= motion.moved.x / motion.zoom * view.unitsPerPixel;
        out.lat -= motion.moved.y / motion.zoom * view.unitsPerPixel;
        out.unitsPerPixel = view.unitsPerPixel / motion.zoom;
        return out;
    }

    // what moveView would need to go from one view to the other
    static Motion motionBetween(const MapLocator::Result& from, const MapLocator::Result& to) {
        Motion motion;
        motion.found = true;
        motion.zoom = from.unitsPerPixel / to.unitsPerPixel;
        motion.moved.x = (from.lng - to.lng) * motion.zoom / from.unitsPerPixel;
        motion.moved.y = (from.lat - to.lat) * motion.zoom / from.unitsPerPixel;
        return motion;
    }

    // one motion after the other
    static Motion chain(const Motion& first, const Motion& then) {
        Motion motion;
        motion.found = true;
        motion.zoom = first.zoom * then.zoom;
        motion.moved = first.moved * then.zoom + then.moved;
        return motion;
    }

    // about the same motion, over this many frames instead of one
    static Motion times(const Motion& motion, double frames) {
        Motion out = motion;
        out.moved = motion.moved * frames;
        out.zoom = std::pow(motion.zoom, frames);
        return out;
    }

    // A still map wobbles a bit from grab to grab (up to about a pixel, it has
    // little animations), so one grab can't tell if it moves. It's judged by
    // how far it went in the last STILL_FOR seconds, a share of the game's
    // height: it starts moving above START_MOVE and stops below STILL_MOVE.
    // After letting go the map glides on slowly for a bit, that falls in the
    // gap between the two, so it doesn't flip back and forth.
    static constexpr double START_MOVE = 0.003;
    static constexpr double START_ZOOM = 0.004;
    static constexpr double STILL_MOVE = 0.001;
    static constexpr double STILL_ZOOM = 0.002;
    static constexpr double STILL_FOR = 0.1;
    // standing still, what's shown follows the map by this share every frame,
    // which smooths out the wobble and still keeps up with the slow glide
    static constexpr double REST_FOLLOW = 0.2;
    // seconds from the map stopping to asking SIFT to land it
    static constexpr double SETTLE_AFTER = 0.05;
    // Standing still, the patches changing by more than this (gray levels, on
    // average) for this many seconds means another floor or layer was picked.
    // Fog and pins moving stay far below.
    static constexpr double LOOKS_DIFFERENT = 6.0;
    static constexpr double DIFFERENT_FOR = 0.15;
    // seconds between two SIFTs asked for that
    static constexpr double LOOK_AGAIN_EVERY = 0.5;
    // seconds after a stop until it counts as landed even if SIFT didn't find it
    static constexpr double LAND_WAIT = 0.6;
    // seconds between SIFT's looks while the map isn't found
    static constexpr double LOOK_INTERVAL = 0.12;
    // the speed is measured over this many seconds, one frame to the next is too jumpy
    static constexpr double SPEED_OVER = 0.05;
    // Every compare is off by a bit, adding them up frame after frame drifts
    // on a slow drag. So frames are compared with a key frame, and a new key
    // is only taken once the map moved this far from it (a share of the
    // game's height, and log of the zoom).
    static constexpr double KEY_MOVE = 0.04;
    static constexpr double KEY_ZOOM = 0.04;
    // frames remembered for answers that come later, a few seconds of them
    static constexpr size_t PAST_KEPT = 300;
    // seconds guessed ahead at most, further than that the map may have
    // stopped or turned
    static constexpr double MAX_AHEAD = 0.1;
    // The game zooms in steps, a wheel notch is done within a frame. There's
    // no going on the way it moves then: zooming faster than this (log of the
    // zoom in a second) nothing is guessed.
    static constexpr double AHEAD_ZOOM = 0.2;
    // SIFT's fixes up to this far (share of the game's height) slide in over
    // GLIDE seconds. Bigger ones jump, sliding in they'd look like the map moving.
    static constexpr double GLIDE_MAX = 0.03;
    static constexpr double GLIDE = 0.2;
    // share of each tile check's fix that is taken, they come every frame and are a bit noisy
    static constexpr double CHECK_PART = 0.25;

    // how much the patches of two frames differ, in gray levels
    static double differ(const Patches& a, const Patches& b) {
        double sum = 0.0;
        int count = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            if (a[i].empty() || a[i].size() != b[i].size()) continue;
            // the patches are faded out to the edges, on average to a quarter
            sum += cv::norm(a[i], b[i], cv::NORM_L1) / (0.25 * a[i].total());
            count++;
        }
        return count ? sum / count : 0.0;
    }

    void Follower::Reset() {
        *this = Follower();
    }

    // how far apart two views are in pixels, and how much they're zoomed
    static double pixelsApart(const MapLocator::Result& a, const MapLocator::Result& b) {
        return std::hypot(a.lng - b.lng, a.lat - b.lat) / b.unitsPerPixel;
    }
    static double zoomApart(const MapLocator::Result& a, const MapLocator::Result& b) {
        return std::abs(std::log(a.unitsPerPixel / b.unitsPerPixel));
    }

    const Follower::Past* Follower::pastBefore(double seconds) const {
        for (auto at = m_past.rbegin(); at != m_past.rend(); ++at) {
            if (at->chain != m_chain) return nullptr;
            if (at->seconds <= seconds) return &*at;
        }
        return nullptr;
    }

    void Follower::Frame(int id, const Patches& patches, const cv::Mat& wide, const RECT& game, double seconds, int frames) {
        double height = game.bottom - game.top;
        bool sameSize = game.right - game.left == m_keyGame.right - m_keyGame.left && game.bottom - game.top == m_keyGame.bottom - m_keyGame.top;
        MapLocator::Result before = m_view;
        bool found = false, newKey = true;
        if (!m_key.empty() && sameSize) {
            // Where the last frame's patches went says about how it moved. If
            // they weren't found (sea, a menu) the map is taken to go on the
            // way it went between the last two frames, unless the game
            // skipped more than a few.
            Motion rough = search(m_lastWide, wide, game);
            Motion step = rough.found ? rough : frames <= 3 ? times(m_step, frames) : Motion();
            Motion expected = chain(motionBetween(m_keyView, m_view), step);
            expected.found = step.found;
            double off = wide.empty() ? 0.0 : WIDE_OFF * (game.right - game.left) / wide.cols;
            auto fits = [&](const Motion& motion, const Motion& to) {
                if (!motion.found) return false;
                return !rough.found || std::hypot(motion.moved.x - to.moved.x, motion.moved.y - to.moved.y) <= off;
            };
            Motion motion = compare(m_key, patches, game, expected);
            if (fits(motion, expected)) {
                m_view = moveView(m_keyView, motion);
                found = true;
                newKey = std::hypot(motion.moved.x, motion.moved.y) >= KEY_MOVE * height || std::abs(std::log(motion.zoom)) >= KEY_ZOOM;
            }
            else {
                // moved too far from the key, the frame before is closer
                motion = m_lastIsKey ? Motion() : compare(m_last, patches, game, step);
                // too far for that one too, the search still saw where it went
                if (!fits(motion, rough)) motion = rough;
                if (motion.found) {
                    m_view = moveView(m_view, motion);
                    found = true;
                }
            }
        }

        m_seconds = seconds;
        m_height = height;
        if (found) {
            m_step = times(motionBetween(before, m_view), 1.0 / std::max(frames, 1));
        }
        else {
            m_step = {};
            // Lost the map: it moved or zoomed too fast, a menu opened, or
            // it's the first frame. A new chain starts and SIFT has to find it.
            m_chain++;
            m_anchored = false;
            m_moving = true;
            m_landed = false;
            if (m_view.unitsPerPixel <= 0.0) m_view.unitsPerPixel = 1.0;
        }

        if (newKey) {
            m_key = patches;
            m_keyView = m_view;
            m_keyGame = game;
        }
        m_last = patches;
        m_lastWide = wide;
        m_lastIsKey = newKey;
        const Past* back = pastBefore(seconds - SPEED_OVER);
        MapLocator::Result backView = back ? back->view : m_view;
        double backSeconds = back ? back->seconds : seconds;
        m_past.push_back({ id, m_chain, seconds, m_view });
        if (m_past.size() > PAST_KEPT) m_past.pop_front();
        m_perSecond = {};
        if (!found) return;

        if (const Past* then = pastBefore(seconds - STILL_FOR)) {
            double apart = pixelsApart(m_view, then->view) / height;
            double zoom = zoomApart(m_view, then->view);
            if (m_moving && apart < STILL_MOVE && zoom < STILL_ZOOM) {
                m_moving = false;
                m_rest = m_view;
                m_stoppedAt = seconds;
                m_settled = false;
            }
            else if (!m_moving && (apart >= START_MOVE || zoom >= START_ZOOM)) {
                if (Landed(seconds)) {
                    // It stood where SIFT put it. What the compare saw drift
                    // by meanwhile doesn't count, only how it moved since it
                    // started to.
                    MapLocator::Result started = moveView(m_rest, motionBetween(then->view, m_view));
                    MapLocator::Result was = m_view;
                    auto rebase = [&](MapLocator::Result& view) {
                        double zoom = started.unitsPerPixel / was.unitsPerPixel;
                        view.lng = started.lng + (view.lng - was.lng) * zoom;
                        view.lat = started.lat + (view.lat - was.lat) * zoom;
                        view.unitsPerPixel *= zoom;
                    };
                    for (Past& past : m_past) {
                        if (past.chain == m_chain) rebase(past.view);
                    }
                    rebase(m_view);
                    rebase(m_keyView);
                }
                m_moving = true;
                m_landed = false;
            }
        }
        // Once SIFT put it down it stays put. Over the sea the fog is animated and
        // slides slowly, the compare follows the fog and would drift off forever.
        if (!m_moving && !Landed(seconds)) {
            m_rest.lng += (m_view.lng - m_rest.lng) * REST_FOLLOW;
            m_rest.lat += (m_view.lat - m_rest.lat) * REST_FOLLOW;
            m_rest.unitsPerPixel += (m_view.unitsPerPixel - m_rest.unitsPerPixel) * REST_FOLLOW;
        }

        if (m_moving && back && seconds - backSeconds < 2 * SPEED_OVER) {
            m_perSecond = times(motionBetween(backView, m_view), 1.0 / (seconds - backSeconds));
        }

        // Going underground or picking another floor changes the map without
        // moving it, the compare sees nothing. SIFT gets to look again then.
        if (m_moving || !m_settled) {
            m_landedLook.clear();
            m_differentSince = -1.0;
        }
        else if (m_landedLook.empty()) {
            m_landedLook = patches;
        }
        else if (differ(m_landedLook, patches) < LOOKS_DIFFERENT) {
            m_differentSince = -1.0;
        }
        else if (m_differentSince < 0.0) {
            m_differentSince = seconds;
        }
        else if (Settings::lookAgainStill && seconds - m_differentSince >= DIFFERENT_FOR && seconds - m_lookedAgainAt >= LOOK_AGAIN_EVERY) {
            m_settled = false;
            m_lookedAgainAt = seconds;
        }
    }

    bool Follower::fixFrom(int id, const MapLocator::Result& result) {
        auto at = std::find_if(m_past.begin(), m_past.end(), [&](const Past& past) { return past.id == id; });
        // too old, or the compare lost the map since
        if (at == m_past.end() || at->chain != m_chain) return false;

        // The view was off by this much on that frame, so it's off by the same
        // now. Everything in the same chain gets fixed.
        MapLocator::Result was = at->view;
        auto fix = [&](MapLocator::Result& view) {
            double x = (view.lng - was.lng) / was.unitsPerPixel;
            double y = (view.lat - was.lat) / was.unitsPerPixel;
            double zoom = view.unitsPerPixel / was.unitsPerPixel;
            MapLocator::Result fixed = result;
            fixed.lng = result.lng + x * result.unitsPerPixel;
            fixed.lat = result.lat + y * result.unitsPerPixel;
            fixed.unitsPerPixel = result.unitsPerPixel * zoom;
            view = fixed;
        };
        for (Past& past : m_past) {
            if (past.chain == m_chain) fix(past.view);
        }
        fix(m_view);
        fix(m_keyView);
        fix(m_rest);
        return true;
    }

    void Follower::Checked(int id, const MapLocator::Result& result) {
        // standing still SIFT lands it, fixing it every frame just shakes it
        if (!m_anchored || !m_moving) return;
        // SIFT may have found another floor or map since that frame, the
        // check would put the old one back
        if (result.mapId != m_view.mapId || result.groupId != m_view.groupId) return;
        auto at = std::find_if(m_past.begin(), m_past.end(), [&](const Past& past) { return past.id == id; });
        if (at == m_past.end()) return;
        const MapLocator::Result& was = at->view;
        MapLocator::Result part = result;
        part.lng = was.lng + (result.lng - was.lng) * CHECK_PART;
        part.lat = was.lat + (result.lat - was.lat) * CHECK_PART;
        part.unitsPerPixel = was.unitsPerPixel * std::pow(result.unitsPerPixel / was.unitsPerPixel, CHECK_PART);
        fixFrom(id, part);
    }

    void Follower::Matched(int id, const MapLocator::Result& result, double seconds) {
        // what's on screen right now, the slide starts from there
        MapLocator::Result shown = Ahead(seconds);
        if (!fixFrom(id, result)) return;
        // the SIFT sent when it stopped came back, it lands right there without a slide
        bool landing = m_settled && !m_moving;
        if (landing) m_landed = true;

        m_glideAt = -1.0;
        if (m_anchored && !landing) {
            m_glideAt = seconds;
            m_glideLng = m_glideLat = 0.0;
            m_glideZoom = 1.0;
            MapLocator::Result fixed = Ahead(seconds);
            double off = std::hypot(shown.lng - fixed.lng, shown.lat - fixed.lat) / fixed.unitsPerPixel;
            double zoom = shown.unitsPerPixel / fixed.unitsPerPixel;
            if (off < GLIDE_MAX * m_height && std::abs(std::log(zoom)) < GLIDE_MAX) {
                m_glideLng = shown.lng - fixed.lng;
                m_glideLat = shown.lat - fixed.lat;
                m_glideZoom = zoom;
            }
        }
        m_anchored = true;
    }

    MapLocator::Result Follower::Ahead(double seconds) const {
        // standing still it stays put instead of wobbling
        MapLocator::Result view = m_moving ? m_view : m_rest;
        bool zooms = std::abs(std::log(m_perSecond.zoom)) >= AHEAD_ZOOM;
        if (m_perSecond.found && Settings::guessAhead && !(Settings::steadyZoom && zooms)) {
            // only where it slides to, the zoom stays as it was seen
            Motion slide = m_perSecond;
            if (Settings::steadyZoom) slide.zoom = 1.0;
            view = moveView(view, times(slide, std::clamp(seconds - m_seconds, 0.0, MAX_AHEAD)));
        }
        double left = m_glideAt < 0.0 ? 0.0 : 1.0 - (seconds - m_glideAt) / GLIDE;
        if (left > 0.0) {
            // eases out, fast at first
            left = std::min(left, 1.0);
            left *= left;
            view.lng += m_glideLng * left;
            view.lat += m_glideLat * left;
            view.unitsPerPixel *= std::pow(m_glideZoom, left);
        }
        return view;
    }

    bool Follower::Landed(double seconds) const {
        return m_anchored && !m_moving && (m_landed || seconds - m_stoppedAt >= LAND_WAIT);
    }

    bool Follower::WantsSettle(double seconds) const {
        return m_anchored && !m_moving && !m_settled && seconds - m_stoppedAt >= SETTLE_AFTER;
    }

    bool Follower::WantsLook(double seconds) const {
        return !m_anchored && seconds - m_lookedAt >= LOOK_INTERVAL;
    }
}
