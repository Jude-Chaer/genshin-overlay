#pragma once
#include <GL/glew.h>
#include <string>

// Pictures from the web as textures, for extensions. Downloaded in the
// background, shrunk to MAX_SIZE and freed when nobody draws them for a while.

namespace WebImages {
    struct Image {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
    };

    extern void Init();
    extern void Shutdown();

    // texture 0 until it's loaded, or when it couldn't be
    extern Image Get(const std::string& url);
    // call once per frame after drawing, uploads finished pictures and frees unused ones
    extern void EndFrame();
}
