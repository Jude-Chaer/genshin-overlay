#pragma once
#include <GL/glew.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <opencv2/core.hpp>
#include "screens.hpp"

// What GetScreenPicture gives: the game as WGC grabs it, of a screen that is
// up, with the screen's hidden parts black. Only frames the overlay saw the
// screen up on are used. One texture per screen and width, made at most once
// per turn of the draw loop and only from a new game frame, shared by every
// extension that asks. Freed when nobody asked for a second.
// Render thread only.

namespace ScreenPictures {
    // the width a picture has when the extension gives none
    inline const int DefaultWidth = 1280;
    // pictures kept at once, so asking for many widths can't fill the memory
    inline const size_t MaxPictures = 8;
    // of those, the ones one extension asked for in the last second
    inline const size_t MaxPerAsker = 2;

    enum class Refused { None, TooManyOwn, TooMany };

    struct Picture {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
        // the game frame it shows, it changes with every new picture
        uint64_t frame = 0;
    };

    // The screen's picture width wide (at most the game's width), a few game
    // frames old. Nothing (texture 0) when the screen isn't up, before its
    // first picture of this time it's up is ready, or when too many are kept
    // (refused says which limit). asker is the extension, nullptr for the
    // overlay, which has no limit of its own.
    Picture get(Screens::Screen screen, int width, const Screens::Up& up, const void* asker = nullptr, Refused* refused = nullptr);
    // a texture handed out right now
    bool owns(GLuint texture);
    // starts the next turn of the draw loop and frees what wasn't asked for
    void nextTurn();
    // pictures kept right now
    size_t count();
    // frees them all
    void clear();
    // stops making pictures, before the capture closes
    void stop();

    // How the newest picture width wide is taken (false when there's none newer than frame),
    // and how a texture is made, filled and freed. The tests run with no game and no GL.
    extern std::function<bool(int width, cv::Mat& bgra, uint64_t& frame)> grab;
    extern std::function<GLuint(const cv::Mat& bgra, GLuint texture)> upload;
    extern std::function<void(GLuint texture)> release;
    extern std::function<double()> seconds;
    // whether the screen was up on a game frame, Screens::shownOn
    extern std::function<bool(Screens::Screen screen, uint64_t frame)> shownOn;
}
