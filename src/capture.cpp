#include "capture.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <vector>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <opencv2/imgproc.hpp>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

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

    // The game only hands over a frame when it draws one, and the tracker and the
    // zoom bar detector each want one at their own pace. So the newest frame is
    // kept on the GPU and every grab copies its part out of that.
    static std::mutex gameMutex;
    static ComPtr<ID3D11Device> device;
    static ComPtr<ID3D11DeviceContext> context;
    static wd3d::IDirect3DDevice captureDevice{ nullptr };
    static wgc::GraphicsCaptureItem item{ nullptr };
    static wgc::Direct3D11CaptureFramePool pool{ nullptr };
    static wgc::GraphicsCaptureSession session{ nullptr };
    static winrt::Windows::Graphics::SizeInt32 poolSize{};
    static HWND capturedWindow = nullptr;
    static ComPtr<ID3D11Texture2D> latest;
    // While light, latest is the frame Windows handed over, held until the
    // next one comes. Otherwise it's our own copy (33 MB at 4K) and
    // latestIsOurs is set.
    static bool light = true;
    static wgc::Direct3D11CaptureFrame held{ nullptr };
    static bool latestIsOurs = false;
    // how often Windows hands over a frame while light
    static constexpr auto LIGHT_EVERY = std::chrono::milliseconds(50);
    static winrt::Windows::Graphics::SizeInt32 latestSize{};
    static ComPtr<ID3D11Texture2D> staging;
    // the whole game and its halves (mip levels), made on the GPU
    static ComPtr<ID3D11Texture2D> halves;
    static ComPtr<ID3D11ShaderResourceView> halvesView;
    static ComPtr<ID3D11Texture2D> smallStaging;
    // the same for the rects that get halved, side by side, and what copyShrunk reads back
    static ComPtr<ID3D11Texture2D> partHalves;
    static ComPtr<ID3D11ShaderResourceView> partHalvesView;
    static ComPtr<ID3D11Texture2D> allStaging;
    // frames the game handed over so far, with the ones skipped for a newer one
    static uint64_t frameCount = 0;
    // when the game drew the one in latest
    static double latestSeconds = 0.0;
    static std::chrono::steady_clock::time_point retryAt;
    static std::atomic<bool> capturing{ false };
    // set whenever the game hands over a frame
    static HANDLE frameArrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    static void closeSession() {
        try {
            if (held) held.Close();
            if (session) session.Close();
            if (pool) pool.Close();
        }
        catch (...) {}
        session = nullptr;
        pool = nullptr;
        item = nullptr;
        held = nullptr;
        latest.Reset();
        latestIsOurs = false;
        capturedWindow = nullptr;
        capturing = false;
    }

    static bool createDevice() {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return false;

        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIDevice> dxgiDevice;
        winrt::com_ptr<::IInspectable> inspectable;
        if (FAILED(device.As(&dxgiDevice)) ||
            FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()))) {
            device.Reset();
            context.Reset();
            return false;
        }
        captureDevice = inspectable.as<wd3d::IDirect3DDevice>();
        return true;
    }

    // missing before Windows 11 24H2, frames then come as the game draws them
    static void setFrameRate() {
        try { session.MinUpdateInterval(light ? LIGHT_EVERY : std::chrono::milliseconds(1)); } catch (...) {}
    }

    static bool openSession(HWND game) {
        try {
            if (!device && !createDevice()) return false;

            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(game, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            poolSize = item.Size();
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(captureDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
            pool.FrameArrived([](auto&&, auto&&) { SetEvent(frameArrived); });
            session = pool.CreateCaptureSession(item);
            // both are missing on older Windows 10
            try { session.IsCursorCaptureEnabled(false); } catch (...) {}
            try { session.IsBorderRequired(false); } catch (...) {}
            setFrameRate();
            session.StartCapture();
            capturedWindow = game;
            capturing = true;
            return true;
        }
        catch (...) {
            closeSession();
            return false;
        }
    }

    // moves the newest waiting frame into latest, if the game drew one
    static void takeNewestFrame() {
        wgc::Direct3D11CaptureFrame frame = pool.TryGetNextFrame();
        if (!frame) return;
        frameCount++;
        for (auto next = pool.TryGetNextFrame(); next; next = pool.TryGetNextFrame()) {
            frame.Close();
            frame = next;
            frameCount++;
        }

        auto size = frame.ContentSize();
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));

        if (!latestIsOurs) latest.Reset();
        if (held) held.Close();
        held = nullptr;
        // same clock as steady_clock, both count from the performance counter
        latestSeconds = std::chrono::duration<double>(frame.SystemRelativeTime()).count();
        latestSize = size;
        if (light) {
            latest = texture;
            latestIsOurs = false;
            held = frame;
        }
        else {
            D3D11_TEXTURE2D_DESC desc = {};
            texture->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC have = {};
            if (latest) latest->GetDesc(&have);
            if (!latest || have.Width != desc.Width || have.Height != desc.Height) {
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                latest.Reset();
                winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &latest));
                latestIsOurs = true;
            }
            context->CopyResource(latest.Get(), texture.Get());
            frame.Close();
        }

        // the window was resized, the next frames come at the new size
        if (size.Width != poolSize.Width || size.Height != poolSize.Height) {
            // a held frame is of the old size, the next grab has a new one
            if (held) {
                latest.Reset();
                held.Close();
                held = nullptr;
            }
            poolSize = size;
            pool.Recreate(captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
    }

    // the parts go side by side into one staging texture, so there's only one wait on the GPU
    static bool copyOut(HWND game, const std::vector<RECT>& rects, int conversion, std::vector<cv::Mat>& out) {
        // the frame starts at the window's visible edge, not at its client area
        RECT bounds;
        if (FAILED(DwmGetWindowAttribute(game, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) return false;
        std::vector<D3D11_BOX> boxes;
        UINT width = 0, height = 0;
        for (const RECT& rect : rects) {
            int left = rect.left - bounds.left;
            int top = rect.top - bounds.top;
            int w = rect.right - rect.left;
            int h = rect.bottom - rect.top;
            if (w <= 0 || h <= 0 || left < 0 || top < 0 || left + w > latestSize.Width || top + h > latestSize.Height) return false;
            boxes.push_back({ UINT(left), UINT(top), 0, UINT(left + w), UINT(top + h), 1 });
            width += w;
            height = std::max(height, UINT(h));
        }

        // only grows, the zoom bar and the tracker ask for different sizes all the time
        D3D11_TEXTURE2D_DESC have = {};
        if (staging) staging->GetDesc(&have);
        if (!staging || have.Width < width || have.Height < height) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = std::max(have.Width, width);
            desc.Height = std::max(have.Height, height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
        }

        UINT x = 0;
        for (const D3D11_BOX& box : boxes) {
            context->CopySubresourceRegion(staging.Get(), 0, x, 0, 0, latest.Get(), 0, &box);
            x += box.right - box.left;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        out.resize(boxes.size());
        x = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            int w = boxes[i].right - boxes[i].left;
            int h = boxes[i].bottom - boxes[i].top;
            cv::Mat bgra(h, w, CV_8UC4, (uchar*)mapped.pData + x * 4, mapped.RowPitch);
            cv::cvtColor(bgra, out[i], conversion);
            x += w;
        }
        context->Unmap(staging.Get(), 0);
        return true;
    }

    // Rect as gray, halved on the GPU until it's at most maxWidth wide, so
    // only the small picture is read back.
    static bool copySmall(HWND game, const RECT& rect, int maxWidth, cv::Mat& out) {
        RECT bounds;
        if (FAILED(DwmGetWindowAttribute(game, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) return false;
        int left = rect.left - bounds.left, top = rect.top - bounds.top;
        int w = rect.right - rect.left, h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0 || left < 0 || top < 0 || left + w > latestSize.Width || top + h > latestSize.Height) return false;
        int level = 0;
        while ((w >> level) > maxWidth) level++;
        int sw = std::max(1, w >> level), sh = std::max(1, h >> level);

        D3D11_TEXTURE2D_DESC have = {};
        if (halves) halves->GetDesc(&have);
        if (!halves || !smallStaging || have.Width != UINT(w) || have.Height != UINT(h) || have.MipLevels != UINT(level + 1)) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = level + 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            halves.Reset();
            halvesView.Reset();
            smallStaging.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &halves))) return false;
            if (FAILED(device->CreateShaderResourceView(halves.Get(), nullptr, &halvesView))) return false;
            D3D11_TEXTURE2D_DESC staged = desc;
            staged.Width = sw;
            staged.Height = sh;
            staged.MipLevels = 1;
            staged.Usage = D3D11_USAGE_STAGING;
            staged.BindFlags = 0;
            staged.MiscFlags = 0;
            staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&staged, nullptr, &smallStaging))) return false;
        }

        D3D11_BOX box = { UINT(left), UINT(top), 0, UINT(left + w), UINT(top + h), 1 };
        context->CopySubresourceRegion(halves.Get(), 0, 0, 0, 0, latest.Get(), 0, &box);
        context->GenerateMips(halvesView.Get());
        context->CopySubresourceRegion(smallStaging.Get(), 0, 0, 0, 0, halves.Get(), level, nullptr);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(smallStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        cv::Mat bgra(sh, sw, CV_8UC4, mapped.pData, mapped.RowPitch);
        cv::cvtColor(bgra, out, cv::COLOR_BGRA2GRAY);
        context->Unmap(smallStaging.Get(), 0);
        return true;
    }

    // copyOut and copySmall in one read back. The first shrink rects are
    // halved on the GPU too, as often as they stay at least atLeast a side.
    static bool copyShrunk(HWND game, const std::vector<RECT>& rects, int shrink, int atLeast, const RECT& whole, int maxWidth,
        std::vector<cv::Mat>& out, cv::Mat& shrunk) {
        RECT bounds;
        if (FAILED(DwmGetWindowAttribute(game, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) return false;
        auto boxOf = [&](const RECT& rect, D3D11_BOX& box) {
            int left = rect.left - bounds.left, top = rect.top - bounds.top;
            int w = rect.right - rect.left, h = rect.bottom - rect.top;
            if (w <= 0 || h <= 0 || left < 0 || top < 0 || left + w > latestSize.Width || top + h > latestSize.Height) return false;
            box = { UINT(left), UINT(top), 0, UINT(left + w), UINT(top + h), 1 };
            return true;
        };
        D3D11_BOX wholeBox;
        if (!boxOf(whole, wholeBox)) return false;
        std::vector<D3D11_BOX> boxes(rects.size());
        for (size_t i = 0; i < rects.size(); ++i) {
            if (!boxOf(rects[i], boxes[i])) return false;
        }
        shrink = std::min<int>(shrink, (int)rects.size());

        int w = wholeBox.right - wholeBox.left, h = wholeBox.bottom - wholeBox.top;
        int level = 0;
        while ((w >> level) > maxWidth) level++;
        int sw = std::max(1, w >> level), sh = std::max(1, h >> level);

        // how often the parts can be halved
        int side = INT_MAX;
        for (int i = 0; i < shrink; ++i) {
            side = std::min<int>({ side, int(boxes[i].right - boxes[i].left), int(boxes[i].bottom - boxes[i].top) });
        }
        int partLevel = 0;
        while (shrink > 0 && (side >> (partLevel + 1)) >= atLeast) partLevel++;
        // Side by side in one texture, each starting where the halving comes
        // out even, so a part's pixels never mix with its neighbour's.
        UINT even = 1u << partLevel;
        auto up = [&](UINT n) { return (n + even - 1) / even * even; };
        std::vector<UINT> starts;
        UINT rowWidth = 0, rowHeight = 0, restWidth = 0, restHeight = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            UINT bw = boxes[i].right - boxes[i].left, bh = boxes[i].bottom - boxes[i].top;
            if ((int)i < shrink) {
                starts.push_back(rowWidth);
                rowWidth += up(bw);
                rowHeight = std::max(rowHeight, up(bh));
            }
            else {
                restWidth += bw;
                restHeight = std::max(restHeight, bh);
            }
        }

        auto halvable = [&](ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view, UINT tw, UINT th, UINT levels) {
            D3D11_TEXTURE2D_DESC have = {};
            if (texture) texture->GetDesc(&have);
            if (texture && have.Width == tw && have.Height == th && have.MipLevels == levels) return true;
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = tw;
            desc.Height = th;
            desc.MipLevels = levels;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            texture.Reset();
            view.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return false;
            return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
        };
        D3D11_TEXTURE2D_DESC had = {};
        if (halves) halves->GetDesc(&had);
        if (!halvable(halves, halvesView, w, h, level + 1)) return false;
        D3D11_TEXTURE2D_DESC now = {};
        halves->GetDesc(&now);
        // the staging of copySmall goes with the size of halves
        if (had.Width != now.Width || had.Height != now.Height || had.MipLevels != now.MipLevels) smallStaging.Reset();
        if (shrink > 0 && !halvable(partHalves, partHalvesView, rowWidth, rowHeight, partLevel + 1)) return false;

        UINT partsWide = rowWidth >> partLevel, partsHigh = rowHeight >> partLevel;
        UINT width = sw + partsWide + restWidth, height = std::max({ UINT(sh), partsHigh, restHeight });
        D3D11_TEXTURE2D_DESC have = {};
        if (allStaging) allStaging->GetDesc(&have);
        if (!allStaging || have.Width < width || have.Height < height) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = std::max(have.Width, width);
            desc.Height = std::max(have.Height, height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            allStaging.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &allStaging))) return false;
        }

        context->CopySubresourceRegion(halves.Get(), 0, 0, 0, 0, latest.Get(), 0, &wholeBox);
        context->GenerateMips(halvesView.Get());
        context->CopySubresourceRegion(allStaging.Get(), 0, 0, 0, 0, halves.Get(), level, nullptr);
        if (shrink > 0) {
            for (int i = 0; i < shrink; ++i) {
                context->CopySubresourceRegion(partHalves.Get(), 0, starts[i], 0, 0, latest.Get(), 0, &boxes[i]);
            }
            context->GenerateMips(partHalvesView.Get());
            context->CopySubresourceRegion(allStaging.Get(), 0, sw, 0, 0, partHalves.Get(), partLevel, nullptr);
        }
        UINT x = sw + partsWide;
        for (size_t i = shrink; i < boxes.size(); ++i) {
            context->CopySubresourceRegion(allStaging.Get(), 0, x, 0, 0, latest.Get(), 0, &boxes[i]);
            x += boxes[i].right - boxes[i].left;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(allStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        allStaging->GetDesc(&have);
        cv::Mat all(have.Height, have.Width, CV_8UC4, mapped.pData, mapped.RowPitch);
        cv::cvtColor(all(cv::Rect(0, 0, sw, sh)), shrunk, cv::COLOR_BGRA2GRAY);
        out.resize(boxes.size());
        x = sw + partsWide;
        for (size_t i = 0; i < boxes.size(); ++i) {
            int bw = boxes[i].right - boxes[i].left, bh = boxes[i].bottom - boxes[i].top;
            cv::Rect in;
            if ((int)i < shrink) {
                in = cv::Rect(sw + (starts[i] >> partLevel), 0, bw >> partLevel, bh >> partLevel);
            }
            else {
                in = cv::Rect(x, 0, bw, bh);
                x += bw;
            }
            cv::cvtColor(all(in), out[i], cv::COLOR_BGRA2GRAY);
        }
        context->Unmap(allStaging.Get(), 0);
        return true;
    }

    // what a grab reads back next to its rects, see grabGameGray
    struct Small {
        RECT whole;
        int maxWidth;
        cv::Mat* out;
        int shrink = 0;
        int atLeast = 0;
    };

    // Every grab goes through here: opens the capture if it isn't, takes the
    // newest frame and copies the rects out. With newerOnly it's false when
    // frame is still the newest.
    static bool grab(HWND game, const std::vector<RECT>& rects, int conversion, std::vector<cv::Mat>& out, Frame* frame, bool newerOnly,
        const Small* also = nullptr) {
        thread_local bool apartment = false;
        if (!apartment) {
            try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
            catch (...) {}
            apartment = true;
        }

        std::lock_guard<std::mutex> lock(gameMutex);
        if (game != capturedWindow) {
            closeSession();
            if (std::chrono::steady_clock::now() < retryAt) return false;
            if (!openSession(game)) {
                std::cerr << "Couldn't capture the game window, trying again in 5 seconds" << std::endl;
                retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                return false;
            }
        }

        try {
            takeNewestFrame();
            if (newerOnly && frame->number == frameCount) return false;
            if (frame) *frame = { frameCount, latestSeconds };
            // right after opening there's no frame yet, the next grab has one
            if (!latest) return false;
            if (also && also->shrink > 0) {
                return copyShrunk(game, rects, also->shrink, also->atLeast, also->whole, also->maxWidth, out, *also->out);
            }
            if (!copyOut(game, rects, conversion, out)) return false;
            return !also || copySmall(game, also->whole, also->maxWidth, *also->out);
        }
        catch (...) {
            // the window closed or the device was lost, opened again next time
            closeSession();
            return false;
        }
    }

    bool grabGame(HWND game, const RECT& rect, cv::Mat& out, Frame* frame) {
        std::vector<cv::Mat> parts;
        if (!grab(game, { rect }, cv::COLOR_BGRA2BGR, parts, frame, false)) return false;
        out = parts[0];
        return true;
    }

    bool grabGameGray(HWND game, const std::vector<RECT>& rects, std::vector<cv::Mat>& out, Frame& frame,
        const RECT& whole, int maxWidth, cv::Mat& shrunk, int shrink, int atLeast) {
        Small both = { whole, maxWidth, &shrunk, shrink, atLeast };
        return grab(game, rects, cv::COLOR_BGRA2GRAY, out, &frame, true, &both);
    }

    bool isCapturing() {
        return capturing;
    }

    void setLight(bool on) {
        std::lock_guard<std::mutex> lock(gameMutex);
        if (light == on) return;
        light = on;
        if (session) setFrameRate();
        if (!light) return;
        // nothing of the map is needed until it opens again
        if (latestIsOurs) latest.Reset();
        latestIsOurs = false;
        staging.Reset();
        smallStaging.Reset();
        halvesView.Reset();
        halves.Reset();
        allStaging.Reset();
        partHalvesView.Reset();
        partHalves.Reset();
    }

    void waitForGameFrame(int milliseconds) {
        WaitForSingleObject(frameArrived, milliseconds);
    }

    void closeGameCapture() {
        std::lock_guard<std::mutex> lock(gameMutex);
        closeSession();
        staging.Reset();
        smallStaging.Reset();
        halvesView.Reset();
        halves.Reset();
        allStaging.Reset();
        partHalvesView.Reset();
        partHalves.Reset();
        captureDevice = nullptr;
        context.Reset();
        device.Reset();
    }
}
