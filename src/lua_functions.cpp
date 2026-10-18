#include <windows.h>
#include <nlohmann/json.hpp>
#include "lua_functions.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "overlay.hpp"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include "map_tiles.hpp"
#include "point_guides.hpp"
#include "web_images.hpp"
#include "file_dialogs.hpp"
#include "game_ui.hpp"
#include "hoyolab_account.hpp"
#include "hoyolab_marks.hpp"
#include "map_tracking.hpp"
#include "minimap.hpp"
#include "packets/game_traffic.hpp"
#include "settings.hpp"
#include <imgui.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <chrono>
#include <algorithm>

#define CHECK_PERMISSIONS() \
    if (!LuaFunctions::checkPermissions(__FUNCTION__)) \
        throw std::runtime_error("Extension does not have permission")


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
		luaState.set_function("ImGUI_Checkbox", ImGUI_Checkbox);
		luaState.set_function("ImGUI_SliderFloat", ImGUI_SliderFloat);
		luaState.set_function("ImGUI_SliderInt", ImGUI_SliderInt);
		luaState.set_function("ImGUI_InputText", ImGUI_InputText);
		luaState.set_function("ImGUI_SameLine", ImGUI_SameLine);
		luaState.set_function("ImGUI_Tooltip", ImGUI_Tooltip);
		luaState.set_function("ImGUI_Separator", ImGUI_Separator);
		luaState.set_function("ImGUI_TextDisabled", ImGUI_TextDisabled);
		luaState.set_function("ImGUI_CollapsingHeader", ImGUI_CollapsingHeader);
		luaState.set_function("ImGUI_IconToggle", ImGUI_IconToggle);
		luaState.set_function("ImGUI_Chip", ImGUI_Chip);
		luaState.set_function("ImGUI_Tabs", ImGUI_Tabs);
		luaState.set_function("IsProgramHidden", IsProgramHidden);
		luaState.set_function("RegisterKeybind", RegisterKeybind);
		luaState.set_function("DeleteKeybind", DeleteKeybind);
		luaState.set_function("DefineExtensionSetting", DefineExtensionSetting);
		luaState.set_function("GetSetting", GetExtensionSetting);
		luaState.set_function("LoadTexture", LoadTextureFromFileLua);
		luaState.set_function("GetItemIcon", GetItemIcon);
		luaState.set_function("GetItemPositions", GetItemPositions);
		luaState.set_function("GetItemTree", GetItemTree);
		luaState.set_function("GetItemAreas", GetItemAreas);
		luaState.set_function("GetPointGuide", GetPointGuide);
		luaState.set_function("GetWebImage", GetWebImage);
		luaState.set_function("CompareImages", CompareImages);
		luaState.set_function("GetMapView", GetMapView);
		luaState.set_function("FollowMapWhileMoving", FollowMapWhileMoving);
		luaState.set_function("GetPlayerPosition", GetPlayerPosition);
		luaState.set_function("GetMapGrid", GetMapGrid);
		luaState.set_function("GetMapTile", GetMapTile);
		luaState.set_function("GetMapFloor", GetMapFloor);
		luaState.set_function("DrawImage", DrawImage);
		luaState.set_function("DrawText", GDrawText);
		luaState.set_function("DrawCircle", DrawCircle);
		luaState.set_function("DrawRect", DrawRect);
		luaState.set_function("TextSize", TextSize);
		luaState.set_function("PushClipRect", PushClipRect);
		luaState.set_function("PopClipRect", PopClipRect);
		luaState.set_function("ClickArea", ClickArea);
		luaState.set_function("Net_Get", NetGet);
		luaState.set_function("Net_Post", NetPost);
		luaState.set_function("JSON_Encode", JSONEncode);
		luaState.set_function("JSON_Decode", JSONDecode);
		luaState.set_function("ReadFile", ReadFile);
		luaState.set_function("WriteFile", WriteFile);
		luaState.set_function("AppendFile", AppendFile);
		luaState.set_function("FileExists", FileExists);
		luaState.set_function("ListDirectory", ListDirectory);
		luaState.set_function("SetInterval", SetInterval);
		luaState.set_function("ClearInterval", ClearInterval);
		luaState.set_function("GetOSTimeMS", GetOSTimeMS);
		luaState.set_function("GetMousePosition", GetMousePosition);
		luaState.set_function("HasPermission", HasPermission);
		luaState.set_function("AskForPermission", AskForPermission);
		luaState.set_function("SaveFileDialog", SaveFileDialog);
		luaState.set_function("OpenFileDialog", OpenFileDialog);
		luaState.set_function("FileDialogResult", FileDialogResult);
		luaState.set_function("HoYoLAB_Account", HoyolabAccountInfo);
		luaState.set_function("HoYoLAB_LogIn", HoyolabLogIn);
		luaState.set_function("HoYoLAB_LogOut", HoyolabLogOut);
		luaState.set_function("HoYoLAB_ShowMap", HoyolabShowMap);
		luaState.set_function("HoYoLAB_FetchMarks", HoyolabFetchMarks);
		luaState.set_function("HoYoLAB_FetchCharacters", HoyolabFetchRoles);
		luaState.set_function("HoYoLAB_ChangeMarks", HoyolabChangeMarks);
		luaState.set_function("HoYoLAB_RestoreBackup", HoyolabRestoreBackup);
		luaState.set_function("HoYoLAB_Result", HoyolabResult);
		luaState.set_function("HoYoLAB_Backups", HoyolabBackups);
		luaState.set_function("GetOpenedPoints", GetOpenedPoints);
		luaState.set_function("GetGameTraffic", GetGameTraffic);
		luaState.set_function("AskForGameTraffic", AskForGameTraffic);
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

		auto websocket = luaState.new_usertype<Net::WebSocket>("WebSocket", sol::constructors<Net::WebSocket()>());
		websocket.set_function("connect", WebSocketConnect);
		websocket.set_function("send", WebSocketSend);
		websocket.set_function("poll", WebSocketPoll);
		websocket.set_function("pollAll", WebSocketPollAll);
		websocket.set_function("isConnected", WebSocketIsConnected);
		websocket.set_function("close", WebSocketClose);

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

	bool ImGUI_Checkbox(const std::string& label, bool currentValue)
	{
		bool value = currentValue;
		ImGui::Checkbox(label.c_str(), &value);
		return value;
	}

	float ImGUI_SliderFloat(const std::string& label, float currentValue, float minValue, float maxValue)
	{
		float value = currentValue;
		ImGui::SliderFloat(label.c_str(), &value, minValue, maxValue);
		return value;
	}

	int ImGUI_SliderInt(const std::string& label, int currentValue, int minValue, int maxValue)
	{
		int value = currentValue;
		ImGui::SliderInt(label.c_str(), &value, minValue, maxValue);
		return value;
	}
	std::string ImGUI_InputText(const std::string& label, const std::string& currentText)
	{
		std::string value = currentText;
		char buffer[4096]{};

		strncpy_s(buffer, sizeof(buffer), value.c_str(), _TRUNCATE);

		if (ImGui::InputText(label.c_str(), buffer, sizeof(buffer)))
			value = buffer;

		return value;
	}

	void ImGUI_SameLine()
	{
		ImGui::SameLine();
	}

	void ImGUI_Tooltip(const std::string& text)
	{
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", text.c_str());
	}

	void ImGUI_Separator()
	{
		ImGui::Separator();
	}

	void ImGUI_TextDisabled(const std::string& str)
	{
		ImGui::TextDisabled("%s", str.c_str());
	}

	// open tells if it starts open, after that the user decides. True while it's open.
	bool ImGUI_CollapsingHeader(const std::string& label, sol::optional<bool> open)
	{
		ImGui::SetNextItemOpen(open.value_or(false), ImGuiCond_Once);
		return ImGui::CollapsingHeader(label.c_str());
	}

	// Game colors for the tiles and chips below
	static const ImU32 TILE_OFF = IM_COL32(43, 48, 62, 255);
	static const ImU32 TILE_HOVER = IM_COL32(56, 62, 79, 255);
	static const ImU32 TILE_ON = IM_COL32(74, 68, 54, 255);
	static const ImU32 GOLD = IM_COL32(211, 188, 142, 255);
	static const ImU32 CREAM = IM_COL32(236, 229, 216, 255);
	static const ImU32 NAVY = IM_COL32(59, 66, 85, 255);
	static const ImU32 GREY = IM_COL32(150, 150, 150, 255);

	// Tiles and chips sit side by side and go to the next row when there's no room left.
	// One right after another counts as the same row, anything else in between starts a new one.
	static ImGuiID lastFlowItem = 0;

	static void flowPlace(float width)
	{
		if (lastFlowItem == 0 || ImGui::GetItemID() != lastFlowItem) return;

		// still at the start of the next line here, so this is the right edge
		float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		ImGui::SameLine();
		if (ImGui::GetCursorScreenPos().x + width > right)
			ImGui::NewLine();
	}

	// One type to pick: its icon in a badge, the name and how many there are. Lit up
	// while it's on. Gives back the new value.
	bool ImGUI_IconToggle(const std::string& id, GLuint texture, const std::string& name, int count, bool on)
	{
		const float width = 172.0f, height = 34.0f;
		flowPlace(width);

		ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImVec2 p1(p0.x + width, p0.y + height);
		ImGui::PushID(id.c_str());
		bool clicked = ImGui::InvisibleButton("##tile", ImVec2(width, height));
		ImGui::PopID();
		bool hovered = ImGui::IsItemHovered();
		lastFlowItem = ImGui::GetItemID();
		if (clicked) on = !on;

		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(p0, p1, on ? TILE_ON : hovered ? TILE_HOVER : TILE_OFF, 6.0f);
		if (on) draw->AddRect(p0, p1, GOLD, 6.0f, 0, 1.5f);

		// the same badge as on the map
		ImVec2 badge(p0.x + height / 2, p0.y + height / 2);
		draw->AddCircleFilled(badge, 13.0f, on ? CREAM : GREY);
		draw->AddCircleFilled(badge, 11.0f, NAVY);
		if (texture != 0)
			draw->AddImage((ImTextureID)(intptr_t)texture, ImVec2(badge.x - 8, badge.y - 8), ImVec2(badge.x + 8, badge.y + 8));

		std::string countText = std::to_string(count);
		ImVec2 countSize = ImGui::CalcTextSize(countText.c_str());
		float textY = p0.y + (height - countSize.y) / 2;
		float countX = p1.x - 8 - countSize.x;
		draw->AddText(ImVec2(countX, textY), on ? GOLD : GREY, countText.c_str());

		// long names are cut, the whole name shows on hover
		float nameX = p0.x + height + 2;
		ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
		bool cut = nameX + nameSize.x > countX - 6;
		draw->PushClipRect(ImVec2(nameX, p0.y), ImVec2(countX - 6, p1.y), true);
		draw->AddText(ImVec2(nameX, textY), on ? CREAM : IM_COL32(200, 200, 200, 255), name.c_str());
		draw->PopClipRect();

		if (hovered && cut)
			ImGui::SetTooltip("%s", name.c_str());

		return on;
	}

	// A round-ended chip that turns on and off, for things like regions. Gives back the new value.
	// Like other ImGui labels, anything after ## isn't shown, it only keeps chips apart.
	bool ImGUI_Chip(const std::string& label, bool on)
	{
		std::string text = label.substr(0, label.find("##"));
		ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
		float width = textSize.x + 24.0f, height = textSize.y + 10.0f;
		flowPlace(width);

		ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImVec2 p1(p0.x + width, p0.y + height);
		ImGui::PushID(label.c_str());
		bool clicked = ImGui::InvisibleButton("##chip", ImVec2(width, height));
		ImGui::PopID();
		bool hovered = ImGui::IsItemHovered();
		lastFlowItem = ImGui::GetItemID();
		if (clicked) on = !on;

		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(p0, p1, on ? GOLD : hovered ? TILE_HOVER : TILE_OFF, height / 2);
		draw->AddText(ImVec2(p0.x + 12, p0.y + 5), on ? NAVY : CREAM, text.c_str());

		return on;
	}

	// Tabs along the top, like ImGUI_Tabs("pages", { "Markers", "Sync" }).
	// Gives back which one is open, from 1. ImGui remembers it by id.
	int ImGUI_Tabs(const std::string& id, sol::table names)
	{
		int open = 1;
		if (!ImGui::BeginTabBar(id.c_str())) return open;
		for (size_t i = 1; i <= names.size(); i++) {
			std::string name = names.get_or(i, std::string());
			if (ImGui::BeginTabItem(name.c_str())) {
				open = (int)i;
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
		return open;
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
		sol::object defaultValue,
		sol::optional<float> min,
		sol::optional<float> max)
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

		if (min && max) {
			s.min = *min;
			s.max = *max;
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
		std::filesystem::path root = std::filesystem::weakly_canonical(
			Extensions::currentExtension->folder
		);

		std::filesystem::path filePath = Helpers::resolveRelativePath(Extensions::currentExtension->folder, path);

		std::string rootStr = root.string();
		std::string filePathStr = filePath.string();
		std::string rootWithSep = rootStr;
		if (!rootWithSep.empty() && rootWithSep.back() != std::filesystem::path::preferred_separator) {
			rootWithSep += std::filesystem::path::preferred_separator;
		}
		if (filePath != root &&
			filePathStr.find(rootWithSep) != 0)
		{
			if (Extensions::currentExtension->extensionPermissions.find(Extensions::ExtensionPermissionTypes::ExternalReadfile) ==
				Extensions::currentExtension->extensionPermissions.end())
			{
				throw std::runtime_error("Extension does not have permission to read external files");
			}
		}
		int width = 0, height = 0;
		GLuint tex = Helpers::loadTextureFromFile(filePath.string(), width, height);
		return { tex, width, height };
	}

	sol::object NetGet(const std::string& url, sol::this_state state, sol::optional<bool> hoyolab)
	{
		CHECK_PERMISSIONS();

		sol::state_view lua(state);
		Net::Response response;

		if (!Net::get(url, response, hoyolab.value_or(false)))
			return sol::nil;

		sol::table result = lua.create_table();
		result["status"] = response.status;
		result["body"] = response.body;

		return result;
	}

	sol::object NetPost(const std::string& url, const std::string& data, sol::this_state state, sol::optional<bool> hoyolab)
	{
		CHECK_PERMISSIONS();
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
	// position at centerX, centerY. moving is true while the map moves, and
	// after it stops until it's lined up exactly. panelLeft is where a panel
	// on the right starts (the region list, a point's info), nil with none
	// open. ui is the game's own UI showing now, a list of {left, top, right,
	// bottom}, the open panel too.
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
		sol::table ui = lua.create_table();
		for (const RECT& box : GameUi::Boxes(view.gameRect, view.panelLeft)) {
			ui.add(lua.create_table_with(
				"left", (float)(box.left - windowX),
				"top", (float)(box.top - windowY),
				"right", (float)(box.right - windowX),
				"bottom", (float)(box.bottom - windowY)));
		}
		sol::table result = lua.create_table_with(
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
			"bottom", bottom,
			"moving", view.moving,
			"ui", ui
		);
		if (view.panelLeft >= 0) result["panelLeft"] = (float)(view.panelLeft - windowX);
		return sol::make_object(state, result);
	}

	// Call every Update if you draw while the map moves. Otherwise GetMapView
	// gives nil while it moves: it's only followed loosely then, which is
	// cheaper, and found exactly when it stops.
	void FollowMapWhileMoving() {
		MapTracking::FollowWhileMoving();
	}

	static const char* stateName(MiniMap::State state) {
		switch (state) {
		case MiniMap::State::Found: return "found";
		case MiniMap::State::Near: return "near";
		case MiniMap::State::Searching: return "searching";
		case MiniMap::State::Nowhere: return "nowhere";
		case MiniMap::State::Domain: return "domain";
		default: return "none";
		}
	}

	// Where the player is on the map, read from the minimap. Always a table:
	// found says if the spot is where the player is right now. If not, state
	// says why ("none", "near", "searching", "nowhere", "domain", see MiniMap::State),
	// the spot is where they were last seen and age how many seconds ago
	// that was. mapId is 0 until they were seen once. The minimap is only
	// watched while this gets called, so the first calls aren't found.
	sol::object GetPlayerPosition(sol::this_state state) {
		MiniMap::Spot spot = MiniMap::GetPlayer();

		int windowX = 0, windowY = 0;
		glfwGetWindowPos(Overlay::Window, &windowX, &windowY);
		float left = (float)(spot.square.left - windowX);
		float top = (float)(spot.square.top - windowY);
		float right = (float)(spot.square.right - windowX);
		float bottom = (float)(spot.square.bottom - windowY);

		// Left to bottom is the square around the game's minimap on the overlay.
		// The minimap is the circle that just fits in it.
		sol::state_view lua(state);
		return sol::make_object(state, lua.create_table_with(
			"found", spot.found,
			"state", stateName(spot.state),
			"age", spot.age,
			"mapId", spot.mapId,
			"groupId", spot.groupId,
			"floorId", spot.floorId,
			"lat", spot.lat,
			"lng", spot.lng,
			"unitsPerPixel", MiniMap::UNITS_ACROSS / spot.zoom / std::max(1.0f, right - left),
			"left", left,
			"top", top,
			"right", right,
			"bottom", bottom,
			"centerX", (left + right) / 2.0f,
			"centerY", (top + bottom) / 2.0f,
			"radius", (right - left) / 2.0f
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
		CHECK_PERMISSIONS();
		if (textureID == 0) return;
		int a = (int)(alpha.value_or(1.0f) * 255.0f);
		ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)textureID, ImVec2(x0, y0), ImVec2(x1, y1),
			ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
	}

	// color is 0xRRGGBBAA like DrawText. Filled, or only the outline when a thickness is given.
	void DrawCircle(float x, float y, float radius, uint32_t color, sol::optional<float> thickness) {
		CHECK_PERMISSIONS();
		ImU32 col = IM_COL32((color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
		if (thickness) ImGui::GetBackgroundDrawList()->AddCircle(ImVec2(x, y), radius, col, 0, *thickness);
		else ImGui::GetBackgroundDrawList()->AddCircleFilled(ImVec2(x, y), radius, col);
	}

	// how wide and tall DrawText would draw this text
	std::tuple<float, float> TextSize(const std::string& text, sol::optional<float> size, sol::optional<float> wrapWidth) {
		ImVec2 measured = ImGui::GetFont()->CalcTextSizeA(size.value_or(ImGui::GetFontSize()), FLT_MAX, wrapWidth.value_or(0.0f), text.c_str());
		return { measured.x, measured.y };
	}

	// color is 0xRRGGBBAA. Filled, or only the outline when a thickness is given.
	void DrawRect(float x0, float y0, float x1, float y1, uint32_t color, sol::optional<float> rounding, sol::optional<float> thickness) {
		CHECK_PERMISSIONS();
		ImU32 col = IM_COL32((color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
		float round = rounding.value_or(0.0f);
		if (thickness) ImGui::GetBackgroundDrawList()->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col, round, 0, *thickness);
		else ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, round);
	}

	void PushClipRect(float x0, float y0, float x1, float y1) {
		ImGui::GetBackgroundDrawList()->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
	}

	void PopClipRect() {
		ImGui::GetBackgroundDrawList()->PopClipRect();
	}

	// Call every Update for something that can be clicked, like a marker. From
	// the next frame the overlay takes the mouse while it's over the area, so the
	// click lands here and not in the game. Gives back clicked (only on the frame
	// it's clicked) and hovered.
	std::tuple<bool, bool> ClickArea(float x0, float y0, float x1, float y1) {
		CHECK_PERMISSIONS();
		Overlay::ClaimMouse(x0, y0, x1, y1);
		// the menu is on top, the mouse is for it
		if (Overlay::IO->WantCaptureMouse) return { false, false };
		bool hovered = ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y1), false);
		return { hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left), hovered };
	}

	static nlohmann::json LuaToJson(sol::object obj) {
		switch (obj.get_type()) {
		case sol::type::nil:
			return nullptr;

		case sol::type::boolean:
			return obj.as<bool>();

		case sol::type::number: {
			// whole numbers stay whole, ids came out as 17.0 otherwise
			double value = obj.as<double>();
			if (value == std::floor(value) && std::abs(value) < 9007199254740992.0)
				return static_cast<int64_t>(value);
			return value;
		}

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

	bool checkPermissions(const std::string& functionName) {
		if (!Extensions::currentExtension)
			return false;

		auto it = functionPermissions.find(functionName);

		if (it == functionPermissions.end()) {
			return false;
		}

		for (auto permission : it->second) {

			if (Extensions::currentExtension->extensionPermissions.find(permission) ==
				Extensions::currentExtension->extensionPermissions.end())
			{
				return false;
			}
		}

		return true;
	}

	std::string ReadFile(const std::string& path) {
		CHECK_PERMISSIONS();

		std::filesystem::path root = std::filesystem::weakly_canonical(
			Extensions::currentExtension->folder
		);

		std::filesystem::path filePath = Helpers::resolveRelativePath(Extensions::currentExtension->folder, path);

		std::string rootStr = root.string();
		std::string filePathStr = filePath.string();
		std::string rootWithSep = rootStr;
		if (!rootWithSep.empty() && rootWithSep.back() != std::filesystem::path::preferred_separator) {
			rootWithSep += std::filesystem::path::preferred_separator;
		}
		if (filePath != root &&
			filePathStr.find(rootWithSep) != 0)
		{
			if (Extensions::currentExtension->extensionPermissions.find(Extensions::ExtensionPermissionTypes::ExternalReadfile) ==
				Extensions::currentExtension->extensionPermissions.end())
			{
				throw std::runtime_error("Extension does not have permission to read external files");
			}
		}

		std::ifstream file(filePath, std::ios::binary);

		if (!file)
			throw std::runtime_error("Failed to open file");

		return std::string(
			std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>()
		);
	}

	bool FileExists(const std::string& path) {
		CHECK_PERMISSIONS();

		std::filesystem::path root = std::filesystem::weakly_canonical(
			Extensions::currentExtension->folder
		);

		std::filesystem::path filePath = Helpers::resolveRelativePath(Extensions::currentExtension->folder, path);

		std::string rootStr = root.string();
		std::string filePathStr = filePath.string();
		std::string rootWithSep = rootStr;
		if (!rootWithSep.empty() && rootWithSep.back() != std::filesystem::path::preferred_separator) {
			rootWithSep += std::filesystem::path::preferred_separator;
		}
		if (filePath != root &&
			filePathStr.find(rootWithSep) != 0)
		{
			if (Extensions::currentExtension->extensionPermissions.find(Extensions::ExtensionPermissionTypes::ExternalReadfile) ==
				Extensions::currentExtension->extensionPermissions.end())
			{
				throw std::runtime_error("Extension does not have permission to read external files");
			}
		}
		return std::filesystem::exists(filePath) && std::filesystem::is_regular_file(filePath);
	}

	sol::table ListDirectory(const std::string& path, sol::this_state state)
	{
		CHECK_PERMISSIONS();

		sol::state_view lua(state);
		sol::table result = lua.create_table();

		std::filesystem::path root = std::filesystem::weakly_canonical(
			Extensions::currentExtension->folder
		);

		std::filesystem::path directoryPath = Helpers::resolveRelativePath(
			Extensions::currentExtension->folder,
			path
		);

		std::string rootStr = root.string();
		std::string directoryPathStr = directoryPath.string();
		std::string rootWithSep = rootStr;

		if (!rootWithSep.empty() &&
			rootWithSep.back() != std::filesystem::path::preferred_separator)
		{
			rootWithSep += std::filesystem::path::preferred_separator;
		}

		if (directoryPath != root &&
			directoryPathStr.find(rootWithSep) != 0)
		{
			if (Extensions::currentExtension->extensionPermissions.find(
				Extensions::ExtensionPermissionTypes::ExternalReadfile) ==
				Extensions::currentExtension->extensionPermissions.end())
			{
				throw std::runtime_error("Extension does not have permission to read external files");
			}
		}

		if (!std::filesystem::exists(directoryPath) ||
			!std::filesystem::is_directory(directoryPath))
		{
			return result;
		}

		int index = 1;

		for (const auto& entry : std::filesystem::directory_iterator(directoryPath))
		{
			result[index++] = entry.path().filename().string();
		}

		return result;
	}

	static void writeExtensionData(std::ofstream& file, const std::string& data) {
		auto* ext = Extensions::currentExtension;

		if (!ext)
			throw std::runtime_error("No current extension");

		if (data.size() > Extensions::ExtensionWriteLimit - ext->bytesToDiskWritten)
			throw std::runtime_error("Extension disk write limit exceeded");

		file.write(data.data(), static_cast<std::streamsize>(data.size()));

		if (!file)
			throw std::runtime_error("Failed to write file");

		ext->bytesToDiskWritten += static_cast<int>(data.size());
	}

	bool WriteFile(const std::string& path, const std::string& data) {
		CHECK_PERMISSIONS();

		std::filesystem::path root = std::filesystem::weakly_canonical(
			Extensions::currentExtension->folder
		);

		std::filesystem::path filePath = Helpers::resolveRelativePath(Extensions::currentExtension->folder, path);

		std::string rootStr = root.string();
		std::string filePathStr = filePath.string();
		std::string rootWithSep = rootStr;

		if (!rootWithSep.empty() && rootWithSep.back() != std::filesystem::path::preferred_separator)
			rootWithSep += std::filesystem::path::preferred_separator;

		if (filePath != root && filePathStr.find(rootWithSep) != 0) {
			if (Extensions::currentExtension->extensionPermissions.find(Extensions::ExtensionPermissionTypes::ExternalWritefile) ==
				Extensions::currentExtension->extensionPermissions.end()) {
				throw std::runtime_error("Extension does not have permission to write external files");
			}
		}

		std::ofstream file(filePath, std::ios::binary | std::ios::trunc);

		if (!file)
			throw std::runtime_error("Failed to open file");

		writeExtensionData(file, data);

		return true;
	}

	bool AppendFile(const std::string& path, const std::string& data) {
		CHECK_PERMISSIONS();

		std::filesystem::path root = std::filesystem::weakly_canonical(
			Extensions::currentExtension->folder
		);

		std::filesystem::path filePath = Helpers::resolveRelativePath(Extensions::currentExtension->folder, path);

		std::string rootStr = root.string();
		std::string filePathStr = filePath.string();
		std::string rootWithSep = rootStr;

		if (!rootWithSep.empty() && rootWithSep.back() != std::filesystem::path::preferred_separator)
			rootWithSep += std::filesystem::path::preferred_separator;

		if (filePath != root && filePathStr.find(rootWithSep) != 0) {
			if (Extensions::currentExtension->extensionPermissions.find(Extensions::ExtensionPermissionTypes::ExternalWritefile) ==
				Extensions::currentExtension->extensionPermissions.end()) {
				throw std::runtime_error("Extension does not have permission to write external files");
			}
		}

		std::ofstream file(filePath, std::ios::binary | std::ios::app);

		if (!file)
			throw std::runtime_error("Failed to open file");

		writeExtensionData(file, data);

		return true;
	}

	bool WebSocketConnect(Net::WebSocket& socket, const std::string& url, sol::optional<bool> hoyolab)
	{
		if (!checkPermissions("LuaFunctions::WebSocketConnect"))
			throw std::runtime_error("Extension does not have permission");

		return socket.connect(url, hoyolab.value_or(false));
	}

	bool WebSocketSend(Net::WebSocket& socket, const std::string& message)
	{
		if (!checkPermissions("LuaFunctions::WebSocketSend"))
			throw std::runtime_error("Extension does not have permission");

		return socket.send(message);
	}

	sol::object WebSocketPoll(Net::WebSocket& socket, sol::this_state state)
	{
		std::string message;

		if (!socket.poll(message))
			return sol::nil;

		return sol::make_object(state, message);
	}

	sol::object WebSocketPollAll(Net::WebSocket& socket, sol::this_state state)
	{
		sol::state_view lua(state);
		sol::table result = lua.create_table();

		auto messages = socket.pollAll();

		int index = 1;
		for (const auto& message : messages)
			result[index++] = message;

		return result;
	}

	bool WebSocketIsConnected(Net::WebSocket& socket)
	{
		return socket.isConnected();
	}

	void WebSocketClose(Net::WebSocket& socket)
	{
		socket.close();
	}

	double CompareImages(GLuint textureID1, GLuint textureID2, int accuracy) {
		return Helpers::compareTextures(textureID1, textureID2, accuracy);
	}

	double GetOSTimeMS() {
		return std::chrono::duration<double, std::milli>(
			std::chrono::system_clock::now().time_since_epoch()
		).count();
	}
	int SetInterval(sol::protected_function callback, int milliseconds)
	{
		if (!callback.valid() || milliseconds <= 0)
			return 0;

		LuaInterval interval;
		interval.id = nextIntervalId++;
		interval.milliseconds = milliseconds;
		interval.nextExecution = GetOSTimeMS() + milliseconds;
		interval.owner = Extensions::currentExtension;
		interval.callback = std::move(callback);

		intervals.push_back(std::move(interval));

		return intervals.back().id;
	}
	void ClearInterval(int id)
	{
		auto it = std::find_if(
			intervals.begin(),
			intervals.end(),
			[id](const LuaInterval& interval) {
				return interval.id == id;
			}
		);

		if (it == intervals.end())
			return;

		if (it->owner != Extensions::currentExtension)
			return;

		intervals.erase(it);
	}
	void UpdateIntervals()
	{
		double now = GetOSTimeMS();

		std::vector<int> dueIntervals;

		for (const auto& interval : intervals)
		{
			if (now >= interval.nextExecution)
				dueIntervals.push_back(interval.id);
		}

		for (int id : dueIntervals)
		{
			auto it = std::find_if(
				intervals.begin(),
				intervals.end(),
				[id](const LuaInterval& interval) {
					return interval.id == id;
				}
			);

			if (it == intervals.end())
				continue;

			it->nextExecution = now + it->milliseconds;

			Extensions::Extension* owner = it->owner;
			sol::protected_function callback = it->callback;

			Extensions::runForExtension(owner, [&callback]() {
				callLua(callback);
				});
		}
	}
	std::tuple<int, int> GetMousePosition()
	{
		POINT point;
		if (!GetCursorPos(&point))
			return { 0, 0 };

		return { point.x, point.y };
	}
	// with a wrap width, long text breaks into lines that fit it
	void GDrawText(const std::string& text, float x, float y, sol::optional<float> size, sol::optional<uint32_t> color, sol::optional<float> wrapWidth)
	{
		CHECK_PERMISSIONS();
		if (text.empty())
			return;

		float fontSize = size.value_or(ImGui::GetFontSize());
		uint32_t rgba = color.value_or(0xFFFFFFFF);

		int r = (rgba >> 24) & 0xFF;
		int g = (rgba >> 16) & 0xFF;
		int b = (rgba >> 8) & 0xFF;
		int a = rgba & 0xFF;

		ImGui::GetBackgroundDrawList()->AddText(
			ImGui::GetFont(),
			fontSize,
			ImVec2(x, y),
			IM_COL32(r, g, b, a),
			text.c_str(),
			nullptr,
			wrapWidth.value_or(0.0f)
		);
	}
	GLuint GetItemIcon(int itemId)
	{
		int width = 0, height = 0;
		return Helpers::loadTextureFromFile(Helpers::exeDirectory().string() + "/items/icons/" + std::to_string(itemId) + ".png", width, height);
	}

	static sol::object readItemFile(const std::filesystem::path& relative, sol::this_state state)
	{
		std::ifstream file(Helpers::exeDirectory() / "items" / relative, std::ios::binary);
		if (!file)
			return sol::lua_nil;

		nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
		if (data.is_discarded())
			return sol::lua_nil;

		return JsonToLua(data, sol::state_view(state));
	}

	// every point of one type, on every map, each with its map_id
	sol::object GetItemPositions(int itemId, sol::this_state state)
	{
		return readItemFile(std::filesystem::path("positions") / (std::to_string(itemId) + ".json"), state);
	}

	// HoYoLAB's groups of a map, each with its types (id, name, icon) as children
	sol::object GetItemTree(int mapId, sol::this_state state)
	{
		return readItemFile(std::filesystem::path("maps") / std::to_string(mapId) / "tree.json", state);
	}

	// HoYoLAB's regions: id, name, map_id and the box they cover (l_x, l_y, r_x, r_y).
	// It sends the regions of every map with each one, so check map_id.
	sol::object GetItemAreas(int mapId, sol::this_state state)
	{
		return readItemFile(std::filesystem::path("maps") / std::to_string(mapId) / "areas.json", state);
	}

	// HoYoLAB's guide for a point and the comments from appsample, nil while it loads
	sol::object GetPointGuide(int mapId, int64_t pointId, int typeId, sol::this_state state)
	{
		CHECK_PERMISSIONS();
		PointGuides::Guide guide;
		if (!PointGuides::Get(mapId, pointId, typeId, guide)) return sol::lua_nil;

		sol::state_view lua(state);
		sol::table result = lua.create_table();
		result["text"] = guide.text;
		result["picture"] = guide.picture;
		result["failed"] = guide.failed;
		sol::table comments = lua.create_table();
		for (size_t i = 0; i < guide.comments.size(); i++) {
			const PointGuides::Comment& comment = guide.comments[i];
			comments[i + 1] = lua.create_table_with(
				"author", comment.author,
				"date", comment.date,
				"text", comment.text,
				"picture", comment.picture,
				"likes", comment.likes);
		}
		result["comments"] = comments;
		return result;
	}

	// a picture from the web as a texture with its size, 0 until it's loaded
	std::tuple<GLuint, int, int> GetWebImage(const std::string& url)
	{
		CHECK_PERMISSIONS();
		WebImages::Image image = WebImages::Get(url);
		return { image.texture, image.width, image.height };
	}
	bool HasPermission(const std::string& name)
	{
		auto* ext = Extensions::currentExtension;
		Extensions::ExtensionPermissionTypes permission = Extensions::permissionNamed(name);
		return ext && permission != Extensions::ExtensionPermissionTypes::None && ext->extensionPermissions.count(permission) > 0;
	}

	// asks the user in a window of its own, HasPermission tells once they said yes
	void AskForPermission(const std::string& name)
	{
		Extensions::askForPermission(Extensions::currentExtension, Extensions::permissionNamed(name));
	}

	// Windows' save dialog, the data goes to the file the user picks. Gives back
	// an id for FileDialogResult, nil while another dialog is open.
	sol::object SaveFileDialog(const std::string& suggestedName, const std::string& data, sol::this_state state)
	{
		int dialog = FileDialogs::Save(glfwGetWin32Window(Overlay::Window), suggestedName, data);
		if (dialog == 0) return sol::lua_nil;
		return sol::make_object(state, dialog);
	}

	sol::object OpenFileDialog(sol::this_state state)
	{
		int dialog = FileDialogs::Open(glfwGetWin32Window(Overlay::Window));
		if (dialog == 0) return sol::lua_nil;
		return sol::make_object(state, dialog);
	}

	// nil while the dialog is open, then { picked, ok, name, data } once
	sol::object FileDialogResult(int dialog, sol::this_state state)
	{
		FileDialogs::Result result;
		if (!FileDialogs::GetResult(dialog, result)) return sol::lua_nil;
		sol::state_view lua(state);
		return lua.create_table_with(
			"picked", result.picked,
			"ok", result.ok,
			"name", result.name,
			"data", result.data);
	}

	// HoYoLAB: the login stays in C++, extensions get what's marked and can change it

	sol::table HoyolabAccountInfo(sol::this_state state)
	{
		CHECK_PERMISSIONS();
		sol::state_view lua(state);
		return lua.create_table_with(
			"loggedIn", HoyolabAccount::LoggedIn(),
			"account", HoyolabAccount::Account(),
			"windowOpen", HoyolabAccount::WindowOpen(),
			"error", HoyolabAccount::LastError(),
			"busy", HoyolabMarks::Busy());
	}

	void HoyolabLogIn()
	{
		CHECK_PERMISSIONS();
		HoyolabAccount::LogIn();
	}

	void HoyolabLogOut()
	{
		CHECK_PERMISSIONS();
		HoyolabAccount::LogOut();
	}

	void HoyolabShowMap()
	{
		CHECK_PERMISSIONS();
		HoyolabAccount::ShowMap();
	}

	// point ids from a Lua list, 17.0 counts as 17
	static std::vector<int64_t> pointIds(const sol::table& list)
	{
		std::vector<int64_t> ids;
		for (size_t i = 1; i <= list.size(); i++) {
			sol::object value = list[i];
			if (value.get_type() != sol::type::number) continue;
			double number = value.as<double>();
			if (number == (double)(int64_t)number) ids.push_back((int64_t)number);
		}
		return ids;
	}

	// with backup true the marks are saved to a backup on the way, its name comes with the result
	int HoyolabFetchMarks(sol::optional<bool> backup)
	{
		CHECK_PERMISSIONS();
		return HoyolabMarks::Fetch(backup.value_or(false));
	}

	// the Genshin characters on the account, one per server
	int HoyolabFetchRoles()
	{
		CHECK_PERMISSIONS();
		return HoyolabMarks::Roles();
	}

	// marks the points in add and unmarks the ones in remove, unmarking asks the user first
	int HoyolabChangeMarks(sol::table add, sol::table remove)
	{
		CHECK_PERMISSIONS();
		return HoyolabMarks::Change(pointIds(add), pointIds(remove));
	}

	int HoyolabRestoreBackup(const std::string& name)
	{
		CHECK_PERMISSIONS();
		return HoyolabMarks::Restore(name);
	}

	static sol::table idList(sol::state_view& lua, const std::vector<int64_t>& ids)
	{
		sol::table list = lua.create_table((int)ids.size(), 0);
		for (size_t i = 0; i < ids.size(); i++) list[i + 1] = ids[i];
		return list;
	}

	// nil while it runs, then { ok, error, captcha, marks, added, removed, kept, unknown, backup, characters } once
	sol::object HoyolabResult(int job, sol::this_state state)
	{
		CHECK_PERMISSIONS();
		HoyolabMarks::Result result;
		if (!HoyolabMarks::GetResult(job, result)) return sol::lua_nil;
		sol::state_view lua(state);
		sol::table table = lua.create_table();
		table["ok"] = result.ok;
		table["error"] = result.error;
		table["captcha"] = result.captcha;
		table["marks"] = idList(lua, result.marks);
		table["added"] = result.added;
		table["removed"] = result.removed;
		table["kept"] = idList(lua, result.kept);
		table["unknown"] = idList(lua, result.unknown);
		table["backup"] = result.backup;
		sol::table roles = lua.create_table();
		for (size_t i = 0; i < result.roles.size(); i++) {
			const HoyolabMarks::Role& role = result.roles[i];
			roles[i + 1] = lua.create_table_with("uid", role.uid, "nickname", role.nickname, "server", role.server, "level", role.level);
		}
		table["characters"] = roles;
		return table;
	}

	// { name, count }, newest first
	sol::table HoyolabBackups(sol::this_state state)
	{
		CHECK_PERMISSIONS();
		sol::state_view lua(state);
		sol::table list = lua.create_table();
		std::vector<HoyolabMarks::Backup> backups = HoyolabMarks::Backups();
		for (size_t i = 0; i < backups.size(); i++) {
			list[i + 1] = lua.create_table_with("name", backups[i].name, "count", backups[i].count);
		}
		return list;
	}

	// the HoYoLAB points of the chests and oculi the game says were opened or picked up since this
	// extension last asked. The first time it starts from now.
	sol::table GetOpenedPoints(sol::this_state state)
	{
		CHECK_PERMISSIONS();
		static std::unordered_map<std::string, size_t> asked;
		std::string key = Extensions::currentExtension->folder.string();
		std::vector<int64_t> points;
		auto it = asked.find(key);
		size_t count = GameTraffic::OpenedPoints(it == asked.end() ? SIZE_MAX : it->second, points);
		asked[key] = count;
		sol::state_view lua(state);
		sol::table list = lua.create_table();
		for (size_t i = 0; i < points.size(); i++) list[i + 1] = points[i];
		return list;
	}

	// Reading the game's traffic, for what needs it: on is the switch in the
	// menu, state one of off, cant_listen, waiting, searching, reading,
	// cant_read, and text what the menu says about it.
	sol::table GetGameTraffic(sol::this_state state)
	{
		CHECK_PERMISSIONS();
		static const char* names[] = { "off", "cant_listen", "waiting", "searching", "reading", "cant_read" };
		GameTraffic::Status status = GameTraffic::GetStatus();
		sol::state_view lua(state);
		return lua.create_table_with("on", Settings::readGameTraffic, "state", names[int(status.state)], "text", status.text);
	}

	// asks the player in a window of its own, the extension can't turn it on
	void AskForGameTraffic()
	{
		Extensions::askForGameTraffic(Extensions::currentExtension);
	}
}
