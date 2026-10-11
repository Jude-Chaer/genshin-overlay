#include <filesystem>
#include <catch2/catch_session.hpp>
#include "settings.hpp"

int main(int argc, char* argv[]) {
    // tests.exe sits next to the overlay's own settings.json, the tests must never touch that one
    std::filesystem::path folder = std::filesystem::temp_directory_path() / "genshin-overlay-tests";
    std::filesystem::create_directories(folder);
    Settings::file = folder / "settings.json";

    return Catch::Session().run(argc, argv);
}
