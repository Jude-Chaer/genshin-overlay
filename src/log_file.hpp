#pragma once
#include <filesystem>

// Where std::cout and std::cerr go. The release exe has no console of its own.
namespace LogFile {
    // the overlay: everything goes to log.txt in the folder (the last run's
    // is kept as log-previous.txt), and also to the console or redirected output
    // there is. --console opens a console when there's none
    extern void StartOverlay(const std::filesystem::path& folder, bool console);
    // a command-line tool like --make-index: the console it was started from,
    // or wherever its output was redirected
    extern void StartTool();
    extern std::filesystem::path Path();
}
