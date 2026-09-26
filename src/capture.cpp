#include "capture.hpp"
#include <dwmapi.h>
#include <iostream>
#include <opencv2/imgproc.hpp>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace Capture {
    HWND findGameWindow() {
        HWND window = FindWindowW(L"UnityWndClass", L"Genshin Impact");
        if (!window) {
            window = FindWindowW(L"UnityWndClass", L"原神");
        }
        return window;
    }

    bool getClientRectOnScreen(HWND window, RECT& out) {
        RECT client;
        if (!GetClientRect(window, &client)) return false;

        POINT topLeft = { client.left, client.top };
        POINT bottomRight = { client.right, client.bottom };
        ClientToScreen(window, &topLeft);
        ClientToScreen(window, &bottomRight);
        out = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
        return out.right > out.left && out.bottom > out.top;
    }

    bool grabScreen(const RECT& rect, HWND exclude, cv::Mat& out) {
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0) return false;

        // DwmFlush waits for a new frame so the affinity change is on screen first
        bool excluded = exclude && SetWindowDisplayAffinity(exclude, WDA_EXCLUDEFROMCAPTURE);
        if (excluded) {
            DwmFlush();
        }
        else if (exclude) {
            // the tracker would see our own drawings, say so once
            static bool warned = false;
            if (!warned) {
                std::cerr << "Couldn't hide the overlay from the screenshot (error " << GetLastError() << ")" << std::endl;
                warned = true;
            }
        }

        HDC screenDC = GetDC(nullptr);
        HDC memoryDC = CreateCompatibleDC(screenDC);

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screenDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = false;
        cv::Mat bgr;
        if (dib && bits) {
            HGDIOBJ old = SelectObject(memoryDC, dib);
            ok = BitBlt(memoryDC, 0, 0, w, h, screenDC, rect.left, rect.top, SRCCOPY | CAPTUREBLT) != FALSE;
            SelectObject(memoryDC, old);
            if (ok) {
                cv::Mat bgra(h, w, CV_8UC4, bits);
                cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
            }
        }

        if (dib) DeleteObject(dib);
        DeleteDC(memoryDC);
        ReleaseDC(nullptr, screenDC);
        if (excluded) SetWindowDisplayAffinity(exclude, WDA_NONE);

        if (!ok || bgr.empty()) return false;
        out = bgr;
        return true;
    }
}
