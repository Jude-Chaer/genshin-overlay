#include <windows.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include "overlay.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "settings.hpp"
#include "item_files.hpp"
#include "map_data.hpp"
#include "map_check.hpp"
#include "map_tiles.hpp"
#include "screen_pictures.hpp"
#include "map_tracking.hpp"
#include "minimap.hpp"
#include "locator/map_locator.hpp"
#include "packets/game_traffic.hpp"
#include "point_guides.hpp"
#include "accounts.hpp"
#include "marks.hpp"
#include "web_images.hpp"
#include "log_file.hpp"
#include "taskbar.hpp"
#include "update_check.hpp"

// PocketEsoptron.exe --locate <screenshot>
// runs the map matching on one image and prints where it is
static int locateImage(const std::string& imagePath) {
    MapLocator::Index index;
    std::filesystem::path manifest = Helpers::dataDirectory() / "map_data" / "manifest.tsv";
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
    index.Load(Helpers::dataDirectory() / "map_data" / "manifest.tsv");
    return [&index](const cv::Mat& grab) { return index.Match(grab, grab.cols / 2.0, grab.rows / 2.0, false, false); };
}

// PocketEsoptron.exe --minimap <screenshot>
// finds the player from the minimap on one image of the whole game, map closed
static int locatePlayer(const std::string& imagePath) {
    cv::Mat image = cv::imread(imagePath, cv::IMREAD_COLOR);
    if (image.empty()) {
        std::cerr << "Could not read " << imagePath << std::endl;
        return 1;
    }
    MiniMap::Load(Helpers::dataDirectory() / "map_data");

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

// PocketEsoptron.exe --minimap-replay <folder> <out.tsv> [pictures per second] [timed]
// Follows the player through a folder of minimap pictures (the square
// MiniMap::rect gives, in name order) like the live thread would, and writes
// one line per look.
//
// Plain, every picture gets one look, however long it takes. Timed, the clock
// runs like it does live: a look that takes 3 seconds misses the 3 seconds of
// pictures after it, and a big map that was opened and closed meanwhile.
// When the big map was open comes from events.tsv next to the folder, if
// there is one: the first picture with it open, the first one after, the map
// and the left, top, right, bottom it showed.
static int replayMinimap(const std::string& folder, const std::string& outPath, double perSecond, bool timed) {
    std::vector<std::filesystem::path> pictures;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) pictures.push_back(entry.path());
    std::sort(pictures.begin(), pictures.end());
    MiniMap::Load(Helpers::dataDirectory() / "map_data");
    MiniMap::OneThread alone;

    struct Open {
        size_t from = 0, to = 0;
        MiniMap::Area area;
    };
    std::vector<Open> opens;
    if (timed) {
        std::ifstream file(std::filesystem::path(folder).parent_path() / "events.tsv");
        Open open;
        while (file >> open.from >> open.to >> open.area.mapId >> open.area.left >> open.area.top >> open.area.right >> open.area.bottom) opens.push_back(open);
    }

    std::ofstream out(outPath);
    out << "picture\tseconds\tfound\tmap\tlat\tlng\tscore\tsearched\tms\tzoom\tfloor\n";
    MapLocator::Index index;
    MiniMap::Watcher watcher(locateWith(index), timed);
    // the live thread looks every 50 ms
    const double every = 0.05;
    const Open* wasOpen = nullptr;
    double seconds = 0.0;
    for (size_t i = 0; i < pictures.size(); i = timed ? (size_t)(seconds * perSecond) : i + 1) {
        if (!timed) seconds = i / perSecond;
        // pictures are numbered from 1
        const Open* open = nullptr;
        for (const Open& one : opens) {
            if (i + 1 >= one.from && i + 1 < one.to) open = &one;
        }
        if (open) {
            wasOpen = open;
            seconds += every;
            continue;
        }
        if (wasOpen) {
            watcher.SearchSoon(seconds);
            watcher.SearchFirst(wasOpen->area);
            wasOpen = nullptr;
        }

        cv::Mat image = cv::imread(pictures[i].string(), cv::IMREAD_COLOR);
        double ms = 0.0;
        if (!image.empty()) {
            bool searched = false;
            auto start = std::chrono::steady_clock::now();
            MiniMap::Spot spot = watcher.Look(image, seconds, &searched);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            out << pictures[i].filename().string() << "\t" << seconds << "\t" << spot.found << "\t" << spot.mapId << "\t" << spot.lat << "\t" << spot.lng
                << "\t" << spot.score << "\t" << searched << "\t" << ms << "\t" << spot.zoom << "\t" << spot.floorId << "\n";
        }
        // a search on its thread takes the time it takes, so the clock has to be the real one meanwhile
        if (watcher.Searching() && ms < every * 1000.0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(every * 1000.0 - ms));
        seconds += std::max(every, ms / 1000.0);
    }
    return 0;
}

// for the workflow's builders: an exception says what it was before the run goes red
static int saysWhy(const std::function<int()>& run) {
    try {
        return run();
    }
    catch (const std::exception& e) {
        std::cerr << "Failed: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "Failed with an unknown exception" << std::endl;
    }
    return 1;
}

static bool hasFlag(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; i++) {
        if (flag == argv[i]) return true;
    }
    return false;
}

static const char* flagValue(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i + 1 < argc; i++) {
        if (flag == argv[i]) return argv[i + 1];
    }
    return nullptr;
}

int main(int argc, char** argv) {
    std::vector<std::string> tools = { "--minimap-replay", "--locate", "--minimap", "--make-index", "--make-items" };
    if (argc >= 2 && std::find(tools.begin(), tools.end(), argv[1]) != tools.end()) {
        LogFile::StartTool();
    }
    else {
        // files next to the exe load by relative path, also when Windows starts it from System32
        std::error_code ignored;
        std::filesystem::current_path(Helpers::exeDirectory(), ignored);
        // a restart as admin waits for the copy that started it
        if (const char* after = flagValue(argc, argv, "--after")) Taskbar::WaitForCopy(std::strtoul(after, nullptr, 10));
        bool restart = hasFlag(argc, argv, "--restart-as-admin");
        // before the log, which the running copy has open
        if (Taskbar::AnotherCopyRuns(restart ? Taskbar::Ask::RestartAsAdmin : Taskbar::Ask::OpenMenu)) return 0;
        if (restart && !Helpers::isElevated() && Taskbar::RestartAsAdmin()) return 0;
        std::vector<std::string> moved;
        if (Helpers::dataDirectory() != Helpers::exeDirectory()) moved = Helpers::moveOldData(Helpers::exeDirectory(), Helpers::dataDirectory());
        LogFile::StartOverlay(Helpers::dataDirectory(), hasFlag(argc, argv, "--console"));
        if (!moved.empty()) std::cout << "Moved " << Helpers::joined(moved) << " to " << Helpers::utf8(Helpers::dataDirectory()) << std::endl;
    }

    if (argc >= 4 && std::string(argv[1]) == "--minimap-replay") {
        return replayMinimap(argv[2], argv[3], argc >= 5 ? std::stod(argv[4]) : 5.0, argc >= 6 && std::string(argv[5]) == "timed");
    }
    if (argc >= 3 && std::string(argv[1]) == "--locate") {
        return locateImage(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--minimap") {
        return locatePlayer(argv[2]);
    }
    // PocketEsoptron.exe --make-index <folder>, used by the map-index workflow
    if (argc >= 3 && std::string(argv[1]) == "--make-index") {
        return saysWhy([&]() { return MapData::makeIndex(argv[2]); });
    }
    // PocketEsoptron.exe --make-items <folder> [--allow-drop], used by the map-index workflow too
    if (argc >= 3 && std::string(argv[1]) == "--make-items") {
        bool allowDrop = argc >= 4 && std::string(argv[3]) == "--allow-drop";
        return saysWhy([&]() { return ItemFiles::make(argv[2], allowDrop); });
    }

    // acts as if Npcap isn't installed, to see what the menu says then
    if (hasFlag(argc, argv, "--skip-npcap")) {
        PacketCapture::skipNpcap = true;
    }

    Settings::Load();
    Accounts::Init();


    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    if (!Overlay::Init()) {
        glfwTerminate();
        return 1;
    }
    Taskbar::Start();
    if (Settings::checkUpdates) UpdateCheck::Start();

    Marks::Init();
    MapTracking::Start(Helpers::dataDirectory() / "map_data");
    ItemFiles::Start(Helpers::dataDirectory() / "items", Settings::downloadItems);
    MapTiles::Init(Helpers::dataDirectory() / "map_data");
    MapCheck::Start(Helpers::dataDirectory() / "map_data");
    MiniMap::Start(Helpers::dataDirectory() / "map_data");
    // off until the player turns it on in the menu
    if (Settings::readGameTraffic) GameTraffic::Start();
    Extensions::initExtensions();
    Overlay::MainLoop();
    Extensions::destroyExtensions();
    PointGuides::stop();
    WebImages::clear();
    // told first so it winds down while the others are joined
    ItemFiles::AskToStop();
    ScreenPictures::stop();
    GameTraffic::Stop();
    // first, it grabs the game and asks the map matching, which the tracker closes
    MiniMap::Stop();
    MapTracking::Stop();
    ItemFiles::Stop();
    MapCheck::Stop();
    MapTiles::Shutdown();
    ScreenPictures::clear();
    UpdateCheck::Stop();
    Taskbar::Stop();
    Overlay::Shutdown();
    Marks::Shutdown();
    Accounts::Shutdown();

    return 0;
}
