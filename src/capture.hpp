#pragma once
#include <windows.h>
#include <opencv2/core.hpp>

namespace Capture {
    // the Genshin window, or nullptr if it isn't running
    extern HWND findGameWindow();
    extern bool getClientRectOnScreen(HWND window, RECT& out);

    // Screenshot of a part of the screen as BGR. exclude is hidden from the
    // screenshot only (our own overlay), the user still sees it.
    extern bool grabScreen(const RECT& rect, HWND exclude, cv::Mat& out);
}
