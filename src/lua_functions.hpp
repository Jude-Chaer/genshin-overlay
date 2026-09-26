#pragma once
#include <GL/glew.h>
#include <sol/sol.hpp>
#include <string>
#include <tuple>
#include "keybindings.hpp"

namespace LuaFunctions {
    void registerLuaFunctions(sol::state& luaState);

    void ImGUI_Text(const std::string& str);
    void ImGUI_Button(const std::string& label, sol::protected_function callback);
    void ImGUI_Image(GLuint textureID, float width, float height);
    bool IsProgramHidden();
    void RegisterKeybind(int glfwKey, sol::protected_function callbackFunction, Keybindings::KeybindFlags flags);
    void DeleteKeybind(int glfwKey);
    void DefineExtensionSetting(const std::string& id, const std::string& label, const std::string& type, sol::object defaultValue);
    sol::object GetExtensionSetting(const std::string& id, sol::this_state state);
    std::tuple<GLuint, int, int> LoadTextureFromFileLua(const std::string& path);

    sol::object GetMapView(sol::this_state state);
    sol::object GetMapGrid(int mapId, sol::this_state state);
    GLuint GetMapTile(int mapId, int x, int y, int size);
    sol::object GetMapFloor(int groupId, int floorId, sol::this_state state);
    void DrawImage(GLuint textureID, float x0, float y0, float x1, float y1, sol::optional<float> alpha);
    void PushClipRect(float x0, float y0, float x1, float y1);
    void PopClipRect();
}
