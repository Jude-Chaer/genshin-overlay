#pragma once
#include <windows.h>
#include <opencv2/core.hpp>

namespace Capture {
    // the Genshin window, or nullptr if it isn't running
    extern HWND findGameWindow();
    extern bool getClientRectOnScreen(HWND window, RECT& out);

    // Part of the game window as BGR, rect in screen coords. Uses Windows Graphics
    // Capture, which sees only the game, so nothing on top of it gets in.
    // Safe to call from several threads, they share one capture.
    extern bool grabGame(HWND game, const RECT& rect, cv::Mat& out);
    extern void closeGameCapture();
    extern bool isCapturing();
}
