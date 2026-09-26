#include "extensions.hpp"
#include "helpers.hpp"
#include "lua_functions.hpp"
#include <imgui.h>
#include <iostream>


namespace Extensions {
    std::vector<std::unique_ptr<Extension>> registeredExtensions;
    sol::state globalLuaState;
    Extension* currentExtension = nullptr;

    // Lua functions like DefineExtensionSetting need to know which extension called them
    void runForExtension(Extension* ext, const std::function<void()>& function) {
        if (!function) return;
        Extension* previous = currentExtension;
        currentExtension = ext;
        function();
        currentExtension = previous;
    }

    // settings are registered before Init so Init can already read them
    void initExtensions() {
        createGlobalLuaState();
        findAndLoadExtensions();
        configureSettings();

        for (auto& ext : registeredExtensions) {
            runForExtension(ext.get(), ext->initializeFunction);
        }
    }

    void destroyExtensions() {
        for (auto& ext : registeredExtensions) {
            runForExtension(ext.get(), ext->shutdownFunction);
            if (ext->extensionImage != 0) {
                glDeleteTextures(1, &ext->extensionImage);
            }
        }
        registeredExtensions.clear();
    }

    void frameUpdateExtensions() {
        for (auto& ext : registeredExtensions) {
            runForExtension(ext.get(), ext->updateFunction);
        }
    }

    static void drawSetting(ExtensionSetting& setting) {
        switch (setting.type) {
        case ExtensionSettingTypes::Bool:
            ImGui::Checkbox(setting.label.c_str(), &std::get<bool>(setting.value));
            break;
        case ExtensionSettingTypes::Int:
            ImGui::InputInt(setting.label.c_str(), &std::get<int>(setting.value));
            break;
        case ExtensionSettingTypes::Float:
            ImGui::InputFloat(setting.label.c_str(), &std::get<float>(setting.value));
            break;
        case ExtensionSettingTypes::String: {
            // ImGui edits text in a plain char buffer
            std::string& text = std::get<std::string>(setting.value);
            char buffer[256];
            strncpy_s(buffer, text.c_str(), _TRUNCATE);
            if (ImGui::InputText(setting.label.c_str(), buffer, sizeof(buffer))) {
                text = buffer;
            }
            break;
        }
        }
    }

    // one section per extension: icon, info, settings, then its own Menu()
    void drawExtensionMenus() {
        if (registeredExtensions.empty()) {
            ImGui::TextDisabled("No extensions found");
            return;
        }

        for (auto& ext : registeredExtensions) {
            ImGui::PushID(ext.get());
            if (ImGui::TreeNode(ext->name.c_str())) {
                if (ext->extensionImage != 0) {
                    float size = (float)Extension::extensionImageDisplaySize;
                    ImGui::Image((ImTextureID)(intptr_t)ext->extensionImage, ImVec2(size, size));
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                ImGui::TextWrapped("%s", ext->description.c_str());
                ImGui::TextDisabled("%s  v%s", ext->author.c_str(), ext->version.c_str());
                ImGui::EndGroup();

                for (const auto& id : ext->settingsOrder) {
                    drawSetting(ext->settings[id]);
                }

                runForExtension(ext.get(), ext->menuFunction);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void configureSettings() {
        for (auto& ext : registeredExtensions) {
            runForExtension(ext.get(), ext->registerSettings);
        }
    }

    // every folder in extensions/ with a main.lua in it is an extension
    void findAndLoadExtensions() {
        std::filesystem::path extensionsRoot = Helpers::exeDirectory() / "extensions";

        if (!std::filesystem::exists(extensionsRoot)) {
            std::filesystem::create_directory(extensionsRoot);
            return;
        }

        for (const auto& dirEntry : std::filesystem::directory_iterator(extensionsRoot)) {
            if (!dirEntry.is_directory()) continue;

            std::filesystem::path scriptPath = dirEntry.path() / "main.lua";
            if (std::filesystem::exists(scriptPath)) {
                std::cout << "Found extension in: " << dirEntry.path().filename() << std::endl;
                loadLuaExtension(scriptPath, dirEntry.path());
            }
        }
    }

    // wraps a Lua function so an error gets printed instead of taking the overlay down
    static std::function<void()> getLuaFunction(sol::environment& env, const char* luaName, const std::string& extensionName) {
        sol::optional<sol::protected_function> function = env[luaName];
        if (!function) return nullptr;

        return [function, luaName, extensionName]() {
            auto result = function->call();
            if (!result.valid()) {
                sol::error err = result;
                std::cerr << "[" << extensionName << "] " << luaName << ": " << err.what() << std::endl;
            }
        };
    }

    void loadLuaExtension(const std::filesystem::path& scriptPath, const std::filesystem::path& folderPath) {
        auto ext = std::make_unique<Extension>();
        ext->folder = folderPath;
        ext->name = folderPath.filename().string();

        // each extension gets its own environment so their globals don't clash
        sol::environment env(globalLuaState, sol::create, globalLuaState.globals());
        env["WORKING_DIR"] = folderPath.string() + "/";

        sol::load_result script = globalLuaState.load_file(scriptPath.string());
        if (!script.valid()) {
            sol::error err = script;
            std::cerr << "Failed to load " << scriptPath << ": " << err.what() << std::endl;
            return;
        }

        sol::protected_function scriptFunction = script;
        env.set_on(scriptFunction);
        auto result = scriptFunction();
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Failed to run " << scriptPath << ": " << err.what() << std::endl;
            return;
        }

        sol::optional<sol::table> meta = env["metadata"];
        if (meta) {
            ext->name = meta->get_or("name", ext->name);
            ext->description = meta->get_or("description", std::string("No description."));
            ext->author = meta->get_or("author", std::string("Unknown"));
            ext->version = meta->get_or("version", std::string("1.0.0"));
        }

        std::filesystem::path iconPath = folderPath / "icon.png";
        if (std::filesystem::exists(iconPath)) {
            ext->extensionImage = Helpers::loadTextureFromFile(iconPath.string(), ext->extensionImageWidth, ext->extensionImageHeight);
        }

        ext->initializeFunction = getLuaFunction(env, "Init", ext->name);
        ext->updateFunction = getLuaFunction(env, "Update", ext->name);
        ext->menuFunction = getLuaFunction(env, "Menu", ext->name);
        ext->registerSettings = getLuaFunction(env, "RegisterSettings", ext->name);
        ext->shutdownFunction = getLuaFunction(env, "Shutdown", ext->name);

        registeredExtensions.push_back(std::move(ext));
    }

    void createGlobalLuaState() {
        globalLuaState.open_libraries(sol::lib::base, sol::lib::package, sol::lib::math, sol::lib::string, sol::lib::table);
        LuaFunctions::registerLuaFunctions(globalLuaState);
    }
}
