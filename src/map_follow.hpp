#pragma once
#include <windows.h>
#include <deque>
#include <vector>
#include <opencv2/core.hpp>
#include "capture.hpp"
#include "locator/map_locator.hpp"

// Follows the map while it moves, without SIFT. Five patches of each frame are
// compared with the same patches of an earlier frame (phase correlation), which
// tells how far the map slid and zoomed in between.
//
// Phase correlation only sees a slide up to half a patch. For bigger steps the
// patches of the last frame are also searched for in the whole game, shrunk
// small. That's rough, but it finds them anywhere on screen and tells the
// compare which way the map went.

namespace MapFollow {
    // The five patches of one frame, shrunk to SIZE and ready to compare.
    // An empty one was flat (open sea) and is skipped.
    using Patches = std::vector<cv::Mat>;

    // how the map moved between two frames
    struct Motion {
        bool found = false;
        cv::Point2d moved;  // screen pixels
        double zoom = 1.0;  // around the middle of the game, above 1 is zooming in
    };

    // side of a patch when it's compared, in pixels
    constexpr int SIZE = 128;
    // the whole game is searched at this width at most
    constexpr int WIDE_WIDTH = 240;

    // the five squares that get compared, in screen coords
    extern std::vector<RECT> patchRects(const RECT& game);
    // gray patches of any size to Patches
    extern Patches shrink(const std::vector<cv::Mat>& gray);

    // Grabs the patches and the whole game shrunk (wide) from one frame. Game
    // is its client rect on screen. False if the game drew no frame after
    // frame, see Capture::grabGameGray.
    extern bool grab(HWND window, const RECT& game, Patches& out, cv::Mat& wide, Capture::Frame& frame);
    // the same two, cut out of a BGR grab of the whole game
    extern Patches cut(const cv::Mat& bgr, const RECT& game);
    extern cv::Mat cutWide(const cv::Mat& bgr);

    // Where the patches of one shrunk game went in the next. Right to about a
    // shrunk pixel, however far they went.
    extern Motion search(const cv::Mat& from, const cv::Mat& to, const RECT& game);
    // How the map moved between two frames, to a fraction of a pixel. Not found
    // if fewer than three patches agree. Expected is about how it moved, if
    // known: a long slide one way and a short one the other look the same to
    // phase correlation, the one closer to expected is taken.
    extern Motion compare(const Patches& from, const Patches& to, const RECT& game, const Motion& expected = {});

    // the view after the map moved like that
    extern MapLocator::Result moveView(const MapLocator::Result& view, const Motion& motion);

    // Keeps where the map is. Frame moves the view by what the compare saw.
    // SIFT answers later and for an older frame: the view was off by that much
    // on that frame, so every view since is shifted the same.
    //
    // Not thread safe. MapTracking uses one on the follow thread and hands a
    // copy to the draw loop.
    class Follower {
    public:
        void Reset();

        // Call for every new frame of the game, in order. Matched names the
        // frame by id. Seconds is when the game drew it, frames how many it
        // drew since the last call.
        void Frame(int id, const Patches& patches, const cv::Mat& wide, const RECT& game, double seconds, int frames = 1);
        // SIFT found the map on that frame. Seconds is now.
        void Matched(int id, const MapLocator::Result& result, double seconds);

        // SIFT found the map and the compare has followed it since
        bool Visible() const { return m_anchored; }
        // where the map was on the last frame
        const MapLocator::Result& View() const { return m_view; }
        // Where the map is at that time if it keeps moving the way it does.
        // What's drawn gets on screen a few frames after the game drew what was
        // grabbed, so the draw loop asks for the time its picture shows up.
        MapLocator::Result Ahead(double seconds) const;
        // the map stopped and SIFT said exactly where
        bool Landed(double seconds) const;

        // The map isn't found and it's time for SIFT to look. Call Looking
        // when a frame was sent to it.
        bool WantsLook(double seconds) const;
        void Looking(double seconds) { m_lookedAt = seconds; }
        // look on the next frame instead of waiting
        void LookNow() { m_lookedAt = 0.0; }
        // The map just stopped, one SIFT lands it exactly. Call Settling when
        // a frame was sent to it.
        bool WantsSettle(double seconds) const;
        void Settling() { m_settled = true; }

    private:
        // a frame that went through Frame, and the view on it
        struct Past {
            int id;
            int chain;
            double seconds;
            MapLocator::Result view;
        };
        // the newest frame at least that old, nullptr if the map was lost since
        const Past* pastBefore(double seconds) const;
        // Result is where the map really was on that frame. Shifts every view
        // since by how far off it was. False if the frame is too old or the
        // map was lost since.
        bool fixFrom(int id, const MapLocator::Result& result);

        // Frames are compared with the key frame, and with the last one when
        // the map went too far from the key.
        Patches m_key, m_last;
        MapLocator::Result m_keyView;
        RECT m_keyGame = {};
        bool m_lastIsKey = false;
        // the whole game on the last frame, shrunk
        cv::Mat m_lastWide;

        // the view on the last frame, in SIFT's units once anchored, in made up ones before
        MapLocator::Result m_view;
        // when the game drew the last frame, and the game's height then
        double m_seconds = 0.0;
        double m_height = 0.0;
        // the last PAST_KEPT frames, oldest first
        std::deque<Past> m_past;
        // goes up whenever the compare loses the map, answers for frames of an older chain are dropped
        int m_chain = 0;

        // SIFT found the map and the compare didn't lose it since
        bool m_anchored = false;
        bool m_moving = true;
        // where it stopped, shown while it stands still
        MapLocator::Result m_rest;
        double m_stoppedAt = 0.0;
        // a SIFT to land it was sent since it stopped, and it came back
        bool m_settled = false;
        bool m_landed = false;
        // when SIFT last looked for a map that isn't found
        double m_lookedAt = 0.0;

        // the patches when SIFT landed it, and since when they look different (-1 if they don't)
        Patches m_landedLook;
        double m_differentSince = -1.0, m_lookedAgainAt = 0.0;

        // how the map moved in one frame lately, not found if it was lost
        Motion m_step;
        // how it moves in a second, not found if it was lost or stands still
        Motion m_perSecond;
        // SIFT's small fixes slide in instead of jumping. This is how far the
        // view was off in map units, it fades out from m_glideAt (-1 for none).
        double m_glideLng = 0.0, m_glideLat = 0.0, m_glideZoom = 1.0;
        double m_glideAt = -1.0;
    };
}
