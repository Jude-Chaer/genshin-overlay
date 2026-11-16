#include "log_file.hpp"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <streambuf>

namespace LogFile {
    static std::filesystem::path path;
    // never destroyed, std::cout and std::cerr point at it until the process ends
    static std::ofstream& file = *new std::ofstream;

    // writes to two buffers, for the file and the console
    class Tee : public std::streambuf {
    public:
        Tee(std::streambuf* first, std::streambuf* second) : first(first), second(second) {}

    protected:
        int overflow(int c) override {
            if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
            first->sputc((char)c);
            second->sputc((char)c);
            return c;
        }

        std::streamsize xsputn(const char* text, std::streamsize count) override {
            first->sputn(text, count);
            second->sputn(text, count);
            return count;
        }

        int sync() override {
            first->pubsync();
            second->pubsync();
            return 0;
        }

    private:
        std::streambuf* first;
        std::streambuf* second;
    };

    static bool redirected(DWORD which) {
        HANDLE handle = GetStdHandle(which);
        return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
    }

    static void toConsole() {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
        std::cout.clear();
        std::cerr.clear();
        std::wcout.clear();
    }

    void StartOverlay(const std::filesystem::path& folder, bool console) {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        path = folder / "log.txt";
        std::filesystem::rename(path, folder / "log-previous.txt", error);
        file.open(path, std::ios::binary | std::ios::trunc);

        // a console (the debug build's, or --console) or output redirected by whoever started it
        bool shown = GetConsoleWindow() != nullptr || redirected(STD_OUTPUT_HANDLE) || redirected(STD_ERROR_HANDLE);
        if (console && !shown && AllocConsole()) {
            toConsole();
            shown = true;
        }
        if (!file.is_open()) return;

        static Tee* out = new Tee(file.rdbuf(), std::cout.rdbuf());
        static Tee* err = new Tee(file.rdbuf(), std::cerr.rdbuf());
        std::cout.rdbuf(shown ? static_cast<std::streambuf*>(out) : file.rdbuf());
        std::cerr.rdbuf(shown ? static_cast<std::streambuf*>(err) : file.rdbuf());
        // so a crash loses nothing
        std::cout << std::unitbuf;
    }

    void StartTool() {
        if (redirected(STD_OUTPUT_HANDLE) || redirected(STD_ERROR_HANDLE)) return;
        if (AttachConsole(ATTACH_PARENT_PROCESS)) toConsole();
    }

    std::filesystem::path Path() {
        return path;
    }
}
