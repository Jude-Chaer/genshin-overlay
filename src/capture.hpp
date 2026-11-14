#pragma once
#include <windows.h>
#include <vector>
#include <opencv2/core.hpp>

namespace Capture {
    // the Genshin window, or nullptr if it isn't running
    extern HWND findGameWindow();
    // the Genshin window, minimized or not, nullptr only if it isn't running
    extern HWND findRunningGame();
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
    // one that was grabbed. colour rects, if given, come out of the same frame
    // in colour as BGR, none if one of them is off the window.
    extern bool grabGameGray(HWND game, const std::vector<RECT>& rects, std::vector<cv::Mat>& out, Frame& frame,
        const RECT& whole, int maxWidth, cv::Mat& shrunk, int shrink = 0, int atLeast = 0,
        const std::vector<RECT>* colour = nullptr, std::vector<cv::Mat>* colourOut = nullptr);
    // The rect of a recent frame as BGRA, width wide, halved on the GPU first
    // so only a little more than that is read back. Never waits for the GPU:
    // each grab orders a copy of the newest frame and hands over the one
    // ordered before once the GPU is done with it. False while none is ready
    // or when it is of frame. Otherwise frame becomes the one handed over.
    extern bool grabGameScaled(HWND game, const RECT& rect, int width, cv::Mat& out, Frame& frame);
    // frees what grabGameScaled keeps on the GPU, for all widths or one
    extern void dropScaled();
    extern void dropScaled(int width);
    // Light is for while the map is closed and only the zoom bar is read:
    // Windows hands over a frame every 50 ms at most and no copy of the game
    // is kept. The capture starts out light.
    extern void setLight(bool light);
    // sleeps until the game hands over a new frame, or that long at most
    extern void waitForGameFrame(int milliseconds);
    extern void closeGameCapture();
    extern bool isCapturing();
}
