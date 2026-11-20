#pragma once
#include <windows.h>
#include <GL/glew.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include "helpers.hpp"
#include "net.hpp"

// Pictures from the web for extensions, one store for ImGUI_Image, DrawImage
// and GetWebImage. Each extension has its own, downloaded with its own limits
// (its sites).
//
// A picture is downloaded on its own thread, shrunk to MaxSide (FullSide when
// asked full size, kept apart from the small one), made a texture
// on the draw thread and freed once nobody asked for it for FreeAfter. A
// failed one is tried again once asked for RetryFailed after it failed.
//
// Extensions get a handle, never the GL id: handles are counted up from
// FirstHandle and never given twice, so a handle kept after its picture was
// freed draws nothing, even when GL gives the same id to another texture.

namespace WebImages {
    inline const int MaxSide = 640;
    inline const int FullSide = 2048;
    inline const std::chrono::seconds FreeAfter{ 30 };
    inline const std::chrono::seconds RetryFailed{ 10 };
    // textures made in one frame at most, the rest wait for the next
    inline const int UploadsPerFrame = 4;
    // pictures kept per extension, and downloads at once
    inline const size_t MaxKept = 256;
    inline const int MaxWaiting = 8;
    // above every GL id, which are 32 bit
    inline const double FirstHandle = 4294967296.0;

    enum class State { Loading, Ready, Failed, Refused };

    struct Image {
        State state = State::Refused;
        double handle = 0;  // 0 unless Ready
        int width = 0, height = 0;
    };

    // How a download ended, for the extension's log
    struct Done {
        const void* owner = nullptr;
        // the Lua call that asked first
        std::string function;
        Net::Response response;
        bool ok = false;
    };

    // The picture at url for owner, started on the first ask (by function).
    // Each ask counts as a use. Refused when owner already keeps MaxKept,
    // Loading while it waits on MaxWaiting.
    Image get(const void* owner, const std::string& url, const Net::Limits& limits, const std::string& function = "", int side = MaxSide);
    // the texture behind a handle of owner's, 0 for one that was freed or isn't
    // owner's; counts as a use
    GLuint texture(const void* owner, double handle);
    // a number that can only be a handle, never a GL id
    bool isHandle(double value);

    // Once per frame on the draw thread: finished downloads become textures,
    // pictures unused for FreeAfter are freed. done is called
    // for each finished download.
    void endFrame(const std::function<void(const Done&)>& done = nullptr);
    // stops owner's downloads and frees its pictures, see Extensions::unload
    void forget(const void* owner);
    // everything, at shutdown
    void clear();

    size_t kept(const void* owner);
    int downloading(const void* owner);

    // shrinks pixels so the longest side is at most side
    void shrink(Helpers::Pixels& pixels, int side = MaxSide);

    // how a download is made, a texture made and freed, and the time (the
    // tests run with no GL)
    extern std::function<bool(const std::string& url, Net::Response& out, Net::Stopper* stopper, const Net::Limits& limits)> fetch;
    extern std::function<GLuint(const Helpers::Pixels& pixels)> make;
    extern std::function<void(GLuint texture)> release;
    extern std::function<std::chrono::steady_clock::time_point()> now;
}
