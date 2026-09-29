#include "capture.hpp"
#include <chrono>
#include <iostream>
#include <mutex>
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
    static winrt::Windows::Graphics::SizeInt32 latestSize{};
    static ComPtr<ID3D11Texture2D> staging;
    static std::chrono::steady_clock::time_point retryAt;

    static void closeSession() {
        try {
            if (session) session.Close();
            if (pool) pool.Close();
        }
        catch (...) {}
        session = nullptr;
        pool = nullptr;
        item = nullptr;
        latest.Reset();
        capturedWindow = nullptr;
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

    static bool openSession(HWND game) {
        try {
            if (!device && !createDevice()) return false;

            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(game, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            poolSize = item.Size();
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(captureDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
            session = pool.CreateCaptureSession(item);
            // both are missing on older Windows 10
            try { session.IsCursorCaptureEnabled(false); } catch (...) {}
            try { session.IsBorderRequired(false); } catch (...) {}
            session.StartCapture();
            capturedWindow = game;
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
        for (auto next = pool.TryGetNextFrame(); next; next = pool.TryGetNextFrame()) {
            frame.Close();
            frame = next;
        }

        auto size = frame.ContentSize();
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));

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
        }
        context->CopyResource(latest.Get(), texture.Get());
        latestSize = size;
        frame.Close();

        // the window was resized, the next frames come at the new size
        if (size.Width != poolSize.Width || size.Height != poolSize.Height) {
            poolSize = size;
            pool.Recreate(captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
    }

    static bool copyOut(HWND game, const RECT& rect, cv::Mat& out) {
        // the frame starts at the window's visible edge, not at its client area
        RECT bounds;
        if (FAILED(DwmGetWindowAttribute(game, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) return false;
        int left = rect.left - bounds.left;
        int top = rect.top - bounds.top;
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0 || left < 0 || top < 0 || left + w > latestSize.Width || top + h > latestSize.Height) return false;

        D3D11_TEXTURE2D_DESC have = {};
        if (staging) staging->GetDesc(&have);
        if (!staging || have.Width != (UINT)w || have.Height != (UINT)h) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
        }

        D3D11_BOX box = { UINT(left), UINT(top), 0, UINT(left + w), UINT(top + h), 1 };
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, latest.Get(), 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        cv::Mat bgra(h, w, CV_8UC4, mapped.pData, mapped.RowPitch);
        cv::cvtColor(bgra, out, cv::COLOR_BGRA2BGR);
        context->Unmap(staging.Get(), 0);
        return true;
    }

    bool grabGame(HWND game, const RECT& rect, cv::Mat& out) {
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
            // right after opening there's no frame yet, the next grab has one
            return latest && copyOut(game, rect, out);
        }
        catch (...) {
            // the window closed or the device was lost, opened again next time
            closeSession();
            return false;
        }
    }

    void closeGameCapture() {
        std::lock_guard<std::mutex> lock(gameMutex);
        closeSession();
        staging.Reset();
        captureDevice = nullptr;
        context.Reset();
        device.Reset();
    }
}
