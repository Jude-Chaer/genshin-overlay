#pragma once
#include <GL/glew.h>
#include <filesystem>
#include <string>

namespace Helpers {
    extern GLuint loadTextureFromFile(const std::string& filePath, int& width, int& height);
    extern std::filesystem::path exeDirectory();
    extern bool isElevated();
    // starts this exe again as admin (UAC prompt), false if that was declined
    extern bool relaunchAsAdmin();
}
