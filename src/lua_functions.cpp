#include <windows.h>
#include <nlohmann/json.hpp>
#include "lua_functions.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "overlay.hpp"
#include "map_tiles.hpp"
#include "map_tracking.hpp"
#include "capture.hpp"
#include "minimap.hpp"
#include "item_icons.hpp"
#include "item_catalog.hpp"
#include "item_files.hpp"
#include "typing.hpp"
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

namespace LuaFunctions {
	using Permission = Extensions::ExtensionPermissionTypes;

	std::vector<LuaInterval> intervals;
	static int nextIntervalId = 1;

	// what is the kind of callback, for the log: "button", "key", "interval"
	static void callLua(const sol::protected_function& function, const char* what) {
		auto result = function();
		if (!result.valid()) {
			sol::error err = result;
			Extensions::Extension* ext = Extensions::currentExtension;
			std::cerr << "[" << (ext ? ext->name : std::string("no extension")) << "] " << what << ": " << err.what() << std::endl;
		}
	}

	// what a C++ function gave, as Lua values (a tuple gives one each)
	template <typename T>
	static void push(sol::variadic_results& results, sol::this_state state, T&& value) {
		using V = std::decay_t<T>;
		if constexpr (sol::meta::is_tuple<V>::value) {
			std::apply([&](auto&&... each) { (push(results, state, each), ...); }, value);
		}
		else if constexpr (std::is_same_v<V, sol::variadic_results>) {
			for (auto& each : value) results.push_back(each);
		}
		else if constexpr (sol::meta::is_optional<V>::value) {
			if (value) push(results, state, *value);
			else results.push_back(sol::make_object(state, sol::lua_nil));
		}
		else {
			results.push_back(sol::make_object(state, std::forward<T>(value)));
		}
	}

	static std::vector<Bound>& bound() {
		static std::vector<Bound> list;
		return list;
	}

	const std::vector<Bound>& boundFunctions() {
		return bound();
	}

	// fn wrapped so the permission is checked before every call
	template <typename R, typename... Args>
	static auto guarded(std::string name, Permission permission, R(*fn)(Args...)) {
		return [name, permission, fn](sol::this_state state, Args... args) -> sol::variadic_results {
			sol::variadic_results results;
			if (!Extensions::allowed(name.c_str(), permission)) {
				if constexpr (std::is_same_v<R, bool>) results.push_back(sol::make_object(state, false));
				else if constexpr (!std::is_void_v<R>) results.push_back(sol::make_object(state, sol::lua_nil));
				return results;
			}
			if constexpr (std::is_void_v<R>) fn(std::forward<Args>(args)...);
			else push(results, state, fn(std::forward<Args>(args)...));
			return results;
		};
	}

	// the one way a function reaches Lua: into target (the globals or a usertype)
	// under name, and into the registry table the tests check the globals against
	template <typename Target, typename R, typename... Args>
	static void bindFunction(sol::state& lua, Target& target, const std::string& prefix, const char* name, Permission permission, R(*fn)(Args...)) {
		std::string fullName = prefix + name;
		// made once, so the copies sol keeps for a usertype are the same function
		sol::table maker = lua.create_table();
		maker.set_function("function", guarded(fullName, permission, fn));
		sol::object function = maker["function"];
		target.set(name, function);

		sol::table registry = lua.registry();
		sol::object list = registry[BOUND_TABLE];
		if (!list.is<sol::table>()) registry[BOUND_TABLE] = lua.create_table();
		sol::table table = registry[BOUND_TABLE];
		table[fullName] = function;

		auto& all = bound();
		bool known = std::any_of(all.begin(), all.end(), [&](const Bound& b) { return b.name == fullName; });
		if (!known) all.push_back({ fullName, permission });
	}

	// everything an extension can call, and what each one needs
	void registerLuaFunctions(sol::state& luaState) {
		auto global = [&](const char* name, Permission permission, auto fn) {
			bindFunction(luaState, luaState, "", name, permission, fn);
		};
		const Permission None = Permission::None;
		const Permission DisplayOverlay = Permission::DisplayOverlay;
		const Permission DisplayRead = Permission::DisplayRead;
		const Permission Keybinds = Permission::Keybinds;
		const Permission InternalRead = Permission::InternalReadfile;
		const Permission InternalWrite = Permission::InternalWritefile;
		const Permission Network = Permission::NetworkAccess;

		// the extension's own page in the menu
		global("ImGUI_Text", None, ImGUI_Text);
		global("ImGUI_Button", None, ImGUI_Button);
		global("ImGUI_Image", None, ImGUI_Image);
		global("ImGUI_Checkbox", None, ImGUI_Checkbox);
		global("ImGUI_SliderFloat", None, ImGUI_SliderFloat);
		global("ImGUI_SliderInt", None, ImGUI_SliderInt);
		global("ImGUI_Number", None, ImGUI_Number);
		global("ImGUI_Toggle", None, ImGUI_Toggle);
		global("ImGUI_InputText", None, ImGUI_InputText);
		global("ImGUI_SameLine", None, ImGUI_SameLine);
		global("ImGUI_Tooltip", None, ImGUI_Tooltip);
		global("ImGUI_Waiting", None, ImGUI_Waiting);
		global("ImGUI_Empty", None, ImGUI_Empty);
		global("ImGUI_Progress", None, ImGUI_Progress);
		global("ImGUI_Separator", None, ImGUI_Separator);
		global("IsProgramHidden", None, IsProgramHidden);
		global("DefineExtensionSetting", None, DefineExtensionSetting);
		global("GetSetting", None, GetExtensionSetting);
		global("JSON_Encode", None, JSONEncode);
		global("JSON_Decode", None, JSONDecode);
		global("SetInterval", None, SetInterval);
		global("ClearInterval", None, ClearInterval);
		global("GetOSTimeMS", None, GetOSTimeMS);

		global("RegisterKeybind", Keybinds, RegisterKeybind);
		global("DeleteKeybind", Keybinds, DeleteKeybind);

		// drawing on the overlay
		global("DrawImage", DisplayOverlay, DrawImage);
		global("DrawText", DisplayOverlay, GDrawText);
		global("GetTheme", None, GetTheme);
		global("PushClipRect", DisplayOverlay, PushClipRect);
		global("PopClipRect", DisplayOverlay, PopClipRect);

		// what's on the screen and the map
		global("GetMapView", DisplayRead, GetMapView);
		global("FollowMapWhileMoving", DisplayRead, FollowMapWhileMoving);
		global("GetPlayerPosition", DisplayRead, GetPlayerPosition);
		global("GetMapGrid", DisplayRead, GetMapGrid);
		global("GetMapTile", DisplayRead, GetMapTile);
		global("GetMapFloor", DisplayRead, GetMapFloor);
		global("GetMousePosition", DisplayRead, GetMousePosition);
		global("CompareImages", DisplayRead, CompareImages);
		global("GetItemsVersion", DisplayRead, GetItemsVersion);
		global("GetItemsState", DisplayRead, GetItemsState);
		global("GetItemTypes", DisplayRead, GetItemTypes);
		global("GetItemPositions", DisplayRead, GetItemPositions);
		global("GetItemRegions", DisplayRead, GetItemRegions);
		global("GetItemIcon", DisplayRead, GetItemIcon);

		// files, only in the extension's own folder (pathFor)
		global("LoadTexture", InternalRead, LoadTextureFromFileLua);
		global("ReadFile", InternalRead, ReadFile);
		global("FileExists", InternalRead, FileExists);
		global("ListDirectory", InternalRead, ListDirectory);
		global("WriteFile", InternalWrite, WriteFile);
		global("AppendFile", InternalWrite, AppendFile);

		global("Net_Get", Network, NetGet);
		global("Net_Post", Network, NetPost);

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
		auto method = [&](const char* name, Permission permission, auto fn) {
			bindFunction(luaState, websocket, "WebSocket.", name, permission, fn);
		};
		method("connect", Network, WebSocketConnect);
		method("send", Network, WebSocketSend);
		method("poll", Network, WebSocketPoll);
		method("pollAll", Network, WebSocketPollAll);
		method("isConnected", Network, WebSocketIsConnected);
		// closing only ends what it could do, so it works without the permission too
		method("close", None, WebSocketClose);

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

	static bool inMenu(const char* function) {
		return Extensions::inPhase(function, Extensions::Phase::Menu);
	}

	static bool inFrame(const char* function) {
		return Extensions::inPhase(function, Extensions::Phase::Update);
	}

	void ImGUI_Text(const std::string& str) {
		if (!inMenu("ImGUI_Text")) return;
		ImGui::Text("%s", str.c_str());
	}

	// ImGUI_Button(label, onPress, options), options is optional:
	// { look = "main" or "risky", busy = true or the text to show while busy }
	void ImGUI_Button(const std::string& label, sol::protected_function callback, sol::optional<sol::table> table) {
		if (!inMenu("ImGUI_Button")) return;
		Controls::ButtonOptions options;
		if (table) {
			std::string look = table->get_or<std::string>("look", "");
			if (look == "main") options.look = Controls::ButtonOptions::Look::Main;
			else if (look == "risky") options.look = Controls::ButtonOptions::Look::Risky;
			sol::object busy = (*table)["busy"];
			if (busy.is<std::string>()) {
				options.busy = true;
				options.busyLabel = busy.as<std::string>();
			}
			else if (busy.is<bool>()) options.busy = busy.as<bool>();
		}
		if (Controls::Button(label.c_str(), options)) {
			callLua(callback, "button");
		}
	}

	void ImGUI_Image(GLuint textureID, float width, float height) {
		if (!inMenu("ImGUI_Image")) return;
		ImGui::Image((ImTextureID)(intptr_t)textureID, ImVec2(width, height));
	}

	bool ImGUI_Checkbox(const std::string& label, bool currentValue)
	{
		bool value = currentValue;
		if (!inMenu("ImGUI_Checkbox")) return value;
		Controls::Toggle(label.c_str(), &value);
		return value;
	}

	// a texture the calling extension loaded itself
	static bool ownsTexture(GLuint texture) {
		auto* ext = Extensions::currentExtension;
		return ext && std::find(ext->textures.begin(), ext->textures.end(), texture) != ext->textures.end();
	}

	// ImGUI_Toggle(label, value, options), options is optional:
	// { look = "checkbox", size = "small", icon = a texture from LoadTexture }
	bool ImGUI_Toggle(const std::string& label, bool currentValue, sol::optional<sol::table> table)
	{
		bool value = currentValue;
		if (!inMenu("ImGUI_Toggle")) return value;
		Controls::ToggleOptions options;
		if (table) {
			options.checkbox = table->get_or<std::string>("look", "") == "checkbox";
			options.compact = table->get_or<std::string>("size", "") == "small";
			GLuint icon = table->get_or<GLuint>("icon", 0);
			if (icon && ownsTexture(icon)) options.icon = (ImTextureID)(intptr_t)icon;
		}
		Controls::Toggle(label.c_str(), &value, options);
		return value;
	}

	// the number piece, as every number in the menu
	static double drawnNumber(const std::string& label, double value, const Controls::NumberOptions& options) {
		value = Controls::Fit(value, options);
		Controls::Number(label.c_str(), value, options);
		return value;
	}

	float ImGUI_SliderFloat(const std::string& label, float currentValue, float minValue, float maxValue)
	{
		if (!inMenu("ImGUI_SliderFloat")) return currentValue;
		Controls::NumberOptions options;
		if (minValue < maxValue) { options.min = minValue; options.max = maxValue; }
		return static_cast<float>(drawnNumber(label, currentValue, options));
	}

	int ImGUI_SliderInt(const std::string& label, int currentValue, int minValue, int maxValue)
	{
		if (!inMenu("ImGUI_SliderInt")) return currentValue;
		Controls::NumberOptions options;
		if (minValue < maxValue) { options.min = minValue; options.max = maxValue; }
		options.whole = true;
		return static_cast<int>(drawnNumber(label, currentValue, options));
	}

	// ImGUI_Number(label, value, options), options is optional:
	// { min, max, step, default (shows Reset), field = true (no slider), whole = true }
	double ImGUI_Number(const std::string& label, double currentValue, sol::optional<sol::table> table)
	{
		if (!inMenu("ImGUI_Number")) return currentValue;
		Controls::NumberOptions options;
		if (table) {
			auto min = table->get<sol::optional<double>>("min");
			auto max = table->get<sol::optional<double>>("max");
			if (min && max && std::isfinite(*min) && std::isfinite(*max) && *min < *max) { options.min = *min; options.max = *max; }
			auto step = table->get<sol::optional<double>>("step");
			if (step && *step > 0 && std::isfinite(*step)) options.step = *step;
			auto fallback = table->get<sol::optional<double>>("default");
			if (fallback && std::isfinite(*fallback)) options.defaultValue = *fallback;
			options.field = table->get_or("field", false);
			options.whole = table->get_or("whole", false);
		}
		return drawnNumber(label, currentValue, options);
	}

	// ImGUI_InputText(label, text, options), options is optional: { hint, default (shows
	// Reset), clear = true (an x), confirm = true (counts once sent), taken = true (sent
	// text greyed), backToGame = false (keeps the focus after Enter or Esc, for the next box) }
	std::string ImGUI_InputText(const std::string& label, const std::string& currentText, sol::optional<sol::table> table)
	{
		std::string value = currentText;
		if (!inMenu("ImGUI_InputText")) return value;
		Controls::TextOptions options;
		if (table) {
			options.backToGame = table->get_or("backToGame", true);
			options.hint = table->get_or<std::string>("hint", "");
			if (auto fallback = table->get<sol::optional<std::string>>("default")) options.defaultValue = *fallback;
			options.clear = table->get_or("clear", false);
			options.confirm = table->get_or("confirm", false);
			options.taken = table->get_or("taken", false);
		}
		Controls::Text(label.c_str(), value, options);
		return value;
	}

	void ImGUI_SameLine()
	{
		if (!inMenu("ImGUI_SameLine")) return;
		ImGui::SameLine();
	}

	void ImGUI_Tooltip(const std::string& text)
	{
		if (!inMenu("ImGUI_Tooltip")) return;
		Controls::Tooltip(text.c_str());
	}

	void ImGUI_Waiting(const std::string& text)
	{
		if (!inMenu("ImGUI_Waiting")) return;
		Controls::Waiting(text.c_str());
	}

	void ImGUI_Empty(const std::string& text)
	{
		if (!inMenu("ImGUI_Empty")) return;
		Controls::Empty(text.c_str());
	}

	void ImGUI_Progress(const std::string& text, sol::optional<float> fraction)
	{
		if (!inMenu("ImGUI_Progress")) return;
		float shown = fraction && std::isfinite(*fraction) ? std::max(0.0f, *fraction) : -1.0f;
		Controls::Progress(text.c_str(), shown);
	}

	void ImGUI_Separator()
	{
		if (!inMenu("ImGUI_Separator")) return;
		ImGui::Separator();
	}

	// hidden means the menu is closed
	bool IsProgramHidden() {
		return !Overlay::MenuOpen();
	}

	// RegisterKeybind(key, function, flags, modifiers), modifiers is optional, like KeybindModifiers.Alt
	// Empty when the key can't be bound, the log says why. The keybind belongs to
	// the extension and goes with it (Extensions::unload).
	std::string RegisterKeybind(int glfwKey, sol::protected_function callbackFunction, Keybindings::KeybindFlags flags, sol::optional<int> modifiers) {
		// remember which extension made it, so GetSetting works inside the callback
		Extensions::Extension* owner = Extensions::currentExtension;
		if (!owner) return "";
		if (owner->keybinds.size() >= Extensions::MaxKeys) {
			Extensions::logOnce("RegisterKeybind", "has " + std::to_string(Extensions::MaxKeys) + " keys, no more are bound");
			return "";
		}
		std::string whyNot;
		std::string id = Keybindings::CreateKeybind(glfwKey, [owner, callbackFunction]() {
			Extensions::runForExtension(owner, [&callbackFunction]() {
				callLua(callbackFunction, "key");
				});
			}, flags, modifiers.value_or(Keybindings::KeybindModifiers_None), &whyNot);
		if (id.empty()) {
			std::cerr << "[" << owner->name << "] RegisterKeybind: " << whyNot << std::endl;
			return "";
		}
		owner->keybinds.push_back(id);
		return id;
	}

	// only the extension's own keybinds
	bool DeleteKeybind(const std::string& uuid) {
		Extensions::Extension* owner = Extensions::currentExtension;
		if (!owner) return false;
		auto it = std::find(owner->keybinds.begin(), owner->keybinds.end(), uuid);
		if (it == owner->keybinds.end()) return false;
		owner->keybinds.erase(it);
		return Keybindings::DeleteKeybind(uuid);
	}

	void DefineExtensionSetting(const std::string& id,
		const std::string& label,
		const std::string& type,
		sol::object defaultValue,
		sol::object minOrOptions,
		sol::optional<float> max)
	{
		auto extPtr = Extensions::currentExtension;
		if (!extPtr) {
			std::cerr << "Error: No current extension while defining setting!" << std::endl;
			return;
		}

		if (extPtr->settings.find(id) != extPtr->settings.end())
			return;
		if (extPtr->settings.size() >= Extensions::MaxSettings) {
			Extensions::logOnce("DefineExtensionSetting", "has " + std::to_string(Extensions::MaxSettings) + " settings, no more are made");
			return;
		}

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
			std::string text = defaultValue.as<std::string>();
			if (text.size() > Extensions::MaxSettingText) {
				Extensions::logOnce("DefineExtensionSetting", "takes a string default of " + std::to_string(Extensions::MaxSettingText) + " bytes at most");
				return;
			}
			s.type = Extensions::ExtensionSettingTypes::String;
			s.defaultValue = text;
		}
		else {
			std::cerr << "Bad setting " << id << ": type " << type << " doesn't match its default value" << std::endl;
			return;
		}

		// the range as two numbers, or { min, max, step, field }
		std::optional<float> min;
		if (minOrOptions.is<sol::table>()) {
			sol::table options = minOrOptions.as<sol::table>();
			if (auto v = options.get<sol::optional<float>>("min")) min = *v;
			if (auto v = options.get<sol::optional<float>>("max")) max = *v;
			auto step = options.get<sol::optional<float>>("step");
			if (step && *step > 0 && std::isfinite(*step)) s.step = *step;
			s.field = options.get_or("field", false);
		}
		else if (minOrOptions.is<float>()) min = minOrOptions.as<float>();
		if (min && max && std::isfinite(*min) && std::isfinite(*max) && *min < *max) {
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

	// A file there that isn't a plain file (a folder, a link to a device) is
	// never opened. One that isn't there yet passes when it's to be made.
	static bool plainFile(const std::filesystem::path& file, bool mayBeNew) {
		std::error_code ec;
		std::filesystem::file_status status = std::filesystem::status(file, ec);
		if (status.type() == std::filesystem::file_type::not_found) return mayBeNew;
		return !ec && status.type() == std::filesystem::file_type::regular;
	}

	// texture, width, height; nil when it can't be read
	sol::variadic_results LoadTextureFromFileLua(const std::string& path, sol::this_state state) {
		sol::variadic_results results;
		Extensions::Extension* ext = Extensions::currentExtension;
		if (ext && ext->textures.size() >= Extensions::MaxTextures) {
			Extensions::logOnce("LoadTexture", "has " + std::to_string(Extensions::MaxTextures) + " textures, no more are loaded");
			results.push_back(sol::make_object(state, sol::lua_nil));
			return results;
		}
		auto file = Extensions::pathFor(path, Extensions::Access::Read);
		if (!file || !plainFile(*file, false)) {
			results.push_back(sol::make_object(state, sol::lua_nil));
			return results;
		}
		int width = 0, height = 0;
		GLuint tex = Helpers::loadTextureFromFile(*file, width, height);
		if (tex != 0 && Extensions::currentExtension) Extensions::currentExtension->textures.push_back(tex);
		results.push_back(sol::make_object(state, tex));
		results.push_back(sol::make_object(state, width));
		results.push_back(sol::make_object(state, height));
		return results;
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

	sol::object NetPost(const std::string& url, const std::string& data, sol::this_state state, sol::optional<bool> hoyolab)
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
	// position at centerX, centerY. moving is true while the map moves, and
	// after it stops until it's lined up exactly. groupId and floorId are the
	// layer shown, layerFrom says who chose it: "game" (read off the game's screen),
	// "picked" (the overlay's floor picker) or "guess" (from the map's picture). ui is the game's
	// own UI as boxes to keep off, and the overlay's floor picker while it's
	// there. panelLeft is where a panel on the right starts, only while one is open.
	sol::object GetMapView(sol::this_state state) {
		MapTracking::MapView view = MapTracking::GetView();
		if (!view.visible) return sol::lua_nil;

		int windowX = 0, windowY = 0;
		glfwGetWindowPos(Overlay::Window, &windowX, &windowY);
		// the game in overlay pixels, everything below is placed from it
		RECT game = view.gameRect;
		OffsetRect(&game, -windowX, -windowY);
		float left = (float)game.left;
		float top = (float)game.top;
		float right = (float)game.right;
		float bottom = (float)game.bottom;

		sol::state_view lua(state);
		sol::table ui = lua.create_table();
		auto addBox = [&](const RECT& box) {
			ui.add(lua.create_table_with(
				"left", (float)box.left,
				"top", (float)box.top,
				"right", (float)box.right,
				"bottom", (float)box.bottom));
		};
		for (const RECT& box : GameUi::Boxes(game, view.panel)) addBox(box);
		RECT picker;
		if (Overlay::PickerBox(picker)) addBox(picker);
		sol::table result = lua.create_table_with(
			"mapId", view.result.mapId,
			"groupId", view.result.groupId,
			"floorId", view.result.floorId,
			"layerFrom", LayerChoice::Name(view.layerFrom),
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
		if (view.panel.seen == GameUi::Seen::Read && view.panel.left >= 0) result["panelLeft"] = left + view.panel.left;
		if (view.noMapFloor) result["noMapFloor"] = true;
		return sol::make_object(lua, result);
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
		if (textureID == 0 || !inFrame("DrawImage")) return;
		int a = (int)(alpha.value_or(1.0f) * 255.0f);
		ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)textureID, ImVec2(x0, y0), ImVec2(x1, y1),
			ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
	}

	void PushClipRect(float x0, float y0, float x1, float y1) {
		if (!inFrame("PushClipRect")) return;
		ImGui::GetBackgroundDrawList()->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
	}

	// only the clips this callback pushed: below them are the overlay's own
	void PopClipRect() {
		if (!inFrame("PopClipRect")) return;
		ImDrawList* list = ImGui::GetBackgroundDrawList();
		if (list->_ClipRectStack.Size <= Extensions::clipFloor) {
			Extensions::logOnce("PopClipRect", "has no PushClipRect of its own left to close");
			return;
		}
		list->PopClipRect();
	}

	// Deeper than this is refused, both ways: each level is a C++ call, and
	// without an end a table that holds itself runs out of stack, which nothing catches.
	static const int JSON_MAX_DEPTH = 128;

	// open: the tables being written right now, a table inside itself is refused
	static nlohmann::json LuaToJson(sol::object obj, int depth, std::vector<const void*>& open) {
		if (depth > JSON_MAX_DEPTH)
			throw std::runtime_error("deeper than " + std::to_string(JSON_MAX_DEPTH) + " levels");

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
			const void* self = table.pointer();
			if (std::find(open.begin(), open.end(), self) != open.end())
				throw std::runtime_error("a table holds itself");
			open.push_back(self);
			struct Close { std::vector<const void*>& open; ~Close() { open.pop_back(); } } close{ open };

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
					result.push_back(LuaToJson(table.raw_get<sol::object>(i), depth + 1, open));
				}

				return result;
			}

			nlohmann::json result = nlohmann::json::object();

			for (const auto& pair : table) {
				if (pair.first.get_type() != sol::type::string)
					throw std::runtime_error("JSON object keys must be strings");

				std::string key = pair.first.as<std::string>();
				result[key] = LuaToJson(pair.second, depth + 1, open);
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
			std::vector<const void*> open;
			std::string encoded = LuaToJson(value, 0, open).dump();
			return sol::make_object(state, encoded);
		}
		catch (const std::exception& e) {
			std::cerr << "JSON Encode Error: " << e.what() << std::endl;
			return sol::lua_nil;
		}
	}

	sol::object JSONDecode(const std::string& input, sol::this_state state) {
		try {
			// the depth is checked while reading, so a reply nested a million deep stops early
			nlohmann::json decoded = nlohmann::json::parse(input, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
				if (depth > JSON_MAX_DEPTH)
					throw std::runtime_error("deeper than " + std::to_string(JSON_MAX_DEPTH) + " levels");
				return true;
			});
			sol::state_view lua(state);
			return JsonToLua(decoded, lua);
		}
		catch (const std::exception& e) {
			std::cerr << "JSON Decode Error: " << e.what() << std::endl;
			return sol::lua_nil;
		}
	}

	sol::optional<std::string> ReadFile(const std::string& path) {
		auto file = Extensions::pathFor(path, Extensions::Access::Read);
		if (!file) return sol::nullopt;

		if (!plainFile(*file, false))
			throw std::runtime_error("Failed to open file");
		std::ifstream stream(*file, std::ios::binary);
		if (!stream)
			throw std::runtime_error("Failed to open file");

		return std::string(
			std::istreambuf_iterator<char>(stream),
			std::istreambuf_iterator<char>()
		);
	}

	bool FileExists(const std::string& path) {
		auto file = Extensions::pathFor(path, Extensions::Access::Read);
		if (!file) return false;
		std::error_code ec;
		return std::filesystem::is_regular_file(*file, ec);
	}

	// the names in the folder, nil when it may not look there
	sol::object ListDirectory(const std::string& path, sol::this_state state)
	{
		auto directory = Extensions::pathFor(path, Extensions::Access::Read);
		if (!directory) return sol::lua_nil;

		sol::state_view lua(state);
		sol::table result = lua.create_table();
		std::error_code ec;
		if (!std::filesystem::is_directory(*directory, ec)) return result;

		int index = 1;
		for (const auto& entry : std::filesystem::directory_iterator(*directory, ec))
		{
			result[index++] = Helpers::utf8(entry.path().filename());
		}

		return result;
	}

	// checked before the file is opened, so a refused write leaves the file as it was
	static void checkWriteLimit(const std::string& data) {
		auto* ext = Extensions::currentExtension;

		if (!ext)
			throw std::runtime_error("No current extension");

		if (data.size() > (size_t)(Extensions::ExtensionWriteLimit - ext->bytesToDiskWritten))
			throw std::runtime_error("Extension disk write limit exceeded");
	}

	static void writeExtensionData(std::ofstream& file, const std::string& data) {
		file.write(data.data(), static_cast<std::streamsize>(data.size()));

		if (!file)
			throw std::runtime_error("Failed to write file");

		Extensions::currentExtension->bytesToDiskWritten += static_cast<int>(data.size());
	}

	bool WriteFile(const std::string& path, const std::string& data) {
		auto file = Extensions::pathFor(path, Extensions::Access::Write);
		if (!file) return false;
		checkWriteLimit(data);

		if (!plainFile(*file, true))
			throw std::runtime_error("Failed to open file");
		std::ofstream stream(*file, std::ios::binary | std::ios::trunc);

		if (!stream)
			throw std::runtime_error("Failed to open file");

		writeExtensionData(stream, data);

		return true;
	}

	bool AppendFile(const std::string& path, const std::string& data) {
		auto file = Extensions::pathFor(path, Extensions::Access::Write);
		if (!file) return false;
		checkWriteLimit(data);

		if (!plainFile(*file, true))
			throw std::runtime_error("Failed to open file");
		std::ofstream stream(*file, std::ios::binary | std::ios::app);

		if (!stream)
			throw std::runtime_error("Failed to open file");

		writeExtensionData(stream, data);

		return true;
	}

	bool WebSocketConnect(Net::WebSocket& socket, const std::string& url, sol::optional<bool> hoyolab)
	{
		return socket.connect(url, hoyolab.value_or(false));
	}

	bool WebSocketSend(Net::WebSocket& socket, const std::string& message)
	{
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
		Extensions::Extension* owner = Extensions::currentExtension;
		if (owner && (size_t)std::count_if(intervals.begin(), intervals.end(),
			[owner](const LuaInterval& each) { return each.owner == owner; }) >= Extensions::MaxIntervals) {
			Extensions::logOnce("SetInterval", "has " + std::to_string(Extensions::MaxIntervals) + " intervals, no more are set");
			return 0;
		}

		LuaInterval interval;
		interval.id = nextIntervalId++;
		interval.milliseconds = milliseconds;
		interval.nextExecution = GetOSTimeMS() + milliseconds;
		interval.owner = owner;
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

	void forgetIntervals(Extensions::Extension* owner)
	{
		intervals.erase(std::remove_if(intervals.begin(), intervals.end(),
			[owner](const LuaInterval& interval) { return interval.owner == owner; }), intervals.end());
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
				callLua(callback, "interval");
				});
		}
	}
	std::optional<std::pair<int, int>> mouseFor(POINT cursor, const std::optional<RECT>& game, POINT overlay, bool anywhere) {
		bool overGame = game && cursor.x >= game->left && cursor.x < game->right && cursor.y >= game->top && cursor.y < game->bottom;
		if (!anywhere && !overGame) return std::nullopt;
		return std::make_pair((int)(cursor.x - overlay.x), (int)(cursor.y - overlay.y));
	}

	sol::variadic_results GetMousePosition(sol::this_state state)
	{
		sol::variadic_results results;
		Extensions::Extension* ext = Extensions::currentExtension;
		POINT cursor;
		std::optional<std::pair<int, int>> at;
		if (ext && GetCursorPos(&cursor)) {
			std::optional<RECT> game;
			RECT rect;
			HWND window = Capture::findRunningGame();
			if (window && Capture::getClientRectOnScreen(window, rect)) game = rect;
			int x = 0, y = 0;
			if (Overlay::Window) glfwGetWindowPos(Overlay::Window, &x, &y);
			at = mouseFor(cursor, game, POINT{ x, y }, ext->extensionPermissions.count(Permission::MouseAnywhere) > 0);
		}
		if (!at) {
			results.push_back(sol::make_object(state, sol::lua_nil));
			return results;
		}
		results.push_back(sol::make_object(state, at->first));
		results.push_back(sol::make_object(state, at->second));
		return results;
	}
	sol::table GetTheme(sol::this_state state) {
		return themeTable(Controls::GetTheme(), state);
	}

	sol::table themeTable(const Controls::Theme& theme, sol::state_view lua) {
		sol::table table = lua.create_table_with(
			"look", Controls::LookName(theme.look), "dark", theme.dark,
			"text", theme.text, "muted", theme.muted, "faint", theme.faint,
			"panel", theme.panel, "page", theme.page, "inside", theme.inside, "cardLine", theme.cardLine,
			"line", theme.line, "picked", theme.picked, "button", theme.button, "buttonLine", theme.buttonLine,
			"gold", theme.gold, "accent", theme.accent, "onAccent", theme.onAccent, "badgeFill", theme.badgeFill,
			"warning", theme.warning, "risky", theme.risky, "success", theme.success, "done", theme.done,
			"shadow", theme.shadow, "switchOff", theme.switchOff);
		table["faded"] = theme.faded;
		table["font"] = theme.font;
		table["gap"] = theme.gap;
		table["rounding"] = theme.rounding;
		table["badge"] = theme.badge;
		return table;
	}

	void GDrawText(const std::string& text, float x, float y, sol::optional<float> size, sol::optional<uint32_t> color)
	{
		if (text.empty() || !inFrame("DrawText"))
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
			text.c_str()
		);
	}
	// The map items, all read only from the catalog in memory. Unknown ids give
	// nil. Take what you keep again when GetItemsVersion changes: a type or a
	// point can be gone from the new list, and icons from the old one are freed.
	// One catalog for the whole frame, so every extension sees the same one.
	namespace Items {
		static std::shared_ptr<const ItemCatalog::Catalog> frameCatalog;

		void beginFrame() {
			frameCatalog = ItemCatalog::current();
			ItemIcons::keepFor(frameCatalog.get());
		}

		static const char* stateName(ItemFiles::State state) {
			switch (state) {
			case ItemFiles::State::Downloading: return "downloading";
			case ItemFiles::State::Ready: return "ready";
			case ItemFiles::State::Old: return "old";
			default: return "none";
			}
		}

		static const char* reasonName(ItemFiles::Reason reason) {
			switch (reason) {
			case ItemFiles::Reason::NoNetwork: return "no_network";
			case ItemFiles::Reason::NewerFormat: return "newer_format";
			case ItemFiles::Reason::BrokenPack: return "broken";
			case ItemFiles::Reason::CantSave: return "cant_save";
			case ItemFiles::Reason::DownloadOff: return "download_off";
			default: return "none";
			}
		}

		static bool knowsType(const ItemCatalog::Catalog& catalog, int type) {
			for (const auto& group : catalog.groups) {
				for (const auto& t : group.types) if (t.id == type) return true;
			}
			return false;
		}

		sol::table state(const ItemFiles::Status& status, sol::state_view lua) {
			return lua.create_table_with(
				"state", stateName(status.state),
				"reason", reasonName(status.reason),
				"percent", status.percent,
				"built", status.built,
				"checking", status.checking || !status.started
			);
		}

		sol::table types(const ItemCatalog::Catalog* catalog, sol::state_view lua) {
			sol::table groups = lua.create_table();
			if (!catalog) return groups;
			int g = 1;
			for (const auto& group : catalog->groups) {
				sol::table types = lua.create_table((int)group.types.size(), 0);
				int t = 1;
				for (const auto& type : group.types) {
					sol::table counts = lua.create_table();
					for (const auto& [map, n] : type.counts) counts[map] = n;
					types[t++] = lua.create_table_with("id", type.id, "name", type.name, "counts", counts);
				}
				groups[g++] = lua.create_table_with("id", group.id, "name", group.name, "types", types);
			}
			return groups;
		}

		sol::object positions(const ItemCatalog::Catalog* catalog, int type, sol::state_view lua) {
			if (!catalog) return sol::lua_nil;
			ItemCatalog::Points points = catalog->points(type);
			if (points.empty() && !knowsType(*catalog, type)) return sol::lua_nil;

			sol::table result = lua.create_table((int)points.size(), 0);
			int i = 1;
			for (const auto& point : points) {
				result[i++] = lua.create_table_with(
					"id", point.id,
					"map", point.map,
					"x", point.x,
					"y", point.y,
					"area", point.area,
					"group", point.group,
					"floor", point.floor
				);
			}
			return result;
		}

		sol::table regions(const ItemCatalog::Catalog* catalog, sol::state_view lua) {
			sol::table result = lua.create_table();
			if (!catalog) return result;
			int i = 1;
			for (const auto& region : catalog->regions) {
				result[i++] = lua.create_table_with(
					"id", region.id,
					"name", region.name,
					"map", region.map,
					"box", lua.create_table_with(
						"left", region.left,
						"top", region.top,
						"right", region.right,
						"bottom", region.bottom
					)
				);
			}
			return result;
		}
	}

	// 0 while there are none, goes up by one with each new list
	uint64_t GetItemsVersion() {
		return Items::frameCatalog ? Items::frameCatalog->version : 0;
	}

	// { state, reason, percent, built, checking }, for an extension's own "waiting" text
	sol::object GetItemsState(sol::this_state state) {
		return Items::state(ItemFiles::GetStatus(), state);
	}

	sol::object GetItemTypes(sol::this_state state) {
		return Items::types(Items::frameCatalog.get(), state);
	}

	// x and y are HoYoLAB map units, the same as GetMapView's lat and lng
	sol::object GetItemPositions(int type, sol::this_state state) {
		return Items::positions(Items::frameCatalog.get(), type, state);
	}

	sol::object GetItemRegions(sol::this_state state) {
		return Items::regions(Items::frameCatalog.get(), state);
	}

	// made once per type and kept, valid until GetItemsVersion changes
	sol::object GetItemIcon(int type, sol::this_state state) {
		GLuint texture = ItemIcons::get(Items::frameCatalog.get(), type);
		if (texture == 0) return sol::lua_nil;
		return sol::make_object(state, texture);
	}
}
