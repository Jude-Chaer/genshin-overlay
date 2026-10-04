#include "helpers.hpp"
#include <SOIL2/SOIL2.h>
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <array>

namespace Helpers {
    GLuint loadTextureFromFile(const std::string& filename, int& width, int& height) {
        GLuint textureID = SOIL_load_OGL_texture(
            filename.c_str(),
            SOIL_LOAD_AUTO,
            SOIL_CREATE_NEW_ID,
            SOIL_FLAG_MIPMAPS
        );

        if (textureID == 0) {
            std::cerr << "Failed to load texture: " << filename << std::endl;
            return 0;
        }

        glBindTexture(GL_TEXTURE_2D, textureID);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

        glBindTexture(GL_TEXTURE_2D, 0);

        return textureID;
    }

    // so files next to the exe are found no matter where it's started from
    std::filesystem::path exeDirectory() {
        wchar_t buffer[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length == 0 || length == MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(buffer).parent_path();
    }

    bool isElevated() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation = {};
        DWORD size = 0;
        bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
        CloseHandle(token);
        return elevated;
    }

    bool relaunchAsAdmin() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        SHELLEXECUTEINFOW info = {};
        info.cbSize = sizeof(info);
        info.lpVerb = L"runas";
        info.lpFile = path;
        info.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&info) != FALSE;
    }


    std::string hashString(const std::string& input) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hashHandle = nullptr;

        if (BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA512_ALGORITHM,
            nullptr,
            0
        ) != 0) {
            return "";
        }

        DWORD objectSize = 0;
        DWORD resultSize = 0;

        if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &resultSize,
            0
        ) != 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }

        std::vector<BYTE> hashObject(objectSize);

        if (BCryptCreateHash(
            algorithm,
            &hashHandle,
            hashObject.data(),
            objectSize,
            nullptr,
            0,
            0
        ) != 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }

        if (BCryptHashData(
            hashHandle,
            reinterpret_cast<PUCHAR>(
                const_cast<char*>(input.data())
                ),
            static_cast<ULONG>(input.size()),
            0
        ) != 0) {
            BCryptDestroyHash(hashHandle);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }

        std::array<BYTE, 64> hash{};

        if (BCryptFinishHash(
            hashHandle,
            hash.data(),
            static_cast<ULONG>(hash.size()),
            0
        ) != 0) {
            BCryptDestroyHash(hashHandle);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }

        BCryptDestroyHash(hashHandle);
        BCryptCloseAlgorithmProvider(algorithm, 0);

        std::ostringstream result;

        for (BYTE byte : hash) {
            result << std::hex
                << std::setw(2)
                << std::setfill('0')
                << static_cast<int>(byte);
        }

        return result.str();
    }

    std::string hashFile(const std::filesystem::path& filePath) {
        std::ifstream file(filePath, std::ios::binary);

        if (!file)
            return "";

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hashHandle = nullptr;

        if (BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA512_ALGORITHM,
            nullptr,
            0
        ) != 0) {
            return "";
        }

        DWORD objectSize = 0;
        DWORD resultSize = 0;

        if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &resultSize,
            0
        ) != 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }

        std::vector<BYTE> hashObject(objectSize);

        if (BCryptCreateHash(
            algorithm,
            &hashHandle,
            hashObject.data(),
            objectSize,
            nullptr,
            0,
            0
        ) != 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }

        char buffer[8192];

        while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
            if (BCryptHashData(
                hashHandle,
                reinterpret_cast<PUCHAR>(buffer),
                static_cast<ULONG>(file.gcount()),
                0
            ) != 0) {
                BCryptDestroyHash(hashHandle);
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return "";
            }
        }

        std::array<BYTE, 64> hash{};

        if (BCryptFinishHash(
            hashHandle,
            hash.data(),
            static_cast<ULONG>(hash.size()),
            0
        ) != 0) {
            BCryptDestroyHash(hashHandle);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return "";
        }

        BCryptDestroyHash(hashHandle);
        BCryptCloseAlgorithmProvider(algorithm, 0);

        std::ostringstream result;

        for (BYTE byte : hash) {
            result << std::hex
                << std::setw(2)
                << std::setfill('0')
                << static_cast<int>(byte);
        }

        return result.str();
    }

    std::filesystem::path resolveRelativePath(const std::filesystem::path& basePath, const std::filesystem::path& relativePath) {
        return std::filesystem::weakly_canonical(basePath / relativePath);
    }
}
