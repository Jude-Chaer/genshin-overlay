#include <windows.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <string>
#include <opencv2/imgcodecs.hpp>
#include "overlay.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "map_data.hpp"
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

    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    if (!Overlay::Init()) {
        glfwTerminate();
        return 1;
    }

    MapTracking::Start(Helpers::exeDirectory() / "map_data");
    Extensions::initExtensions();
    Overlay::MainLoop();
    Extensions::destroyExtensions();
    MapTracking::Stop();
    Overlay::Shutdown();

    return 0;
}
