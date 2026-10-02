#include "settings.hpp"
#include "helpers.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace Settings {
    bool runAsAdmin = false;
    bool ignoreSea = true;
    bool steadyZoom = true;
    bool guessAhead = true;
    bool lookAgainStill = true;
    bool followOnThread = true;
    bool shrinkOnGpu = true;

    static std::filesystem::path settingsPath() {
        return Helpers::exeDirectory() / "settings.json";
    }

    void Load() {
        std::ifstream file(settingsPath());
        if (!file) return;

        nlohmann::json settings = nlohmann::json::parse(file, nullptr, false);
        if (settings.is_discarded()) return;
        runAsAdmin = settings.value("runAsAdmin", false);
        ignoreSea = settings.value("ignoreSea", true);
        steadyZoom = settings.value("steadyZoom", true);
        guessAhead = settings.value("guessAhead", true);
        lookAgainStill = settings.value("lookAgainStill", true);
        followOnThread = settings.value("followOnThread", true);
        shrinkOnGpu = settings.value("shrinkOnGpu", true);
    }

    void Save() {
        nlohmann::json settings = {
            { "runAsAdmin", runAsAdmin },
            { "ignoreSea", ignoreSea },
            { "steadyZoom", steadyZoom },
            { "guessAhead", guessAhead },
            { "lookAgainStill", lookAgainStill },
            { "followOnThread", followOnThread },
            { "shrinkOnGpu", shrinkOnGpu },
        };
        std::ofstream file(settingsPath(), std::ios::trunc);
        file << settings.dump(2);
    }
}
