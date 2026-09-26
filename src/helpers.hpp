#pragma once
#include <GL/glew.h>
#include <filesystem>
#include <string>

namespace Helpers {
    extern GLuint loadTextureFromFile(const std::string& filePath, int& width, int& height);
    extern std::filesystem::path exeDirectory();
}
