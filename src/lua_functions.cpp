#include <windows.h>
#include <nlohmann/json.hpp>
#include "lua_functions.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "overlay.hpp"
#include "map_tiles.hpp"
#include "map_tracking.hpp"
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
        luaState.set_function("GetMapView", GetMapView);
        luaState.set_function("GetMapGrid", GetMapGrid);
        luaState.set_function("GetMapTile", GetMapTile);
        luaState.set_function("GetMapFloor", GetMapFloor);
        luaState.set_function("DrawImage", DrawImage);
        luaState.set_function("PushClipRect", PushClipRect);
        luaState.set_function("PopClipRect", PopClipRect);
        luaState.set_function("Net_Get", NetGet);
        luaState.set_function("Net_Post", NetPost);
        luaState.set_function("JSON_Encode", JSONEncode);
		luaState.set_function("JSON_Decode", JSONDecode);
        luaState["KeybindFlags"] = luaState.create_table_with(
            "None", Keybindings::KeybindFlags_None,
            "ProcessWhileHidden", Keybindings::KeybindFlags_ProcessWhileHidden
        );
        luaState["KeybindModifiers"] = luaState.create_table_with(
            "None", Keybindings::KeybindModifiers_None,
            "Alt", Keybindings::KeybindModifiers_Alt,
            "Ctrl", Keybindings::KeybindModifiers_Ctrl,
            "Shift", Keybindings::KeybindModifiers_Shift
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

    // RegisterKeybind(key, function, flags, modifiers), modifiers is optional, like KeybindModifiers.Alt
    void RegisterKeybind(int glfwKey, sol::protected_function callbackFunction, Keybindings::KeybindFlags flags, sol::optional<int> modifiers) {
        // remember which extension made it, so GetSetting works inside the callback
        Extensions::Extension* owner = Extensions::currentExtension;
        Keybindings::CreateKeybind(glfwKey, [owner, callbackFunction]() {
            Extensions::runForExtension(owner, [&callbackFunction]() {
                callLua(callbackFunction);
            });
        }, flags, modifiers.value_or(Keybindings::KeybindModifiers_None));
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

    sol::object NetGet(const std::string& url, sol::this_state state, sol::optional<bool> hoyolab)
    {
        sol::state_view lua(state);
        Net::Response response;

        if (!Net::get(url, response, hoyolab.value_or(false)))
            return sol::nil;

        sol::table result = lua.create_table();
        result["status"] = response.status;
        result["body"] = response.body;

        return result;
    }

    sol::object NetPost(const std::string& url,const std::string& data,sol::this_state state,sol::optional<bool> hoyolab)
    {
        sol::state_view lua(state);
        Net::Response response;

        if (!Net::post(url, data, response, hoyolab.value_or(false)))
            return sol::nil;

        sol::table result = lua.create_table();
        result["status"] = response.status;
        result["body"] = response.body;

        return result;
    }

    // Where the in-game map is looking, or nil when it isn't open. Positions are
    // in overlay pixels, the same ones DrawImage uses. lat/lng is the map
    // position at centerX, centerY.
    sol::object GetMapView(sol::this_state state) {
        MapTracking::MapView view = MapTracking::GetView();
        if (!view.visible) return sol::lua_nil;

        int windowX = 0, windowY = 0;
        glfwGetWindowPos(Overlay::Window, &windowX, &windowY);
        float left = (float)(view.gameRect.left - windowX);
        float top = (float)(view.gameRect.top - windowY);
        float right = (float)(view.gameRect.right - windowX);
        float bottom = (float)(view.gameRect.bottom - windowY);

        sol::state_view lua(state);
        return sol::make_object(state, lua.create_table_with(
            "mapId", view.result.mapId,
            "groupId", view.result.groupId,
            "floorId", view.result.floorId,
            "lat", view.result.lat,
            "lng", view.result.lng,
            "unitsPerPixel", view.result.unitsPerPixel,
            "centerX", (left + right) / 2.0f,
            "centerY", (top + bottom) / 2.0f,
            "left", left,
            "top", top,
            "right", right,
            "bottom", bottom
        ));
    }

    // how a map's tiles are laid out: tile x covers map units x * tileSize - originX and up
    sol::object GetMapGrid(int mapId, sol::this_state state) {
        MapTiles::Grid grid;
        if (!MapTiles::GetGrid(mapId, grid)) return sol::lua_nil;

        sol::state_view lua(state);
        return sol::make_object(state, lua.create_table_with(
            "cols", grid.cols,
            "rows", grid.rows,
            "tileSize", grid.tileSize,
            "originX", grid.originX,
            "originY", grid.originY
        ));
    }

    GLuint GetMapTile(int mapId, int x, int y, int size) {
        return MapTiles::GetTile(mapId, x, y, size);
    }

    // an underground floor's image and the box of map units it covers, nil while it loads
    sol::object GetMapFloor(int groupId, int floorId, sol::this_state state) {
        MapTiles::Floor floor;
        if (!MapTiles::GetFloor(groupId, floorId, floor)) return sol::lua_nil;

        sol::state_view lua(state);
        return sol::make_object(state, lua.create_table_with(
            "texture", floor.texture,
            "left", floor.left,
            "top", floor.top,
            "right", floor.right,
            "bottom", floor.bottom
        ));
    }

    // draws behind every ImGui window, straight onto the overlay
    void DrawImage(GLuint textureID, float x0, float y0, float x1, float y1, sol::optional<float> alpha) {
        if (textureID == 0) return;
        int a = (int)(alpha.value_or(1.0f) * 255.0f);
        ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)textureID, ImVec2(x0, y0), ImVec2(x1, y1),
            ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
    }

    void PushClipRect(float x0, float y0, float x1, float y1) {
        ImGui::GetBackgroundDrawList()->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    }

    void PopClipRect() {
        ImGui::GetBackgroundDrawList()->PopClipRect();
    }

    static nlohmann::json LuaToJson(sol::object obj) {
        switch (obj.get_type()) {
        case sol::type::nil:
            return nullptr;

        case sol::type::boolean:
            return obj.as<bool>();

        case sol::type::number:
            return obj.as<double>();

        case sol::type::string:
            return obj.as<std::string>();

        case sol::type::table: {
            sol::table table = obj.as<sol::table>();

            bool isArray = true;
            size_t count = 0;

            for (const auto& pair : table) {
                if (pair.first.get_type() != sol::type::number) {
                    isArray = false;
                    break;
                }

                double key = pair.first.as<double>();

                if (key < 1 || key != static_cast<size_t>(key)) {
                    isArray = false;
                    break;
                }

                count++;
            }

            if (isArray) {
                for (size_t i = 1; i <= count; i++) {
                    if (!table.raw_get<sol::object>(i).valid()) {
                        isArray = false;
                        break;
                    }
                }
            }

            if (isArray) {
                nlohmann::json result = nlohmann::json::array();

                for (size_t i = 1; i <= count; i++) {
                    result.push_back(LuaToJson(table.raw_get<sol::object>(i)));
                }

                return result;
            }

            nlohmann::json result = nlohmann::json::object();

            for (const auto& pair : table) {
                if (pair.first.get_type() != sol::type::string)
                    throw std::runtime_error("JSON object keys must be strings");

                std::string key = pair.first.as<std::string>();
                result[key] = LuaToJson(pair.second);
            }

            return result;
        }

        default:
            throw std::runtime_error("Unsupported Lua type for JSON encoding");
        }
    }

    static sol::object JsonToLua(const nlohmann::json& value, sol::state_view lua) {
        if (value.is_null())
            return sol::make_object(lua, sol::lua_nil);

        if (value.is_boolean())
            return sol::make_object(lua, value.get<bool>());

        if (value.is_number_integer())
            return sol::make_object(lua, value.get<long long>());

        if (value.is_number())
            return sol::make_object(lua, value.get<double>());

        if (value.is_string())
            return sol::make_object(lua, value.get<std::string>());

        if (value.is_array()) {
            sol::table result = lua.create_table();

            for (size_t i = 0; i < value.size(); i++) {
                result[i + 1] = JsonToLua(value[i], lua);
            }

            return sol::make_object(lua, result);
        }

        if (value.is_object()) {
            sol::table result = lua.create_table();

            for (auto it = value.begin(); it != value.end(); ++it) {
                result[it.key()] = JsonToLua(it.value(), lua);
            }

            return sol::make_object(lua, result);
        }

        return sol::lua_nil;
    }

    sol::object JSONEncode(sol::object value, sol::this_state state) {
        try {
            std::string encoded = LuaToJson(value).dump();
            return sol::make_object(state, encoded);
        }
        catch (const std::exception& e) {
            std::cerr << "JSON Encode Error: " << e.what() << std::endl;
            return sol::lua_nil;
        }
    }

    sol::object JSONDecode(const std::string& input, sol::this_state state) {
        try {
            nlohmann::json decoded = nlohmann::json::parse(input);
            sol::state_view lua(state);
            return JsonToLua(decoded, lua);
        }
        catch (const std::exception& e) {
            std::cerr << "JSON Decode Error: " << e.what() << std::endl;
            return sol::lua_nil;
        }
    }
}
