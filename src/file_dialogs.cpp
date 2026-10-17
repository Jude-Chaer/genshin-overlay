#include "file_dialogs.hpp"
#include <shobjidl.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace FileDialogs {
    // bigger than anything an extension should be loading in one go
    static constexpr uintmax_t MAX_OPEN = 64 * 1024 * 1024;

    static std::mutex mutex;
    static std::map<int, Result> results;
    static std::atomic<bool> open = false;
    static int nextDialog = 1;

    static std::wstring widen(const std::string& text) {
        if (text.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
        return out;
    }

    // the path the user picked, empty when they closed it
    static std::filesystem::path show(HWND owner, bool save, const std::string& suggestedName) {
        std::filesystem::path picked;
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            save ? IID_IFileSaveDialog : IID_IFileOpenDialog, (void**)&dialog))) return picked;

        COMDLG_FILTERSPEC types[] = { { L"JSON", L"*.json" }, { L"All files", L"*.*" } };
        dialog->SetFileTypes(2, types);
        dialog->SetDefaultExtension(L"json");
        if (save) dialog->SetFileName(widen(suggestedName).c_str());
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));

        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) picked = path;
            CoTaskMemFree(path);
            item->Release();
        }
        dialog->Release();
        return picked;
    }

    static void run(int id, HWND owner, bool save, std::string suggestedName, std::string data) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Result result;
        std::filesystem::path path = show(owner, save, suggestedName);
        if (!path.empty()) {
            result.picked = true;
            result.name = path.filename().u8string();
            if (save) {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << data;
                file.close();
                result.ok = !file.fail();
            }
            else {
                std::error_code error;
                uintmax_t size = std::filesystem::file_size(path, error);
                std::ifstream file(path, std::ios::binary);
                if (!error && size <= MAX_OPEN && file) {
                    result.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                    result.ok = true;
                }
            }
        }
        CoUninitialize();

        std::lock_guard<std::mutex> lock(mutex);
        results[id] = std::move(result);
        open = false;
    }

    static int start(HWND owner, bool save, const std::string& suggestedName, const std::string& data) {
        bool expected = false;
        if (!open.compare_exchange_strong(expected, true)) return 0;
        int id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            id = nextDialog++;
        }
        // nothing waits for it, closing the overlay with a dialog open just ends it
        std::thread(run, id, owner, save, suggestedName, data).detach();
        return id;
    }

    int Save(HWND owner, const std::string& suggestedName, const std::string& data) {
        return start(owner, true, suggestedName, data);
    }

    int Open(HWND owner) {
        return start(owner, false, "", "");
    }

    bool GetResult(int dialog, Result& out) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = results.find(dialog);
        if (it == results.end()) return false;
        out = std::move(it->second);
        results.erase(it);
        return true;
    }
}
