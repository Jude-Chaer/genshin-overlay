#include "lua_functions.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "overlay.hpp"
#include <imgui.h>
#include <iostream>

namespace LuaFunctions {
    static void callLua(const sol::protected_function& function) {
        auto result = function();
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Lua Runtime Error: " << err.what() << std::endl;
        }
    }

    // everything an extension can call
    void registerLuaFunctions(sol::state& luaState) {
        luaState.set_function("ImGUI_Text", ImGUI_Text);
        luaState.set_function("ImGUI_Button", ImGUI_Button);
        luaState.set_function("ImGUI_Image", ImGUI_Image);
        luaState.set_function("IsProgramHidden", IsProgramHidden);
        luaState.set_function("RegisterKeybind", RegisterKeybind);
        luaState.set_function("DeleteKeybind", DeleteKeybind);
        luaState.set_function("DefineExtensionSetting", DefineExtensionSetting);
        luaState.set_function("GetSetting", GetExtensionSetting);
        luaState.set_function("LoadTexture", LoadTextureFromFileLua);
        luaState["KeybindFlags"] = luaState.create_table_with(
            "None", Keybindings::KeybindFlags_None,
            "ProcessWhileHidden", Keybindings::KeybindFlags_ProcessWhileHidden
        );

        // key names for RegisterKeybind, like Keys.F5 or Keys.Grave
        sol::table keys = luaState.create_table();
        for (int key = GLFW_KEY_A; key <= GLFW_KEY_Z; key++) {
            keys[std::string(1, (char)key)] = key;
        }
        for (int key = GLFW_KEY_0; key <= GLFW_KEY_9; key++) {
            keys["Num" + std::string(1, (char)key)] = key;
        }
        for (int i = 1; i <= 12; i++) {
            keys["F" + std::to_string(i)] = GLFW_KEY_F1 + i - 1;
        }
        keys["Grave"] = GLFW_KEY_GRAVE_ACCENT;
        keys["Space"] = GLFW_KEY_SPACE;
        keys["Insert"] = GLFW_KEY_INSERT;
        keys["Delete"] = GLFW_KEY_DELETE;
        keys["Home"] = GLFW_KEY_HOME;
        keys["End"] = GLFW_KEY_END;
        luaState["Keys"] = keys;
    }

    void ImGUI_Text(const std::string& str) {
        ImGui::Text("%s", str.c_str());
    }

    void ImGUI_Button(const std::string& label, sol::protected_function callback) {
        if (ImGui::Button(label.c_str())) {
            callLua(callback);
        }
    }

    void ImGUI_Image(GLuint textureID, float width, float height) {
        ImGui::Image((ImTextureID)(intptr_t)textureID, ImVec2(width, height));
    }

    // hidden means the menu is closed
    bool IsProgramHidden() {
        return !Overlay::menuOpen;
    }

    void RegisterKeybind(int glfwKey, sol::protected_function callbackFunction, Keybindings::KeybindFlags flags) {
        // remember which extension made it, so GetSetting works inside the callback
        Extensions::Extension* owner = Extensions::currentExtension;
        Keybindings::CreateKeybind(glfwKey, [owner, callbackFunction]() {
            Extensions::runForExtension(owner, [&callbackFunction]() {
                callLua(callbackFunction);
            });
        }, flags);
    }

    void DeleteKeybind(int glfwKey) {
        Keybindings::DeleteKeybind(glfwKey);
    }

    void DefineExtensionSetting(const std::string& id,
        const std::string& label,
        const std::string& type,
        sol::object defaultValue)
    {
        auto extPtr = Extensions::currentExtension;
        if (!extPtr) {
            std::cerr << "Error: No current extension while defining setting!" << std::endl;
            return;
        }

        if (extPtr->settings.find(id) != extPtr->settings.end())
            return;

        Extensions::ExtensionSetting s;
        s.id = id;
        s.label = label;

        if (type == "bool" && defaultValue.is<bool>()) {
            s.type = Extensions::ExtensionSettingTypes::Bool;
            s.defaultValue = defaultValue.as<bool>();
        }
        else if (type == "int" && defaultValue.is<int>()) {
            s.type = Extensions::ExtensionSettingTypes::Int;
            s.defaultValue = defaultValue.as<int>();
        }
        else if (type == "float" && defaultValue.is<float>()) {
            s.type = Extensions::ExtensionSettingTypes::Float;
            s.defaultValue = defaultValue.as<float>();
        }
        else if (type == "string" && defaultValue.is<std::string>()) {
            s.type = Extensions::ExtensionSettingTypes::String;
            s.defaultValue = defaultValue.as<std::string>();
        }
        else {
            std::cerr << "Bad setting " << id << ": type " << type << " doesn't match its default value" << std::endl;
            return;
        }

        s.value = s.defaultValue;
        extPtr->settings[id] = s;
        extPtr->settingsOrder.push_back(id);
    }

    sol::object GetExtensionSetting(const std::string& id, sol::this_state state) {
        auto extPtr = Extensions::currentExtension;
        if (!extPtr) return sol::lua_nil;

        auto it = extPtr->settings.find(id);
        if (it == extPtr->settings.end()) return sol::lua_nil;

        return std::visit([&](auto&& value) {
            return sol::make_object(state, value);
        }, it->second.value);
    }

    std::tuple<GLuint, int, int> LoadTextureFromFileLua(const std::string& path) {
        int width = 0, height = 0;
        GLuint tex = Helpers::loadTextureFromFile(path, width, height);
        return { tex, width, height };
    }
}
