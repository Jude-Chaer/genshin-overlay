#include "helpers.hpp"
#include <SOIL2/SOIL2.h>
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <bcrypt.h>
#include <algorithm>
#include <opencv2/opencv.hpp>
#include <cmath>
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
    
    double compareTextures(GLuint texture1, GLuint texture2, int accuracy) {
        accuracy = std::clamp(accuracy, 0, 5);

        if (!texture1 || !texture2)
            return 0.0;

        GLint width1 = 0, height1 = 0, width2 = 0, height2 = 0;

        glBindTexture(GL_TEXTURE_2D, texture1);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width1);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height1);

        glBindTexture(GL_TEXTURE_2D, texture2);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width2);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height2);

        if (width1 <= 0 || height1 <= 0 || width2 <= 0 || height2 <= 0)
            return 0.0;

        std::vector<unsigned char> data1(width1 * height1 * 4);
        std::vector<unsigned char> data2(width2 * height2 * 4);

        glBindTexture(GL_TEXTURE_2D, texture1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data1.data());

        glBindTexture(GL_TEXTURE_2D, texture2);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data2.data());

        cv::Mat rgba1(height1, width1, CV_8UC4, data1.data());
        cv::Mat rgba2(height2, width2, CV_8UC4, data2.data());

        cv::Mat gray1, gray2;
        cv::cvtColor(rgba1, gray1, cv::COLOR_RGBA2GRAY);
        cv::cvtColor(rgba2, gray2, cv::COLOR_RGBA2GRAY);

        const int scale = 1 << accuracy;

        if (scale > 1)
        {
            cv::resize(gray1, gray1, cv::Size(std::max(1, gray1.cols / scale), std::max(1, gray1.rows / scale)), 0, 0, cv::INTER_AREA);
            cv::resize(gray2, gray2, cv::Size(std::max(1, gray2.cols / scale), std::max(1, gray2.rows / scale)), 0, 0, cv::INTER_AREA);
        }

        int features = 0;
        double contrastThreshold = 0.04;

        switch (accuracy)
        {
        case 0:
            features = 0;
            contrastThreshold = 0.02;
            break;
        case 1:
            features = 2500;
            contrastThreshold = 0.025;
            break;
        case 2:
            features = 1800;
            contrastThreshold = 0.03;
            break;
        case 3:
            features = 1200;
            contrastThreshold = 0.04;
            break;
        case 4:
            features = 800;
            contrastThreshold = 0.05;
            break;
        case 5:
            features = 500;
            contrastThreshold = 0.06;
            break;
        }

        auto sift = cv::SIFT::create(features, 3, contrastThreshold, 10, 1.6);

        std::vector<cv::KeyPoint> keypoints1, keypoints2;
        cv::Mat descriptors1, descriptors2;

        sift->detectAndCompute(gray1, cv::noArray(), keypoints1, descriptors1);
        sift->detectAndCompute(gray2, cv::noArray(), keypoints2, descriptors2);

        if (descriptors1.empty() || descriptors2.empty())
            return 0.0;

        cv::FlannBasedMatcher matcher(
            cv::makePtr<cv::flann::KDTreeIndexParams>(4),
            cv::makePtr<cv::flann::SearchParams>(64)
        );

        std::vector<std::vector<cv::DMatch>> matches;
        matcher.knnMatch(descriptors1, descriptors2, matches, 2);

        const double ratio = accuracy <= 1 ? 0.75 : accuracy == 2 ? 0.78 : accuracy == 3 ? 0.80 : accuracy == 4 ? 0.83 : 0.86;

        std::vector<cv::DMatch> goodMatches;
        for (const auto& match : matches)
        {
            if (match.size() >= 2 && match[0].distance < ratio * match[1].distance)
                goodMatches.push_back(match[0]);
        }

        if (goodMatches.size() < 4)
            return 0.0;

        std::vector<cv::Point2f> points1, points2;
        points1.reserve(goodMatches.size());
        points2.reserve(goodMatches.size());

        for (const auto& match : goodMatches)
        {
            points1.push_back(keypoints1[match.queryIdx].pt);
            points2.push_back(keypoints2[match.trainIdx].pt);
        }

        std::vector<unsigned char> inlierMask;
        cv::Mat homography = cv::findHomography(
            points1,
            points2,
            cv::RANSAC,
            accuracy <= 1 ? 3.0 : accuracy <= 3 ? 5.0 : 8.0,
            inlierMask,
            2000,
            0.995
        );

        if (homography.empty())
            return 0.0;

        int inliers = 0;
        for (unsigned char value : inlierMask)
            inliers += value != 0;

        if (inliers < 4)
            return 0.0;

        double inlierRatio = static_cast<double>(inliers) / static_cast<double>(goodMatches.size());
        double matchScore = std::min(1.0, static_cast<double>(inliers) / static_cast<double>(std::max(8, 20 - accuracy * 2)));

        return std::clamp(0.7 * inlierRatio + 0.3 * matchScore, 0.0, 1.0);
    }
}
