#include <windows.h>
#include <shellapi.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <string>
#include <opencv2/imgcodecs.hpp>
#include "overlay.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "map_data.hpp"
#include "map_tiles.hpp"
#include "map_tracking.hpp"
#include "locator/map_locator.hpp"

// genshin-overlay.exe --locate <screenshot>
// runs the map matching on one image and prints where it is
static int locateImage(const std::string& imagePath) {
    MapLocator::Index index;
    std::filesystem::path manifest = Helpers::exeDirectory() / "map_data" / "manifest.tsv";
    if (!index.Load(manifest)) {
        std::cerr << "Could not load map data from " << manifest << std::endl;
        return 1;
    }

    cv::Mat image = cv::imread(imagePath, cv::IMREAD_COLOR);
    if (image.empty()) {
        std::cerr << "Could not read " << imagePath << std::endl;
        return 1;
    }

    MapLocator::Result result = index.Match(image, image.cols / 2.0, image.rows / 2.0);
    if (!result.found) {
        std::cout << "No map found (" << result.inliers << " inliers)" << std::endl;
        return 2;
    }

    std::cout << MapTracking::GetMapName(result.mapId) << " (map " << result.mapId << ", group " << result.groupId
        << ", floor " << result.floorId << ")  lat " << result.lat << "  lng " << result.lng
        << "  units/px " << result.unitsPerPixel << "  inliers " << result.inliers << std::endl;
    return 0;
}

// The game runs as admin, and Windows won't let a normal program read the
// keyboard while an admin window has focus, so the overlay asks for admin too.
// --locate and --make-index don't need it, they return before this.
// --no-admin skips it for testing outside the game.
static bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

static bool relaunchAsAdmin() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--locate") {
        return locateImage(argv[2]);
    }
    // genshin-overlay.exe --make-index <folder>, used by the map-index workflow
    if (argc >= 3 && std::string(argv[1]) == "--make-index") {
        return MapData::makeIndex(argv[2]);
    }

    bool noAdmin = argc >= 2 && std::string(argv[1]) == "--no-admin";
    if (!noAdmin && !isElevated()) {
        if (relaunchAsAdmin()) return 0;
        std::cerr << "Running without admin, keys won't work while the game has focus" << std::endl;
    }

    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    if (!Overlay::Init()) {
        glfwTerminate();
        return 1;
    }

    MapTracking::Start(Helpers::exeDirectory() / "map_data");
    MapTiles::Init(Helpers::exeDirectory() / "map_data");
    Extensions::initExtensions();
    Overlay::MainLoop();
    Extensions::destroyExtensions();
    MapTracking::Stop();
    MapTiles::Shutdown();
    Overlay::Shutdown();

    return 0;
}
