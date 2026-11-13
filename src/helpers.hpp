#pragma once
#include <GL/glew.h>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

namespace Helpers {
    extern GLuint loadTextureFromFile(const std::filesystem::path& filePath, int& width, int& height);
    // a picture file's bytes, up to 4096 x 4096; name is for the log
    extern GLuint loadTextureFromMemory(const std::string& bytes, const std::string& name, int& width, int& height);
    // the same in two halves: unpacked as RGBA on any thread, then made a texture on the draw thread
    struct Pixels {
        std::vector<unsigned char> rgba;
        int width = 0, height = 0;
    };
    extern bool decodePicture(const std::string& bytes, const std::string& name, Pixels& out);
    extern GLuint textureFromPixels(const Pixels& pixels, int& width, int& height);
    // Path text as UTF-8, what Lua, ImGui and settings.json use. path::string()
    // gives the code page instead, which can't hold every folder name.
    extern std::string utf8(const std::filesystem::path& path);
    extern std::filesystem::path fromUtf8(const std::string& text);
    extern std::filesystem::path exeDirectory();
    // "a, b, c"
    extern std::string joined(const std::vector<std::string>& list);
    extern bool isElevated();
    // Windows set to dark for apps (Settings, Personalization, Colors)
    extern bool systemDark();
    // Windows' accent colour (Settings, Personalization, Colors) in its shades
    // as 0xRRGGBBFF, lightest first: Light3, Light2, Light1, the accent itself,
    // Dark1, Dark2, Dark3. None when Windows doesn't have them.
    extern std::optional<std::array<unsigned int, 7>> accentShades();
    // true when the accent may have changed since the last call, the first
    // call too. Windows tells us, so asking every loop costs nothing.
    extern bool accentChanged();
    // the system's own font file for the menu, empty when it has none
    extern std::filesystem::path systemFont();
    // starts this exe again as admin (UAC prompt), false if that was declined
    extern bool relaunchAsAdmin();
    extern std::string GenerateUUID();
    extern std::string hashFile(const std::filesystem::path& filePath);
    extern std::string hashString(const std::string& input);
    extern std::filesystem::path resolveRelativePath(const std::filesystem::path& basePath, const std::filesystem::path& relativePath);
    extern double compareTextures(GLuint texture1, GLuint texture2, int accuracy = 2);
}
