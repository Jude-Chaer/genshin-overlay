#include "settings.hpp"
#include "helpers.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace Settings {
    bool runAsAdmin = false;
    bool lineUpWithMap = true;
    bool ignoreSea = true;
    bool steadyZoom = true;
    bool guessAhead = true;
    bool lookAgainStill = true;
    bool followOnThread = true;
    bool shrinkOnGpu = true;
    bool findSmallFirst = true;

    static std::filesystem::path settingsPath() {
        return Helpers::exeDirectory() / "settings.json";
    }

    void Load() {
        std::ifstream file(settingsPath());
        if (!file) return;

        nlohmann::json settings = nlohmann::json::parse(file, nullptr, false);
        if (settings.is_discarded()) return;
        runAsAdmin = settings.value("runAsAdmin", false);
        lineUpWithMap = settings.value("lineUpWithMap", true);
        ignoreSea = settings.value("ignoreSea", true);
        steadyZoom = settings.value("steadyZoom", true);
        guessAhead = settings.value("guessAhead", true);
        lookAgainStill = settings.value("lookAgainStill", true);
        followOnThread = settings.value("followOnThread", true);
        shrinkOnGpu = settings.value("shrinkOnGpu", true);
        findSmallFirst = settings.value("findSmallFirst", true);
    }

    static nlohmann::json read() {
        std::ifstream file(settingsPath());
        if (!file) return nlohmann::json::object();
        nlohmann::json settings = nlohmann::json::parse(file, nullptr, false);
        if (settings.is_discarded() || !settings.is_object()) return nlohmann::json::object();
        return settings;
    }

    static void write(const nlohmann::json& settings) {
        std::ofstream file(settingsPath(), std::ios::trunc);
        file << settings.dump(2);
    }

    // extension settings are in the same file, don't wipe them
    void Save() {
        nlohmann::json settings = read();
        settings["runAsAdmin"] = runAsAdmin;
        settings["lineUpWithMap"] = lineUpWithMap;
        settings["ignoreSea"] = ignoreSea;
        settings["steadyZoom"] = steadyZoom;
        settings["guessAhead"] = guessAhead;
        settings["lookAgainStill"] = lookAgainStill;
        settings["followOnThread"] = followOnThread;
        settings["shrinkOnGpu"] = shrinkOnGpu;
        settings["findSmallFirst"] = findSmallFirst;
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

    void SaveExtensionPermissions(std::string extensionHash, std::unordered_set<Extensions::ExtensionPermissionTypes> permissions) {
        nlohmann::json settings = read();
        nlohmann::json& extPermissions = settings["extensionPermissions"][extensionHash];
        extPermissions = nlohmann::json::array();
        for (const auto& permission : permissions) {
            extPermissions.push_back(static_cast<int>(permission));
        }
        write(settings);
	}
    std::unordered_set<Extensions::ExtensionPermissionTypes> LoadExtensionPermissions(std::string extensionHash) {
        nlohmann::json settings = read();
        std::unordered_set<Extensions::ExtensionPermissionTypes> permissions;
        auto it = settings.find("extensionPermissions");
        if (it == settings.end() || !it->is_object() || !it->contains(extensionHash)) {
			return Extensions::defaultPermissions;
        }
        if (it != settings.end() && it->is_object()) {
            auto extIt = it->find(extensionHash);
            if (extIt != it->end() && extIt->is_array()) {
                for (const auto& perm : *extIt) {
                    if (perm.is_number_integer()) {
                        permissions.insert(static_cast<Extensions::ExtensionPermissionTypes>(perm.get<int>()));
                    }
                }
            }
        }
        return permissions;
	}
}
