#pragma once
#include <GL/glew.h>
#include <filesystem>
#include <string>
#include <opencv2/opencv.hpp>

namespace Helpers {
    extern GLuint loadTextureFromFile(const std::string& filePath, int& width, int& height);
    extern std::filesystem::path exeDirectory();
    extern bool isElevated();
    // starts this exe again as admin (UAC prompt), false if that was declined
    extern bool relaunchAsAdmin();
    extern std::string hashFile(const std::filesystem::path& filePath);
    extern std::string hashString(const std::string& input);
    extern std::filesystem::path resolveRelativePath(const std::filesystem::path& basePath, const std::filesystem::path& relativePath);
    extern double compareTextures(GLuint texture1, GLuint texture2, int accuracy = 2);
}
