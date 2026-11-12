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
    bool recordTakenKeys = false;
    bool followOnThread = true;
    bool shrinkOnGpu = true;
    bool findSmallFirst = true;
    bool downloadItems = true;
    std::string menuKey = "Alt+`";
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
        recordTakenKeys = settings.value("recordTakenKeys", false);
        followOnThread = settings.value("followOnThread", true);
        shrinkOnGpu = settings.value("shrinkOnGpu", true);
        findSmallFirst = settings.value("findSmallFirst", true);
        downloadItems = settings.value("downloadItems", true);
        menuKey = settings.value("menuKey", std::string("Alt+`"));
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
        settings["recordTakenKeys"] = recordTakenKeys;
        settings["followOnThread"] = followOnThread;
        settings["shrinkOnGpu"] = shrinkOnGpu;
        settings["findSmallFirst"] = findSmallFirst;
        settings["downloadItems"] = downloadItems;
        settings["menuKey"] = menuKey;
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

    std::map<std::string, long long> LoadLastUsed(const std::string& folder) {
        std::map<std::string, long long> times;
        nlohmann::json settings = read();
        auto all = settings.find("extensionLastUsed");
        if (all == settings.end() || !all->is_object()) return times;
        auto saved = all->find(folder);
        if (saved == all->end() || !saved->is_object()) return times;
        for (auto& [name, time] : saved->items()) {
            if (time.is_number_integer()) times[name] = time.get<long long>();
        }
        return times;
    }

    void SaveLastUsed(const std::map<std::string, std::map<std::string, long long>>& lastUsed) {
        nlohmann::json settings = read();
        nlohmann::json& all = settings["extensionLastUsed"];
        if (!all.is_object()) all = nlohmann::json::object();
        for (const auto& [folder, times] : lastUsed) {
            if (times.empty()) all.erase(folder);
            else all[folder] = times;
        }
        if (all.empty()) settings.erase("extensionLastUsed");
        write(settings);
    }

    std::vector<std::string> LoadCalls(const std::string& folder) {
        std::vector<std::string> names;
        nlohmann::json settings = read();
        auto all = settings.find("extensionCalls");
        if (all == settings.end() || !all->is_object()) return names;
        auto saved = all->find(folder);
        if (saved == all->end() || !saved->is_array()) return names;
        for (const auto& name : *saved) {
            if (name.is_string()) names.push_back(name.get<std::string>());
        }
        return names;
    }

    void SaveCalls(const std::string& folder, const std::vector<std::string>& names) {
        nlohmann::json settings = read();
        nlohmann::json& all = settings["extensionCalls"];
        if (!all.is_object()) all = nlohmann::json::object();
        if (names.empty()) all.erase(folder);
        else all[folder] = names;
        if (all.empty()) settings.erase("extensionCalls");
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

    static std::vector<std::string> names(const nlohmann::json& saved, const char* field) {
        std::vector<std::string> list;
        auto it = saved.find(field);
        if (it == saved.end() || !it->is_array()) return list;
        for (const auto& name : *it) {
            if (name.is_string()) list.push_back(name.get<std::string>());
        }
        return list;
    }

    static Grants grantsFrom(const nlohmann::json& saved) {
        Grants grants;
        if (!saved.is_object()) return grants;
        // only picked keys: nothing answered yet
        grants.saved = saved.contains("granted");
        grants.hash = Files::stringField(saved, "hash");
        grants.names = names(saved, "granted");
        grants.never = names(saved, "never");
        grants.off = names(saved, "off");
        grants.keys = names(saved, "keys");
        auto picked = saved.find("picked");
        if (picked != saved.end() && picked->is_object()) {
            for (const auto& [author, name] : picked->items()) {
                if (name.is_string()) grants.picked[author] = name.get<std::string>();
            }
        }
        auto enabled = saved.find("enabled");
        if (enabled != saved.end() && enabled->is_boolean()) grants.enabled = enabled->get<bool>();
        return grants;
    }

    static nlohmann::json toJson(const Grants& grants) {
        if (!grants.saved) return { { "picked", grants.picked } };
        nlohmann::json saved = { { "hash", grants.hash }, { "granted", grants.names } };
        if (!grants.never.empty()) saved["never"] = grants.never;
        if (!grants.off.empty()) saved["off"] = grants.off;
        if (!grants.keys.empty()) saved["keys"] = grants.keys;
        if (!grants.picked.empty()) saved["picked"] = grants.picked;
        if (!grants.enabled) saved["enabled"] = false;
        return saved;
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
        // the old list had these unless the player took them away
        for (const char* name : { "display_overlay", "display_read", "internal_read", "internal_write" }) {
            if (std::find(grants.names.begin(), grants.names.end(), name) == grants.names.end()) grants.off.push_back(name);
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
