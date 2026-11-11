#include "settings.hpp"
#include "files.hpp"
#include "helpers.hpp"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>

namespace Settings {
    //bool runAsAdmin = false;
	bool hideOnMapClose = false;
    bool lineUpWithMap = true;
    bool ignoreSea = true;
    bool steadyZoom = true;
    bool guessAhead = true;
    bool lookAgainStill = true;
    bool followOnThread = true;
    bool shrinkOnGpu = true;
    bool findSmallFirst = true;
    bool downloadItems = true;
    std::string look = "Windows 11";
    std::string lightDark = "Follow Windows";
    int size = 100;
    const std::vector<int> Sizes = { 80, 90, 100, 110, 125, 150 };

    std::filesystem::path file;

    static std::filesystem::path settingsPath() {
        if (!file.empty()) return file;
        return Helpers::exeDirectory() / "settings.json";
    }

    // The whole file, or {} when there's none. One that isn't a JSON object is
    // moved to settings.json.broken first, so the next save can't wipe what's in it.
    // An empty file has nothing to keep, and one that can't be opened right now
    // (another program has it) isn't broken, so both stay where they are.
    static nlohmann::json read() {
        std::filesystem::path path = settingsPath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return nlohmann::json::object();
        std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec || size == 0) return nlohmann::json::object();

        std::string text = Files::readText(path);
        if (text.empty()) {
            std::cerr << "[settings] " << Helpers::utf8(path.filename()) << " can't be opened right now, starting from the defaults" << std::endl;
            return nlohmann::json::object();
        }
        nlohmann::json settings = nlohmann::json::parse(text, nullptr, false);
        if (!settings.is_discarded() && settings.is_object()) return settings;

        // an older broken file stays too: settings.json.broken2, 3 and so on
        std::filesystem::path broken = path;
        broken += ".broken";
        for (int n = 2; std::filesystem::exists(broken, ec); n++) {
            broken = path;
            broken += ".broken" + std::to_string(n);
        }
        std::filesystem::rename(path, broken, ec);
        if (ec) {
            std::cerr << "[settings] " << Helpers::utf8(path.filename()) << " can't be read and can't be moved: " << ec.message() << std::endl;
        }
        else {
            std::cerr << "[settings] " << Helpers::utf8(path.filename()) << " can't be read, kept it as "
                << Helpers::utf8(broken.filename()) << ", starting from the defaults" << std::endl;
        }
        return nlohmann::json::object();
    }

    // under another name first, so a save cut half way leaves the old file
    static void write(const nlohmann::json& settings) {
        // text that isn't UTF-8 (an extension's setting) is saved with each bad byte replaced, dump would throw on it
        if (!Files::writeFile(settingsPath(), settings.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) {
            std::cerr << "[settings] couldn't save " << Helpers::utf8(settingsPath()) << std::endl;
        }
    }

    // nothing to read leaves the settings as they are
    void Load() {
        nlohmann::json settings = read();
        if (settings.empty()) return;
		hideOnMapClose = settings.value("hideOnMapClose", false);
        lineUpWithMap = settings.value("lineUpWithMap", true);
        ignoreSea = settings.value("ignoreSea", true);
        steadyZoom = settings.value("steadyZoom", true);
        guessAhead = settings.value("guessAhead", true);
        lookAgainStill = settings.value("lookAgainStill", true);
        followOnThread = settings.value("followOnThread", true);
        shrinkOnGpu = settings.value("shrinkOnGpu", true);
        findSmallFirst = settings.value("findSmallFirst", true);
        downloadItems = settings.value("downloadItems", true);
        look = settings.value("look", std::string("Windows 11"));
        lightDark = settings.value("lightDark", std::string("Follow Windows"));
        // a size that isn't one of the choices, hand edited, is 100
        auto savedSize = settings.find("size");
        size = 100;
        if (savedSize != settings.end() && savedSize->is_number_integer()) {
            int saved = savedSize->get<int>();
            if (std::find(Sizes.begin(), Sizes.end(), saved) != Sizes.end()) size = saved;
        }
    }

    // extension settings are in the same file, don't wipe them
    void Save() {
        nlohmann::json settings = read();
		settings["hideOnMapClose"] = hideOnMapClose;
        settings["lineUpWithMap"] = lineUpWithMap;
        settings["ignoreSea"] = ignoreSea;
        settings["steadyZoom"] = steadyZoom;
        settings["guessAhead"] = guessAhead;
        settings["lookAgainStill"] = lookAgainStill;
        settings["followOnThread"] = followOnThread;
        settings["shrinkOnGpu"] = shrinkOnGpu;
        settings["findSmallFirst"] = findSmallFirst;
        settings["downloadItems"] = downloadItems;
        settings["look"] = look;
        settings["lightDark"] = lightDark;
        settings["size"] = size;
        write(settings);
    }

    nlohmann::json LoadExtensions() {
        nlohmann::json settings = read();
        auto it = settings.find("extensions");
        if (it == settings.end() || !it->is_object()) return nlohmann::json::object();
        return *it;
    }

    void SaveExtensions(const nlohmann::json& extensions) {
        nlohmann::json settings = read();
        settings["extensions"] = extensions;
        write(settings);
    }

    // the numbers the old lists used, which were the order of the permission enum then
    static const char* oldPermissionName(int number) {
        switch (number) {
        case 1: return "external_read";
        case 2: return "internal_read";
        case 3: return "external_write";
        case 4: return "internal_write";
        case 5: return "network";
        case 6: return "display_overlay";
        case 7: return "display_read";
        case 8: return "keybinds";
        default: return nullptr;
        }
    }

    static Grants grantsFrom(const nlohmann::json& saved) {
        Grants grants;
        if (!saved.is_object()) return grants;
        grants.saved = true;
        grants.hash = Files::stringField(saved, "hash");
        auto names = saved.find("granted");
        if (names != saved.end() && names->is_array()) {
            for (const auto& name : *names) {
                if (name.is_string()) grants.names.push_back(name.get<std::string>());
            }
        }
        return grants;
    }

    static nlohmann::json toJson(const Grants& grants) {
        return { { "hash", grants.hash }, { "granted", grants.names } };
    }

    Grants LoadGrants(const std::string& folder, const std::string& hash) {
        nlohmann::json settings = read();
        auto all = settings.find("extensionGrants");
        if (all != settings.end() && all->is_object() && all->contains(folder)) {
            return grantsFrom((*all)[folder]);
        }

        auto old = settings.find("extensionPermissions");
        if (old == settings.end() || !old->is_object()) return {};
        auto list = old->find(hash);
        if (list == old->end() || !list->is_array()) return {};

        Grants grants;
        grants.saved = true;
        grants.hash = hash;
        for (const auto& number : *list) {
            const char* name = number.is_number_integer() ? oldPermissionName(number.get<int>()) : nullptr;
            if (name) grants.names.push_back(name);
        }
        old->erase(hash);
        if (old->empty()) settings.erase("extensionPermissions");
        settings["extensionGrants"][folder] = toJson(grants);
        write(settings);
        std::cout << "[settings] moved the old permission list of " << folder << " to its folder" << std::endl;
        return grants;
    }

    void SaveGrants(const std::string& folder, const Grants& grants) {
        nlohmann::json settings = read();
        settings["extensionGrants"][folder] = toJson(grants);
        write(settings);
    }
}
