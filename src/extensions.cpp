#include "extensions.hpp"
#include "helpers.hpp"
#include "lua_functions.hpp"
#include "settings.hpp"
#include <imgui.h>
#include <iostream>


namespace Extensions {
    std::vector<std::unique_ptr<Extension>> registeredExtensions;
    Extension* currentExtension = nullptr;

    // Lua functions like DefineExtensionSetting need to know which extension called them
    void runForExtension(Extension* ext, const std::function<void()>& function) {
        if (!function) return;

        Extension* previous = currentExtension;
        currentExtension = ext;

        try {
            function();
        }
        catch (...) {
            currentExtension = previous;
            throw;
        }

        currentExtension = previous;
    }

    // settings are registered before Init so Init can already read them
    void initExtensions() {
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

    // kept per extension folder in settings.json
    static void saveSettings() {
        nlohmann::json all = Settings::LoadExtensions();
        for (auto& ext : registeredExtensions) {
            nlohmann::json& saved = all[ext->folder.filename().string()];
            for (const auto& [id, setting] : ext->settings) {
                std::visit([&](auto&& value) { saved[id] = value; }, setting.value);
            }
        }
        Settings::SaveExtensions(all);
    }

    // a saved value that isn't the setting's type anymore is left out
    static void loadSettings() {
        nlohmann::json all = Settings::LoadExtensions();
        for (auto& ext : registeredExtensions) {
            auto saved = all.find(ext->folder.filename().string());
            if (saved == all.end() || !saved->is_object()) continue;
            for (auto& [id, setting] : ext->settings) {
                auto it = saved->find(id);
                if (it == saved->end()) continue;
                std::visit([&](auto&& current) {
                    try { setting.value = it->get<std::decay_t<decltype(current)>>(); }
                    catch (const nlohmann::json::exception&) {}
                }, setting.value);
            }
        }
    }

    // true once a change is done (let go of the slider, clicked, ...), so it gets saved then
    static bool drawSetting(ExtensionSetting& setting) {
        bool done = false;
        switch (setting.type) {
        case ExtensionSettingTypes::Bool:
            done = ImGui::Checkbox(setting.label.c_str(), &std::get<bool>(setting.value));
            break;
        case ExtensionSettingTypes::Int:
            ImGui::InputInt(setting.label.c_str(), &std::get<int>(setting.value));
            done = ImGui::IsItemDeactivatedAfterEdit();
            break;
        case ExtensionSettingTypes::Float:
            if (setting.min && setting.max) {
                // Slider, and next to it the number, drag on it for small steps.
                // No typing, the overlay never gets the keyboard, the game keeps it.
                float& value = std::get<float>(setting.value);
                ImGui::PushID(setting.id.c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
                ImGui::SliderFloat("##slider", &value, *setting.min, *setting.max, "", ImGuiSliderFlags_NoInput);
                done |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
                float step = (*setting.max - *setting.min) / 200.0f;
                ImGui::DragFloat("##number", &value, step, *setting.min, *setting.max, "%.2f",
                    ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
                done |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SameLine();
                ImGui::TextUnformatted(setting.label.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(value == std::get<float>(setting.defaultValue));
                if (ImGui::SmallButton("Reset")) {
                    value = std::get<float>(setting.defaultValue);
                    done = true;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            else {
                ImGui::InputFloat(setting.label.c_str(), &std::get<float>(setting.value));
                done = ImGui::IsItemDeactivatedAfterEdit();
            }
            break;
        case ExtensionSettingTypes::String: {
            // ImGui edits text in a plain char buffer
            std::string& text = std::get<std::string>(setting.value);
            char buffer[256];
            strncpy_s(buffer, text.c_str(), _TRUNCATE);
            if (ImGui::InputText(setting.label.c_str(), buffer, sizeof(buffer))) {
                text = buffer;
            }
            done = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        }
        return done;
    }

    // one section per extension: icon, info, settings, then its own Menu()
    void drawExtensionMenus(int extensionIndex) {
        if (extensionIndex < 0 ||
            extensionIndex >= static_cast<int>(registeredExtensions.size())) {
            ImGui::TextDisabled("Extension not found");
            return;
        }

        auto& ext = registeredExtensions[extensionIndex];

        ImGui::PushID(ext.get());

        if (ext->extensionImage != 0) {
            float size =
                static_cast<float>(
                    Extension::extensionImageDisplaySize
                    );

            ImGui::Image(
                (ImTextureID)(intptr_t)ext->extensionImage,
                ImVec2(size, size)
            );

            ImGui::SameLine();
        }

        ImGui::BeginGroup();

        ImGui::Text(
            "%s",
            ext->name.c_str()
        );

        ImGui::TextWrapped(
            "%s",
            ext->description.c_str()
        );

        ImGui::TextDisabled(
            "%s  v%s",
            ext->author.c_str(),
            ext->version.c_str()
        );

        ImGui::EndGroup();

        ImGui::Separator();


        for (const auto& id : ext->settingsOrder) {
            auto it = ext->settings.find(id);

            if (it != ext->settings.end() && drawSetting(it->second)) {
                saveSettings();
            }
        }

        runForExtension(
            ext.get(),
            ext->menuFunction
        );

        ImGui::PopID();
    }

    void configureSettings() {
        for (auto& ext : registeredExtensions) {
            runForExtension(ext.get(), ext->registerSettings);
        }
        loadSettings();
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

        ext->luaState = std::make_unique<sol::state>();

        ext->luaState->open_libraries(
            sol::lib::base,
            sol::lib::package,
            sol::lib::math,
            sol::lib::string,
            sol::lib::table
        );
        ext->luaState->globals()["load"] = sol::nil;
        ext->luaState->globals()["loadfile"] = sol::nil;
        ext->luaState->globals()["dofile"] = sol::nil;
        ext->luaState->globals()["loadstring"] = sol::nil;
        ext->luaState->globals()["package"]["path"] = folderPath.string() + "/?.lua";
        ext->luaState->globals()["package"]["cpath"] = "";
        ext->luaState->globals()["package"]["loadlib"] = sol::nil;
        LuaFunctions::registerLuaFunctions(*ext->luaState);

        std::vector<std::pair<std::string, std::string>> luaFileHashes;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folderPath)) {
            if (!entry.is_regular_file())
                continue;

            if (entry.path().extension() != ".lua")
                continue;

            std::filesystem::path relative =
                std::filesystem::relative(entry.path(), folderPath);

            std::string fileHash = Helpers::hashFile(entry.path());

            if (fileHash.empty()) {
                std::cerr << "Failed to hash " << entry.path() << std::endl;
                return;
            }

            luaFileHashes.emplace_back(
                relative.generic_string(),
                fileHash
            );
        }
        std::sort(
            luaFileHashes.begin(),
            luaFileHashes.end(),
            [](const auto& a, const auto& b) {
                return a.first < b.first;
            }
        );

        std::string combined;

        for (const auto& [path, hash] : luaFileHashes) {
            combined += path;
            combined += "\n";
            combined += hash;
            combined += "\n";
        }

        ext->extensionHash = Helpers::hashString(combined);
        loadExtensionPermissions(ext.get());

        sol::environment env(
            *ext->luaState,
            sol::create,
            ext->luaState->globals()
        );

        env["WORKING_DIR"] = folderPath.string() + "/";

        sol::load_result script = ext->luaState->load_file(scriptPath.string());
        if (!script.valid()) {
            sol::error err = script;
            std::cerr << "Failed to load " << scriptPath << ": " << err.what() << std::endl;
            return;
        }

        sol::protected_function scriptFunction = script;
        env.set_on(scriptFunction);

        sol::protected_function_result result;

        runForExtension(ext.get(), [&]() {
            result = scriptFunction();
            });

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
            ext->extensionImage = Helpers::loadTextureFromFile(
                iconPath.string(),
                ext->extensionImageWidth,
                ext->extensionImageHeight
            );
        }
        

        ext->initializeFunction = getLuaFunction(env, "Init", ext->name);
        ext->updateFunction = getLuaFunction(env, "Update", ext->name);
        ext->menuFunction = getLuaFunction(env, "Menu", ext->name);
        ext->registerSettings = getLuaFunction(env, "RegisterSettings", ext->name);
        ext->shutdownFunction = getLuaFunction(env, "Shutdown", ext->name);
		std::cout << "Loaded extension: " << ext->name << " Hash: " << ext->extensionHash << std::endl;
        registeredExtensions.push_back(std::move(ext));
    }

    void drawExtensionPermissionMenu() {
        ImGui::TextWrapped(
            "Extensions can be given permissions to access certain features.  Some of these are sensitive and should be granted with caution.");
        for (auto& ext : registeredExtensions) {
            ImGui::Separator();
            ImGui::Text("%s", ext->name.c_str());
            ImGui::TextDisabled("%s  v%s", ext->author.c_str(), ext->version.c_str());
            ImGui::TextWrapped("%s", ext->description.c_str());
            ImGui::Separator();
            ImGui::PushID(ext.get());
            for (auto permission : allPermissions) {
                if (permission.first == ExtensionPermissionTypes::None) continue;
                bool hasPermission = ext->extensionPermissions.count(permission.first) > 0;
                std::string permissionName = permission.second;
                if (ImGui::Checkbox(permissionName.c_str(), &hasPermission)) {
                    if (hasPermission) {
                        ext->extensionPermissions.insert(permission.first);
                    }
                    else {
                        ext->extensionPermissions.erase(permission.first);
                    }
                    Settings::SaveExtensionPermissions(ext->extensionHash, ext->extensionPermissions);
                }
            }
            ImGui::PopID();
        }
    }
    void loadExtensionPermissions(Extension* ext) {
        ext->extensionPermissions = Settings::LoadExtensionPermissions(ext->extensionHash);
    }
}
