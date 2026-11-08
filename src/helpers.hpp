#pragma once
#include <GL/glew.h>
#include <filesystem>
#include <string>
#include <opencv2/opencv.hpp>

namespace Helpers {
    extern GLuint loadTextureFromFile(const std::filesystem::path& filePath, int& width, int& height);
    // Path text as UTF-8, what Lua, ImGui and settings.json use. path::string()
    // gives the code page instead, which can't hold every folder name.
    extern std::string utf8(const std::filesystem::path& path);
    extern std::filesystem::path fromUtf8(const std::string& text);
    extern std::filesystem::path exeDirectory();
    extern bool isElevated();
    // starts this exe again as admin (UAC prompt), false if that was declined
    extern bool relaunchAsAdmin();
    extern std::string GenerateUUID();
    extern std::string hashFile(const std::filesystem::path& filePath);
    extern std::string hashString(const std::string& input);
    extern std::filesystem::path resolveRelativePath(const std::filesystem::path& basePath, const std::filesystem::path& relativePath);
    extern double compareTextures(GLuint texture1, GLuint texture2, int accuracy = 2);
}
