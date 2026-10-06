#pragma once
#include <GL/glew.h>
#include <sol/sol.hpp>
#include <string>
#include <tuple>
#include "keybindings.hpp"
#include "net.hpp"
#include "extensions.hpp"

namespace LuaFunctions {
    void registerLuaFunctions(sol::state& luaState);

    const std::unordered_map<std::string, std::vector<Extensions::ExtensionPermissionTypes>> functionPermissions = {
        { "LuaFunctions::LoadTexture", { Extensions::ExtensionPermissionTypes::InternalReadfile } },
        { "LuaFunctions::NetGet", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::NetPost", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::RegisterKeybind", { Extensions::ExtensionPermissionTypes::Keybinds } },
        { "LuaFunctions::DrawImage", {Extensions::ExtensionPermissionTypes::DisplayOverlay}},
        { "LuaFunctions::ReadFile", {Extensions::ExtensionPermissionTypes::InternalReadfile}}, // External has to be checked within the function, once the paths are resolved
		{ "LuaFunctions::WriteFile", {Extensions::ExtensionPermissionTypes::InternalWritefile}}, // External has to be checked within the function, once the paths are resolved
		{ "LuaFunctions::AppendFile", {Extensions::ExtensionPermissionTypes::InternalWritefile}}, // External has to be checked within the function, once the paths are resolved}
        { "LuaFunctions::WebSocketConnect", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::WebSocketSend", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::WebSocketPoll", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::WebSocketPollAll", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::WebSocketIsConnected", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::WebSocketClose", { Extensions::ExtensionPermissionTypes::NetworkAccess } },
        { "LuaFunctions::LoadTextureFromFileLua", {Extensions::ExtensionPermissionTypes::InternalReadfile}},
    };

    struct LuaInterval
    {
        int id;
        int milliseconds;
        double nextExecution;
        Extensions::Extension* owner;
        sol::protected_function callback;
    };
    static std::vector<LuaInterval> intervals;
    static int nextIntervalId = 1;

    bool checkPermissions(const std::string& functionName);

    void ImGUI_Text(const std::string& str);
    void ImGUI_Button(const std::string& label, sol::protected_function callback);
    void ImGUI_Image(GLuint textureID, float width, float height);
    bool IsProgramHidden();
    void RegisterKeybind(int glfwKey, sol::protected_function callbackFunction, Keybindings::KeybindFlags flags, sol::optional<int> modifiers);
    void DeleteKeybind(int glfwKey);
    void DefineExtensionSetting(const std::string& id, const std::string& label, const std::string& type, sol::object defaultValue,
        sol::optional<float> min, sol::optional<float> max);
    sol::object GetExtensionSetting(const std::string& id, sol::this_state state);
    std::tuple<GLuint, int, int> LoadTextureFromFileLua(const std::string& path);
    sol::object GetMapView(sol::this_state state);
    void FollowMapWhileMoving();
    sol::object GetMapGrid(int mapId, sol::this_state state);
    GLuint GetMapTile(int mapId, int x, int y, int size);
    sol::object GetMapFloor(int groupId, int floorId, sol::this_state state);
    void DrawImage(GLuint textureID, float x0, float y0, float x1, float y1, sol::optional<float> alpha);
    void GDrawText(const std::string& text, float x, float y, sol::optional<float> size, sol::optional<uint32_t> color); // The "G" is only there because DrawText is already used by the Windows API
    void PushClipRect(float x0, float y0, float x1, float y1);
    void PopClipRect();
    sol::object NetGet(const std::string& url, sol::this_state state, sol::optional<bool> hoyolab);
	sol::object NetPost(const std::string& url, const std::string& data, sol::this_state state, sol::optional<bool> hoyolab);
    sol::object JSONEncode(sol::object value, sol::this_state state);
    sol::object JSONDecode(const std::string& json, sol::this_state state);
	std::string ReadFile(const std::string& path);
	bool WriteFile(const std::string& path, const std::string& contents);
	bool AppendFile(const std::string& path, const std::string& contents);
    bool WebSocketConnect(Net::WebSocket& socket, const std::string& url, sol::optional<bool> hoyolab);
    bool WebSocketSend(Net::WebSocket& socket, const std::string& message);
    sol::object WebSocketPoll(Net::WebSocket& socket, sol::this_state state);
    sol::object WebSocketPollAll(Net::WebSocket& socket, sol::this_state state);
    bool WebSocketIsConnected(Net::WebSocket& socket);
    void WebSocketClose(Net::WebSocket& socket);
	double CompareImages(GLuint textureID1, GLuint textureID2, int accuracy);
    double GetOSTimeMS(); // Yeah I think we need to expose this because Lua doesn't have a way to get time, since with don't include sol::lib:os
    int SetInterval(sol::protected_function callback, int milliseconds);
    void ClearInterval(int id);
    void UpdateIntervals();
    std::tuple<int, int> GetMousePosition();

}
