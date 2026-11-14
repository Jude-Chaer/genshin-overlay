#include "file_dialogs.hpp"
#include <shobjidl.h>
#include <fstream>
#include <regex>
#include <thread>
#include "files.hpp"

namespace fs = std::filesystem;

namespace FileDialogs {
    static std::wstring widen(const std::string& text) {
        if (text.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
        return out;
    }

    static fs::path showWindows(HWND owner, bool save, const std::wstring& name, const std::optional<Filter>& filter) {
        fs::path picked;
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            save ? IID_IFileSaveDialog : IID_IFileOpenDialog, (void**)&dialog))) return picked;

        std::vector<COMDLG_FILTERSPEC> types;
        if (filter) types.push_back({ filter->label.c_str(), filter->pattern.c_str() });
        if (!save || !filter) types.push_back({ L"All files", L"*.*" });
        dialog->SetFileTypes((UINT)types.size(), types.data());
        // *.json gives json
        if (filter) {
            std::wstring first = filter->pattern.substr(2, filter->pattern.find(L';') - 2);
            dialog->SetDefaultExtension(first.c_str());
        }
        if (save) dialog->SetFileName(name.c_str());
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

    Picker picker = showWindows;

    // the one open now, only the overlay's thread looks at it
    static std::shared_ptr<Answer> showing;
    static bool showingFocus = false;

    // the thread has only what it was given, the overlay can close while it runs
    static void run(std::shared_ptr<Answer> answer, Picker pick, HWND owner, bool save, std::wstring name, std::string data,
        std::optional<Filter> filter) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        fs::path path = pick(owner, save, name, filter);
        if (path.empty()) {
            answer->error = "cancelled";
        }
        else if (save) {
            if (Files::writeFile(path, data)) answer->name = path.filename().u8string();
            else answer->error = "failed";
        }
        else {
            std::error_code error;
            uintmax_t size = fs::file_size(path, error);
            std::ifstream file(path, std::ios::binary);
            if (error || !file) answer->error = "failed";
            else if (size > MaxSize) answer->error = "too_big";
            else {
                // at most MaxSize + 1, the file can grow after file_size
                std::string data;
                char buffer[64 * 1024];
                while (file && data.size() <= MaxSize) {
                    file.read(buffer, sizeof(buffer));
                    data.append(buffer, (size_t)file.gcount());
                }
                if (file.bad()) answer->error = "failed";
                else if (data.size() > MaxSize) answer->error = "too_big";
                else {
                    answer->data = std::move(data);
                    answer->name = path.filename().u8string();
                }
            }
        }
        CoUninitialize();
        answer->done = true;
    }

    std::shared_ptr<Answer> start(HWND owner, bool save, const std::string& name, const std::string& data,
        const std::optional<Filter>& filter) {
        if (open()) return nullptr;
        // bad UTF-8 becomes U+FFFD
        std::wstring fileName = fs::path(widen(name)).filename().wstring();
        auto answer = std::make_shared<Answer>();
        std::thread(run, answer, picker, owner, save, fileName, data, filter).detach();
        showing = answer;
        showingFocus = true;
        return answer;
    }

    bool open() {
        return showing && !showing->done;
    }

    std::optional<Filter> filterFrom(const std::string& label, const std::string& pattern) {
        static const std::regex patterns(R"(\*\.[A-Za-z0-9]{1,16}(;\*\.[A-Za-z0-9]{1,16}){0,7})");
        std::wstring wide = widen(label);
        if (wide.empty() || wide.size() > 64 || !std::regex_match(pattern, patterns)) return std::nullopt;
        for (wchar_t c : wide) {
            if (c < 0x20) return std::nullopt;
        }
        return Filter{ wide, widen(pattern) };
    }

    std::function<void()> giveFocusBack;

    void update() {
        if (!showingFocus || !showing || !showing->done) return;
        showingFocus = false;
        if (giveFocusBack) giveFocusBack();
    }
}
