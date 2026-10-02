#pragma once
#include <windows.h>
#include <vector>
#include <opencv2/core.hpp>

namespace Capture {
    // the Genshin window, or nullptr if it isn't running
    extern HWND findGameWindow();
    extern bool getClientRectOnScreen(HWND window, RECT& out);

    // which frame of the game a grab came from
    struct Frame {
        uint64_t number = 0;    // goes up with every frame the game draws
        double seconds = 0.0;   // when the game drew it, steady_clock time
    };

    // Part of the game window as BGR, rect in screen coords. Uses Windows Graphics
    // Capture, which sees only the game, so nothing on top of it gets in.
    // Safe to call from several threads, they share one capture. Frame, if
    // given, gets the frame it came from.
    extern bool grabGame(HWND game, const RECT& rect, cv::Mat& out, Frame* frame = nullptr);
    // Several parts of one frame as gray, rects in screen coords, and the rect
    // whole of the same frame in shrunk, halved on the GPU until it's at most
    // maxWidth wide. The first shrink rects are halved on the GPU too, as often
    // as they stay at least atLeast pixels a side.
    // False if the game drew nothing since frame. Otherwise frame becomes the
    // one that was grabbed.
    extern bool grabGameGray(HWND game, const std::vector<RECT>& rects, std::vector<cv::Mat>& out, Frame& frame,
        const RECT& whole, int maxWidth, cv::Mat& shrunk, int shrink = 0, int atLeast = 0);
    // sleeps until the game hands over a new frame, or that long at most
    extern void waitForGameFrame(int milliseconds);
    extern void closeGameCapture();
    extern bool isCapturing();
}
