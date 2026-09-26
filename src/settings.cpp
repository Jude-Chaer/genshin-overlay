#include "settings.hpp"
#include "helpers.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace Settings {
    bool runAsAdmin = false;

    static std::filesystem::path settingsPath() {
        return Helpers::exeDirectory() / "settings.json";
    }

    void Load() {
        std::ifstream file(settingsPath());
        if (!file) return;

        nlohmann::json settings = nlohmann::json::parse(file, nullptr, false);
        if (settings.is_discarded()) return;
        runAsAdmin = settings.value("runAsAdmin", false);
    }

    void Save() {
        nlohmann::json settings = {
            { "runAsAdmin", runAsAdmin },
        };
        std::ofstream file(settingsPath(), std::ios::trunc);
        file << settings.dump(2);
    }
}
