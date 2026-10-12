#include <windows.h>
#include <GLFW/glfw3.h>
#include <chrono>
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
#include "minimap.hpp"
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

// the map matching for the minimap commands, like the live thread has it
static MiniMap::Locate locateWith(MapLocator::Index& index) {
    index.Load(Helpers::exeDirectory() / "map_data" / "manifest.tsv");
    return [&index](const cv::Mat& grab) { return index.Match(grab, grab.cols / 2.0, grab.rows / 2.0, false, false); };
}

// genshin-overlay.exe --minimap <screenshot>
// finds the player from the minimap on one image of the whole game, map closed
static int locatePlayer(const std::string& imagePath) {
    cv::Mat image = cv::imread(imagePath, cv::IMREAD_COLOR);
    if (image.empty()) {
        std::cerr << "Could not read " << imagePath << std::endl;
        return 1;
    }
    MiniMap::Load(Helpers::exeDirectory() / "map_data");

    RECT rect = MiniMap::rect({ 0, 0, image.cols, image.rows });
    cv::Mat grab = image(cv::Rect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top));
    cv::Mat mini = MiniMap::shrink(grab);
    MapLocator::Index index;
    MiniMap::Watcher watcher(locateWith(index));
    auto start = std::chrono::steady_clock::now();
    // the way the live thread looks with no last spot
    MiniMap::Spot spot = watcher.Look(grab, 0.0);
    auto searched = std::chrono::steady_clock::now();
    if (!spot.found) {
        std::cout << "No minimap found, best score " << spot.score << ", search " << std::chrono::duration<double, std::milli>(searched - start).count() << " ms" << std::endl;
        return 2;
    }
    if (spot.floorId != 0) {
        std::cout << MapTracking::GetMapName(spot.mapId) << " (map " << spot.mapId << ", group " << spot.groupId << ", floor " << spot.floorId << ")  lat " << spot.lat << "  lng " << spot.lng
            << "  zoom " << spot.zoom << "  score " << spot.score << "  search " << std::chrono::duration<double, std::milli>(searched - start).count() << " ms" << std::endl;
        return 0;
    }
    // again from a bit off, the way it's followed
    MiniMap::Spot again = MiniMap::nearby(mini, spot.mapId, spot.lat + 6.0, spot.lng - 9.0, false, spot.zoom);
    searched = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) again = MiniMap::nearby(mini, spot.mapId, spot.lat + 6.0, spot.lng - 9.0, false, spot.zoom);
    auto followed = std::chrono::steady_clock::now();

    std::cout << MapTracking::GetMapName(spot.mapId) << " (map " << spot.mapId << ", group " << spot.groupId << ", floor " << spot.floorId << ")  lat " << spot.lat << "  lng " << spot.lng
        << "  zoom " << spot.zoom << "  score " << spot.score << "  search " << std::chrono::duration<double, std::milli>(searched - start).count() << " ms" << std::endl;
    std::cout << "from nearby: lat " << again.lat << "  lng " << again.lng << "  score " << again.score
        << "  " << std::chrono::duration<double, std::milli>(followed - searched).count() / 100.0 << " ms" << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--locate") {
        return locateImage(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--minimap") {
        return locatePlayer(argv[2]);
    }
    // genshin-overlay.exe --make-index <folder>, used by the map-index workflow
    if (argc >= 3 && std::string(argv[1]) == "--make-index") {
        return MapData::makeIndex(argv[2]);
    }

    Settings::Load();


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
    MiniMap::Start(Helpers::exeDirectory() / "map_data");
    Extensions::initExtensions();
    Overlay::MainLoop();
    Extensions::destroyExtensions();
    MapTracking::Stop();
    MapCheck::Stop();
    MiniMap::Stop();
    MapTiles::Shutdown();
    Overlay::Shutdown();

    return 0;
}
