#include <windows.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <string>
#include <opencv2/imgcodecs.hpp>
#include "overlay.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "settings.hpp"
#include "map_data.hpp"
#include "map_check.hpp"
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

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--locate") {
        return locateImage(argv[2]);
    }
    // genshin-overlay.exe --make-index <folder>, used by the map-index workflow
    if (argc >= 3 && std::string(argv[1]) == "--make-index") {
        return MapData::makeIndex(argv[2]);
    }

    // Admin is optional, see Settings::runAsAdmin. If the prompt is declined we
    // just carry on without it. --no-admin skips it for testing.
    Settings::Load();
    bool noAdmin = argc >= 2 && std::string(argv[1]) == "--no-admin";
    if (Settings::runAsAdmin && !noAdmin && !Helpers::isElevated()) {
        if (Helpers::relaunchAsAdmin()) return 0;
        std::cerr << "Not running as admin, the map closes about a second after the game's map" << std::endl;
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
    MapCheck::Start(Helpers::exeDirectory() / "map_data");
    Extensions::initExtensions();
    Overlay::MainLoop();
    Extensions::destroyExtensions();
    MapTracking::Stop();
    MapCheck::Stop();
    MapTiles::Shutdown();
    Overlay::Shutdown();

    return 0;
}
