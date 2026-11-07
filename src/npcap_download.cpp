#include "npcap_download.hpp"
#include <windows.h>
#include <shellapi.h>
#include <softpub.h>
#include <wintrust.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <thread>
#include "net.hpp"

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace NpcapDownload {
    static constexpr const char* SITE = "https://npcap.com/";
    static std::atomic<State> state = State::Idle;
    // the installer it got, opened again from here without downloading it again
    static std::filesystem::path downloaded;

    State GetState() {
        return state;
    }

    std::string InstallerName(const std::string& page) {
        std::smatch found;
        static const std::regex name(R"(dist/(npcap-[0-9][0-9.]*\.exe))");
        if (!std::regex_search(page, found, name))
            return "";
        return found[1];
    }

    bool NpcapSigner(const std::wstring& name) {
        return name == L"Nmap Software LLC" || name == L"Insecure.Com LLC";
    }

    // Windows checks the signature, then who signed it
    static bool signedByNpcap(const std::wstring& path) {
        WINTRUST_FILE_INFO file = {};
        file.cbStruct = sizeof(file);
        file.pcwszFilePath = path.c_str();
        WINTRUST_DATA data = {};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &file;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        bool ok = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
        if (ok) {
            ok = false;
            CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
            CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
            if (signer && signer->csCertChain > 0 && signer->pasCertChain[0].pCert) {
                wchar_t name[256] = {};
                CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
                ok = NpcapSigner(name);
                if (!ok)
                    std::wcout << L"Npcap download: signed by " << name << L", not opened" << std::endl;
            }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &data);
        return ok;
    }

    static bool run(const std::filesystem::path& path) {
        // Npcap's installer asks for admin itself
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }

    static bool download() {
        std::error_code error;
        // Windows checks it each time, the file in temp could have been swapped
        if (!downloaded.empty() && std::filesystem::exists(downloaded, error)) {
            if (signedByNpcap(downloaded.wstring()))
                return run(downloaded);
            std::cout << "Npcap download: the saved installer didn't pass the check, getting it again" << std::endl;
            downloaded.clear();
        }
        Net::Response page;
        if (!Net::get(SITE, page) || page.status != 200)
            return false;
        std::string name = InstallerName(page.body);
        if (name.empty()) {
            std::cout << "Npcap download: no installer on " << SITE << std::endl;
            return false;
        }
        Net::Response installer;
        if (!Net::get(std::string(SITE) + "dist/" + name, installer) || installer.status != 200)
            return false;
        std::filesystem::path temp = std::filesystem::temp_directory_path(error);
        if (error) {
            std::cout << "Npcap download: no temp folder, " << error.message() << std::endl;
            return false;
        }
        std::filesystem::path path = temp / name;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(installer.body.data(), std::streamsize(installer.body.size()));
            // a full disk can show only when it's closed
            out.close();
            if (!out)
                return false;
        }
        if (!signedByNpcap(path.wstring()))
            return false;
        downloaded = path;
        return run(path);
    }

    void Start() {
        State expected = state;
        if (expected == State::Downloading || !state.compare_exchange_strong(expected, State::Downloading))
            return;
        // nothing may come out of the thread, it would close the overlay
        try {
            std::thread([] {
                bool ok = false;
                try {
                    ok = download();
                }
                catch (const std::exception& e) {
                    std::cout << "Npcap download: " << e.what() << std::endl;
                }
                state = ok ? State::Opened : State::Failed;
            }).detach();
        }
        catch (const std::exception&) {
            state = State::Failed;
        }
    }
}
