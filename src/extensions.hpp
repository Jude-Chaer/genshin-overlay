#pragma once
#include <GL/glew.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <sol/sol.hpp>

namespace Extensions {
    enum class ExtensionSettingTypes {
        Bool,
        Int,
        Float,
        String
    };

    enum class ExtensionPermissionTypes {
        None,
        ExternalReadfile,
		InternalReadfile,
		ExternalWritefile,
        InternalWritefile,
		NetworkAccess,
		DisplayOverlay,
    };

    struct ExtensionSetting {
        std::string id;
        std::string label;
        ExtensionSettingTypes type;

        std::variant<bool, int, float, std::string> defaultValue;
        std::variant<bool, int, float, std::string> value;
        // a float with both of these set gets a slider
        std::optional<float> min, max;
    };

    class Extension {
    public:
        std::string name;
        std::string description;
        std::string author;
        std::string version;
        std::filesystem::path folder;
        std::unordered_map<std::string, ExtensionSetting> settings;
        std::vector<std::string> settingsOrder;
        std::function<void()> initializeFunction;
        std::function<void()> shutdownFunction;
        std::function<void()> registerSettings;
        std::function<void()> updateFunction;
        std::function<void()> menuFunction;
        GLuint extensionImage = 0;
        int extensionImageWidth = 0;
        int extensionImageHeight = 0;
        static constexpr int extensionImageDisplaySize = 64;
    };


    extern std::vector<std::unique_ptr<Extension>> registeredExtensions;
    extern sol::state globalLuaState;
    extern Extension* currentExtension;
    extern void initExtensions();
    extern void destroyExtensions();
    extern void frameUpdateExtensions();
    extern void drawExtensionMenus(int extensionIndex);
    extern void findAndLoadExtensions();
    extern void configureSettings();
    extern void loadLuaExtension(const std::filesystem::path& scriptPath, const std::filesystem::path& folderPath);
    extern void createGlobalLuaState();
    extern void runForExtension(Extension* ext, const std::function<void()>& function);
}
