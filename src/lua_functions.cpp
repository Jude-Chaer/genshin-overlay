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
#include "file_dialogs.hpp"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include "item_icons.hpp"
#include "item_catalog.hpp"
#include "item_files.hpp"
#include "typing.hpp"
#include "asks.hpp"
#include "triggers.hpp"
#include "key_choices.hpp"
#include "settings.hpp"
#include "notices.hpp"
#include "screen_pictures.hpp"
#include "packets/game_traffic.hpp"
#include "accounts.hpp"
#include "marks.hpp"
#include <imgui.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include "web_images.hpp"
#include "point_guides.hpp"
#include "language.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <memory>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace LuaFunctions {
	using Permission = Extensions::ExtensionPermissionTypes;

	std::vector<LuaInterval> intervals;
	static int nextIntervalId = 1;

	// what is the kind of callback, for the log: "button", "key", "interval", "menu item"
	// false when it failed, the log says why
	static bool callLua(const sol::protected_function& function, const char* what) {
		auto result = function();
		if (!result.valid()) {
			sol::error err = result;
			Extensions::Extension* ext = Extensions::currentExtension;
			std::cerr << "[" << (ext ? ext->name : std::string("no extension")) << "] " << what << ": " << err.what() << std::endl;
			return false;
		}
		return true;
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
		const Permission MapInfo = Permission::MapInfo;
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
		global("ImGUI_Tabs", None, ImGUI_Tabs);
		global("ImGUI_Header", None, ImGUI_Header);
		global("ImGUI_EndHeader", None, ImGUI_EndHeader);
		global("ImGUI_SideList", None, ImGUI_SideList);
		global("ImGUI_EndSideList", None, ImGUI_EndSideList);
		global("ImGUI_RightClick", None, ImGUI_RightClick);
		global("ImGUI_InputText", None, ImGUI_InputText);
		global("ImGUI_SameLine", None, ImGUI_SameLine);
		global("ImGUI_Tooltip", None, ImGUI_Tooltip);
		global("Notice", Permission::Notices, Notice);
		global("Dot", Permission::Dots, Dot);
		global("ImGUI_Waiting", None, ImGUI_Waiting);
		global("ImGUI_Empty", None, ImGUI_Empty);
		global("ImGUI_Progress", None, ImGUI_Progress);
		global("ImGUI_Separator", None, ImGUI_Separator);
		global("GetScreens", None, GetScreens);
		global("DefineExtensionSetting", None, DefineExtensionSetting);
		global("GetSetting", None, GetExtensionSetting);
		global("JSON_Encode", None, JSONEncode);
		global("JSON_Decode", None, JSONDecode);
		global("SetInterval", None, SetInterval);
		global("ClearInterval", None, ClearInterval);
		// the map, player and item ones run only with Map info, like their calls
		global("AddTrigger", None, Triggers::add);
		global("RemoveTrigger", None, Triggers::remove);
		global("GetOSTimeMS", None, GetOSTimeMS);
		global("PermissionState", None, PermissionState);
		global("AskForPermission", None, AskForPermission);
		global("OpenPermissions", None, OpenPermissions);

		// they ask for Keybinds themselves, a key waits until it's allowed
		global("RegisterKeybind", None, RegisterKeybind);
		global("DeleteKeybind", None, DeleteKeybind);
		// its own items in its ⋯ menu in the page list
		global("AddMenuItem", None, AddMenuItem);
		global("RemoveMenuItem", None, RemoveMenuItem);

		// drawing on the overlay
		global("DrawImage", DisplayOverlay, DrawImage);
		global("GetWebImage", Network, GetWebImage);
		global("GetPointGuide", Network, GetPointGuide);
		global("DrawText", DisplayOverlay, GDrawText);
		global("DrawRect", DisplayOverlay, DrawRect);
		global("DrawCircle", DisplayOverlay, DrawCircle);
		global("DrawBadge", DisplayOverlay, DrawBadge);
		global("DrawBubble", DisplayOverlay, DrawBubble);
		global("TextSize", None, TextSize);
		global("GetTheme", None, GetTheme);
		global("PushClipRect", DisplayOverlay, PushClipRect);
		global("PopClipRect", DisplayOverlay, PopClipRect);
		global("ClickArea", Permission::TakeClicks, ClickArea);

		// what's on the screen and the map
		global("GetMapView", MapInfo, GetMapView);
		global("FollowMapWhileMoving", MapInfo, FollowMapWhileMoving);
		global("GetPlayer", MapInfo, GetPlayer);
		global("GetMapGrid", MapInfo, GetMapGrid);
		global("GetMapTile", MapInfo, GetMapTile);
		global("GetMapFloor", MapInfo, GetMapFloor);
		global("FreeSpace", MapInfo, FreeSpace);
		global("GetMousePosition", MapInfo, GetMousePosition);
		global("GetScreenPicture", Permission::DisplayRead, GetScreenPicture);
		global("CompareImages", None, CompareImages);
		global("GetItems", MapInfo, GetItems);
		global("GetItemTypes", MapInfo, GetItemTypes);
		global("GetItemPositions", MapInfo, GetItemPositions);
		global("GetItemRegions", MapInfo, GetItemRegions);
		global("GetItemIcon", MapInfo, GetItemIcon);
		global("GetGameTraffic", None, GetGameTraffic);

		// the player's logins, each site's account permission checked inside
		global("GetAccount", None, GetAccount);
		global("LogIn", None, LogIn);
		global("LogOut", None, LogOut);
		global("Marks_Start", None, MarksStart);
		global("Marks_Result", None, MarksResult);
		global("Marks_Backups", None, MarksBackups);

		// files, only in the extension's own folder (pathFor)
		global("LoadTexture", InternalRead, LoadTextureFromFileLua);
		global("ReadFile", InternalRead, ReadFile);
		global("FileExists", InternalRead, FileExists);
		global("ListDirectory", InternalRead, ListDirectory);
		global("WriteFile", InternalWrite, WriteFile);
		global("AppendFile", InternalWrite, AppendFile);

		global("Net_Get", Network, NetGet);
		global("Net_Post", Network, NetPost);
		global("Net_Start", Network, NetStart);
		global("Net_Result", Network, NetResult);
		global("OpenFileDialog", None, OpenFileDialog);
		global("SaveFileDialog", None, SaveFileDialog);
		global("FileDialogResult", None, FileDialogResult);

		luaState["KeybindModifiers"] = luaState.create_table_with(
			"None", Keybindings::KeybindModifiers_None,
			"Alt", Keybindings::KeybindModifiers_Alt,
			"Ctrl", Keybindings::KeybindModifiers_Ctrl,
			"Shift", Keybindings::KeybindModifiers_Shift
		);

		auto websocket = luaState.new_usertype<Socket>("WebSocket", sol::constructors<Socket()>());
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

	// ImGUI_Text(text, options), options is optional: { look = "title" } for a part's name.
	// It wraps at the page's edge.
	void ImGUI_Text(const std::string& str, sol::optional<sol::table> table) {
		if (!inMenu("ImGUI_Text")) return;
		if (table && table->get_or<std::string>("look", "") == "title") Controls::PartTitle(str.c_str());
		else ImGui::TextWrapped("%s", str.c_str());
	}

	// while the extension's button or key function runs, lowered however it ends
	struct PlayerAction {
		Extensions::Extension* ext;
		explicit PlayerAction(Extensions::Extension* ext) : ext(ext) { if (ext) ext->playerAction++; }
		~PlayerAction() { if (ext) ext->playerAction--; }
	};

	// whether one of the extension's controls has a dot, by its label
	static bool dotted(const std::string& label) {
		auto* ext = Extensions::currentExtension;
		return ext && ext->dots.count(label);
	}

	// a control's dot goes once the player uses it
	static void used(const std::string& label, bool used) {
		if (used && Extensions::currentExtension) Extensions::currentExtension->dots.erase(label);
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
		options.dotted = dotted(label);
		bool pressed = Controls::Button(label.c_str(), options);
		used(label, pressed);
		if (pressed) {
			PlayerAction action(Extensions::currentExtension);
			callLua(callback, "button");
		}
	}

	bool ImGUI_Checkbox(const std::string& label, bool currentValue)
	{
		bool value = currentValue;
		if (!inMenu("ImGUI_Checkbox")) return value;
		Controls::ToggleOptions options;
		options.dotted = dotted(label);
		used(label, Controls::Toggle(label.c_str(), &value, options));
		return value;
	}

	// a texture the calling extension loaded itself, or one the overlay gave
	// out: a map tile or floor, an item icon, a screen picture with Display Read
	static bool ownsTexture(GLuint texture) {
		auto* ext = Extensions::currentExtension;
		if (!ext || texture == 0) return false;
		if (std::find(ext->textures.begin(), ext->textures.end(), texture) != ext->textures.end()) return true;
		if (MapTiles::Owns(texture) || ItemIcons::owns(texture)) return true;
		return ScreenPictures::owns(texture) && ext->extensionPermissions.count(Permission::DisplayRead) > 0;
	}

	static WebImages::Image webImage(const char* function, const std::string& url);

	// a number from Lua as a texture to draw: a web image's handle, or a texture
	// the extension may draw; 0 for anything else
	static GLuint textureOf(const char* function, double value) {
		Extensions::Extension* ext = Extensions::currentExtension;
		if (WebImages::isHandle(value)) return WebImages::texture(ext, value);
		if (value < 0 || value != (double)(GLuint)value) return 0;
		GLuint texture = (GLuint)value;
		if (!ownsTexture(texture)) {
			Extensions::logOnce(function, "draws only textures it loaded itself");
			return 0;
		}
		return texture;
	}

	// a table's icon field, a texture or a web image's handle; 0 when none
	static ImTextureID iconOf(const char* function, const sol::table& table) {
		double value = table.get_or("icon", 0.0);
		if (value == 0) return 0;
		return (ImTextureID)(intptr_t)textureOf(function, value);
	}

	bool ImGUI_Image(sol::object image, float width, float height, sol::optional<sol::table> table) {
		if (!inMenu("ImGUI_Image")) return false;
		GLuint texture = 0;
		bool loading = false;
		if (image.is<std::string>()) {
			if (!Extensions::allowed("ImGUI_Image", Permission::NetworkAccess)) return false;
			WebImages::Image web = webImage("ImGUI_Image", image.as<std::string>());
			texture = WebImages::texture(Extensions::currentExtension, web.handle);
			loading = web.state == WebImages::State::Loading;
		}
		else if (image.is<double>()) {
			texture = textureOf("ImGUI_Image", image.as<double>());
			if (texture == 0) return false;
		}
		else return false;

		float scale = ImGui::GetStyle().FontScaleDpi;
		ImVec2 size(std::max(1.0f, width * scale), std::max(1.0f, height * scale));
		sol::object onClick = table ? (*table)["onClick"] : sol::object(sol::lua_nil);
		bool clickable = onClick.is<sol::protected_function>();

		// one id per image drawn this frame, the same picture can be there twice
		static int frame = -1, drawn = 0;
		if (frame != ImGui::GetFrameCount()) { frame = ImGui::GetFrameCount(); drawn = 0; }
		ImGui::PushID(drawn++);
		ImVec2 at = ImGui::GetCursorScreenPos();
		bool clicked = false;
		bool hovered = false;
		if (clickable) {
			clicked = ImGui::InvisibleButton("image", size);
			hovered = ImGui::IsItemHovered();
			if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}
		else ImGui::Dummy(size);
		ImGui::PopID();

		ImDrawList* draw = ImGui::GetWindowDrawList();
		ImVec2 end(at.x + size.x, at.y + size.y);
		if (texture != 0) draw->AddImage((ImTextureID)(intptr_t)texture, at, end);
		else {
			draw->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_FrameBg));
			ImVec2 text = ImGui::CalcTextSize("...");
			if (loading && text.x < size.x && text.y < size.y)
				draw->AddText(ImVec2(at.x + (size.x - text.x) / 2, at.y + (size.y - text.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), "...");
		}
		if (hovered) draw->AddRect(at, end, ImGui::GetColorU32(ImGuiCol_ButtonHovered));

		if (clicked) {
			PlayerAction action(Extensions::currentExtension);
			callLua(onClick.as<sol::protected_function>(), "image");
		}
		return clicked;
	}

	// ImGUI_Tabs({ "Markers", "Sync" }) draws the extension's own tab row and gives the open tab's name
	// a part of the page shown this frame: its dot goes once it's been open long enough
	static void dotSeen(Extensions::Extension* ext, const std::string& part) {
		auto dot = ext->dots.find(part);
		if (dot == ext->dots.end()) return;
		double now = ImGui::GetTime();
		int frame = ImGui::GetFrameCount();
		Extensions::Extension::DotSeen& seen = dot->second;
		// not open last frame: the time starts again
		if (seen.lastFrame < 0 || frame - seen.lastFrame > 1) seen.since = now;
		seen.lastFrame = frame;
		if (now - seen.since >= Extensions::DotSeenAfter) ext->dots.erase(dot);
	}

	void Dot(const std::string& part, sol::optional<bool> on) {
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext || part.empty()) return;
		if (!on.value_or(true)) {
			ext->dots.erase(part);
			return;
		}
		if (ext->dots.count(part)) return;
		if (ext->dots.size() >= Extensions::MaxDots) {
			Extensions::logOnce("Dot", "has 3 dots on, no more are added");
			return;
		}
		ext->dots[part] = {};
	}

	sol::object ImGUI_Tabs(sol::table names, sol::this_state state) {
		if (!inMenu("ImGUI_Tabs")) return sol::lua_nil;
		std::vector<std::string> list;
		for (size_t i = 1; i <= Extensions::MaxTabs; i++) {
			sol::optional<std::string> name = names[i];
			if (!name) break;
			list.push_back(*name);
		}
		// the rows by their order in the frame, so a renamed tab keeps the row's open one
		static int rowFrame = -1, row = 0;
		if (rowFrame != ImGui::GetFrameCount()) {
			rowFrame = ImGui::GetFrameCount();
			row = 0;
		}
		std::string id = "##tabs" + std::to_string(row++);
		if (list.empty()) return sol::lua_nil;
		auto* ext = Extensions::currentExtension;
		std::vector<bool> dotted;
		for (const std::string& name : list) dotted.push_back(ext && ext->dots.count(name));
		const std::string& open = list[Controls::Segments(id.c_str(), list, dotted)];
		if (ext) dotSeen(ext, open);
		return sol::make_object(state, open);
	}

	static size_t utf8Characters(const std::string& text) {
		size_t characters = 0;
		for (unsigned char c : text) if ((c & 0xC0) != 0x80) characters++;
		return characters;
	}

	// a summary line from an extension, its line breaks and tabs as spaces;
	// none past MaxSummary characters
	static std::string summaryText(const char* function, const std::string& text) {
		if (utf8Characters(text) > Extensions::MaxSummary) {
			Extensions::logOnce(function, "summary is longer than " + std::to_string(Extensions::MaxSummary) + " characters");
			return "";
		}
		std::string out = text;
		for (char& c : out) if ((unsigned char)c < 0x20) c = ' ';
		return out;
	}

	// ImGUI_Header(label, options) is true while the part under it is open. options is optional:
	// { open = true } opens it until the player closes it. The player's choice is kept.
	// { summary = "Synced 2 minutes ago" } is a short line under its name, open or closed.
	// What's drawn while it's open sits in a card under the header, up to the next
	// header, ImGUI_EndHeader() or the end of Menu().
	bool ImGUI_Header(const std::string& label, sol::optional<sol::table> table) {
		if (!inMenu("ImGUI_Header")) return false;
		bool open = table ? table->get_or("open", false) : false;
		Controls::Summary summary;
		if (table) summary.text = summaryText("ImGUI_Header", table->get_or("summary", std::string()));
		auto* ext = Extensions::currentExtension;
		if (!ext) {
			Controls::Header(label.c_str(), open, false, summary);
			return open;
		}
		ImGUI_EndHeader();
		auto saved = ext->openHeaders.find(label);
		bool known = saved != ext->openHeaders.end();
		if (known) open = saved->second;
		auto session = ext->sessionHeaders.find(label);
		if (!known && session != ext->sessionHeaders.end()) open = session->second;
		bool roomFor = known || (ext->openHeaders.size() < Extensions::MaxHeaders && label.size() <= Extensions::MaxHeaderLabel);
		if (Controls::BeginHeader(label.c_str(), open, ext->dots.count(label) > 0, summary).changed) {
			if (roomFor) {
				ext->openHeaders[label] = open;
				Settings::SaveHeaders(ext->folderKey, ext->openHeaders);
			}
			else if (session != ext->sessionHeaders.end()) session->second = open;
			else if (ext->sessionHeaders.size() < Extensions::MaxSessionHeaders) ext->sessionHeaders[label] = open;
		}
		if (open) dotSeen(ext, label);
		ext->headerCard = open;
		return open;
	}

	// ImGUI_EndHeader() ends the open header's card early, so what comes after
	// sits on the page. Nothing happens when no header is open.
	void ImGUI_EndHeader() {
		auto* ext = Extensions::currentExtension;
		if (!ext || !ext->headerCard) return;
		ext->headerCard = false;
		Controls::EndSection();
	}

	// the open item of each extension's side lists, by folder and list id, kept by name
	static std::map<std::string, std::string> sideListOpen;

	// ImGUI_SideList(id, items) draws items down the page's side as the menu's own
	// page list and gives the open one's number, from 1. items: { { name = "Chests",
	// icon = a texture, summary = "3 on", on = true }, ... }, all but name optional;
	// on draws the summary in the accent, for an item with picks in it; glyph =
	// "sets" or "collected" draws the overlay's own icon instead of a picture,
	// for an item that isn't one of the others; line = true puts a line under the item. A third
	// argument { open = 2 } picks the item open until the player picks one. What's
	// drawn after it sits beside the list, up to ImGUI_EndSideList() or the end of
	// Menu(); the list and the part beside it each scroll by themselves, down to
	// the page's visible end. A page too narrow for both shows the items as chips above instead.
	sol::object ImGUI_SideList(const std::string& id, sol::table items, sol::optional<sol::table> options, sol::this_state state) {
		if (!inMenu("ImGUI_SideList")) return sol::lua_nil;
		auto* ext = Extensions::currentExtension;
		if (!ext) return sol::lua_nil;
		ImGUI_EndSideList();
		ImGUI_EndHeader();
		struct Item { std::string name; ImTextureID icon = 0; Controls::Summary summary; Controls::PageIcon glyph = Controls::PageIcon::None; bool line = false; };
		std::vector<Item> list;
		for (size_t i = 1; i <= Extensions::MaxSideItems; i++) {
			sol::optional<sol::table> entry = items[i];
			if (!entry) break;
			Item item;
			item.name = entry->get_or<std::string>("name", "");
			item.icon = iconOf("ImGUI_SideList", *entry);
			item.summary.text = summaryText("ImGUI_SideList", entry->get_or<std::string>("summary", ""));
			if (entry->get_or("on", false)) item.summary.kind = Controls::Summary::Kind::On;
			std::string glyph = entry->get_or<std::string>("glyph", "");
			if (glyph == "sets") item.glyph = Controls::PageIcon::Sets;
			if (glyph == "collected") item.glyph = Controls::PageIcon::Collected;
			if (item.glyph != Controls::PageIcon::None) item.icon = 0;
			item.line = entry->get_or("line", false);
			list.push_back(item);
		}
		if (list.empty()) return sol::lua_nil;

		std::string key = ext->folderKey + "\n" + id;
		// options { open = 2 }: the item open until the player picks one, from 1
		size_t open = 0;
		if (options) open = (size_t)std::clamp(options->get_or("open", 1), 1, (int)list.size()) - 1;
		auto kept = sideListOpen.find(key);
		if (kept != sideListOpen.end()) {
			for (size_t i = 0; i < list.size(); i++) if (list[i].name == kept->second) open = i;
		}

		float font = ImGui::GetFontSize();
		float avail = ImGui::GetContentRegionAvail().x;
		float width = font * 8;
		for (const Item& item : list) width = std::max(width, Controls::PageButtonWidth(item.name.c_str(), item.summary));
		width = std::ceil(std::min(width, avail * 0.45f));
		bool beside = avail - width >= font * 14;
		// beside: the list and what's next to it each scroll by themselves, down
		// to the page's visible end
		float height = std::max(font * 16, ImGui::GetContentRegionAvail().y);

		ImGui::PushID(("##side" + id).c_str());
		if (beside) {
			ImGui::BeginChild("list", ImVec2(width, height), 0, ImGuiWindowFlags_NoBackground);
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, font * 0.2f));
			for (size_t i = 0; i < list.size(); i++) {
				ImGui::PushID((int)i);
				if (Controls::PageButton(list[i].name.c_str(), i == open, list[i].icon, false, 0, list[i].summary, list[i].glyph, false, true)) open = i;
				ImGui::PopID();
				if (list[i].line) {
					// a thin line in the look's line colour, with room around it
					ImVec2 at = ImGui::GetCursorScreenPos();
					float y = std::floor(at.y + font * 0.3f) + 0.5f;
					ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, y), ImVec2(at.x + ImGui::GetContentRegionAvail().x, y), Controls::Color(Controls::GetTheme().line));
					ImGui::Dummy(ImVec2(0, font * 0.6f));
				}
			}
			ImGui::PopStyleVar();
			ImGui::EndChild();
		}
		else {
			for (size_t i = 0; i < list.size(); i++) {
				ImGui::PushID((int)i);
				bool on = i == open;
				Controls::ToggleOptions options;
				options.chip = true;
				options.icon = list[i].icon;
				options.note = list[i].summary.text;
				if (Controls::Toggle(list[i].name.c_str(), &on, options)) open = i;
				ImGui::PopID();
			}
		}
		ImGui::PopID();
		sideListOpen[key] = list[open].name;
		if (beside) {
			ImGui::SameLine(0, font);
			ImGui::BeginChild(("##beside" + id).c_str(), ImVec2(0, height), 0, ImGuiWindowFlags_NoBackground);
			ext->sideList = true;
		}
		return sol::make_object(state, (int)open + 1);
	}

	// ImGUI_EndSideList() ends the part beside a side list, so what comes after
	// takes the page's whole width. Nothing happens with no side list open.
	void ImGUI_EndSideList() {
		auto* ext = Extensions::currentExtension;
		if (!ext || !ext->sideList) return;
		ImGUI_EndHeader();
		ext->sideList = false;
		ImGui::EndChild();
	}

	// ImGUI_RightClick(id, items) goes right after an item: a right-click on that
	// item opens a menu, as the menu's own page list has it. items: { { name =
	// "Rename" }, { name = "Icon", items = { { name = "Chests", icon = a texture }
	// } }, ... }, an item with items opens them beside it. Gives the picked item's
	// name and the name of the item it's in, nothing while none is picked.
	struct RightClickItem { std::string name; ImTextureID icon = 0; std::vector<RightClickItem> inside; };

	static std::vector<RightClickItem> rightClickItems(sol::table items, bool top) {
		std::vector<RightClickItem> list;
		for (size_t i = 1; i <= Extensions::MaxMenuEntries; i++) {
			sol::optional<sol::table> entry = items[i];
			if (!entry) break;
			RightClickItem item;
			item.name = entry->get_or<std::string>("name", "");
			if (item.name.empty()) continue;
			item.icon = iconOf("ImGUI_RightClick", *entry);
			// one level of items inside
			sol::optional<sol::table> inside = entry->get<sol::optional<sol::table>>("items");
			if (top && inside) item.inside = rightClickItems(*inside, false);
			list.push_back(item);
		}
		return list;
	}

	static bool rightClickRow(const RightClickItem& item) {
		ImGui::PushID(item.name.c_str());
		bool picked;
		if (item.icon) {
			float side = ImGui::GetTextLineHeight();
			ImVec2 at = ImGui::GetCursorScreenPos();
			// spaces as wide as the icon and a gap, whatever the font's space is
			float space = std::max(1.0f, ImGui::CalcTextSize(" ").x);
			std::string room = std::string((size_t)std::ceil(side * 1.4f / space), ' ') + item.name;
			picked = ImGui::MenuItem(room.c_str());
			ImGui::GetWindowDrawList()->AddImage(item.icon, ImVec2(at.x, at.y), ImVec2(at.x + side, at.y + side));
		}
		else picked = ImGui::MenuItem(item.name.c_str());
		ImGui::PopID();
		return picked;
	}

	sol::variadic_results ImGUI_RightClick(const std::string& id, sol::table items, sol::this_state state) {
		sol::variadic_results out;
		if (!inMenu("ImGUI_RightClick")) return out;
		std::string popup = "##rightclick" + id;
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
			&& ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()))
			ImGui::OpenPopup(popup.c_str());
		if (!ImGui::BeginPopup(popup.c_str())) return out;
		for (const RightClickItem& item : rightClickItems(items, true)) {
			if (item.inside.empty()) {
				if (rightClickRow(item)) out.push_back(sol::make_object(state, item.name));
				continue;
			}
			if (!ImGui::BeginMenu(item.name.c_str())) continue;
			for (const RightClickItem& inner : item.inside) {
				if (rightClickRow(inner)) {
					out.push_back(sol::make_object(state, inner.name));
					out.push_back(sol::make_object(state, item.name));
				}
			}
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
		return out;
	}

	bool Notice(const std::string& text, sol::optional<sol::table> options) {
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext) return false;
		::Notices::Kind kind = ::Notices::Kind::Plain;
		std::string tip;
		if (options) {
			std::string name = options->get_or<std::string>("kind", "");
			if (name == "error") kind = ::Notices::Kind::Error;
			else if (name == "tip") kind = ::Notices::Kind::Tip;
			else if (name == "done") kind = ::Notices::Kind::Done;
			else if (!name.empty() && name != "plain") Extensions::logOnce("Notice", "kind is error, tip, done or nothing");
			tip = options->get_or<std::string>("id", "").substr(0, 64);
		}
		return ::Notices::Add(ext->folderKey, ext->name, kind, text, glfwGetTime(), tip) != 0;
	}

	// ImGUI_Toggle(label, value, options), options is optional:
	// { look = "checkbox" or "chip", size = "small", icon = a texture from LoadTexture,
	//   note = muted text after a chip's name }
	bool ImGUI_Toggle(const std::string& label, bool currentValue, sol::optional<sol::table> table)
	{
		bool value = currentValue;
		if (!inMenu("ImGUI_Toggle")) return value;
		Controls::ToggleOptions options;
		if (table) {
			std::string look = table->get_or<std::string>("look", "");
			options.checkbox = look == "checkbox";
			options.chip = look == "chip";
			options.note = table->get_or<std::string>("note", "");
			options.compact = table->get_or<std::string>("size", "") == "small";
			options.icon = iconOf("ImGUI_Toggle", *table);
		}
		options.dotted = dotted(label);
		used(label, Controls::Toggle(label.c_str(), &value, options));
		return value;
	}

	// the number piece, as every number in the menu
	static double drawnNumber(const std::string& label, double value, Controls::NumberOptions options) {
		value = Controls::Fit(value, options);
		options.dotted = dotted(label);
		used(label, Controls::Number(label.c_str(), value, options).changed);
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
	// text greyed), backToGame = false (keeps the focus after Enter or Esc, for the next box),
	// look = "search" (a search bar over the whole row, the label not shown), with search:
	// tabs = { "Everywhere", "Here" } and tab = the open one's name, a segmented row at
	// the bar's end, and then the open tab's name comes back second }. Without tabs,
	// "enter" or "escape" comes back second on the frame the typing ends with that key.
	sol::variadic_results ImGUI_InputText(const std::string& label, const std::string& currentText, sol::optional<sol::table> table,
		sol::this_state state)
	{
		std::string value = currentText;
		sol::variadic_results out;
		if (!inMenu("ImGUI_InputText")) {
			out.push_back(sol::make_object(state, value));
			return out;
		}
		Controls::TextOptions options;
		int tab = 0;
		if (table) {
			options.backToGame = table->get_or("backToGame", true);
			options.hint = table->get_or<std::string>("hint", "");
			if (auto fallback = table->get<sol::optional<std::string>>("default")) options.defaultValue = *fallback;
			options.clear = table->get_or("clear", false);
			options.confirm = table->get_or("confirm", false);
			options.taken = table->get_or("taken", false);
			options.search = table->get_or<std::string>("look", "") == "search";
			if (options.search) {
				if (auto tabs = table->get<sol::optional<sol::table>>("tabs")) {
					std::string open = table->get_or<std::string>("tab", "");
					for (size_t i = 1; i <= Extensions::MaxTabs; i++) {
						sol::optional<std::string> name = (*tabs)[i];
						if (!name) break;
						if (*name == open) tab = (int)options.tabs.size();
						options.tabs.push_back(*name);
					}
					if (!options.tabs.empty()) options.tab = &tab;
				}
			}
		}
		bool sent = false, escaped = false;
		options.sent = &sent;
		options.escaped = &escaped;
		options.dotted = dotted(label);
		used(label, Controls::Text(label.c_str(), value, options));
		out.push_back(sol::make_object(state, value));
		if (options.tab) out.push_back(sol::make_object(state, options.tabs[tab]));
		else if (sent) out.push_back(sol::make_object(state, "enter"));
		else if (escaped) out.push_back(sol::make_object(state, "escape"));
		return out;
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

	// { map = true, menu = false }: which of the screens the overlay knows are up
	sol::table GetScreens(sol::this_state state) {
		return screensTable(Screens::now(), state);
	}

	sol::table screensTable(const Screens::Up& up, sol::state_view lua) {
		sol::table screens = lua.create_table();
		for (const Screens::Known& known : Screens::list) screens[known.name] = up[known.screen];
		return screens;
	}

	sol::object screenPictureTable(const std::string& screen, int width, const Screens::Up& up, sol::state_view lua) {
		auto known = std::find_if(Screens::list.begin(), Screens::list.end(), [&](const Screens::Known& k) { return screen == k.name; });
		if (known == Screens::list.end()) {
			Extensions::logOnce("GetScreenPicture", "knows no screen named " + screen);
			return sol::make_object(lua, sol::lua_nil);
		}
		if (!known->picture) {
			Extensions::logOnce("GetScreenPicture", "gives no picture of the " + screen);
			return sol::make_object(lua, sol::lua_nil);
		}
		ScreenPictures::Refused refused = ScreenPictures::Refused::None;
		ScreenPictures::Picture picture = ScreenPictures::get(known->screen, width, up, Extensions::currentExtension, &refused);
		if (refused == ScreenPictures::Refused::TooManyOwn)
			Extensions::logOnce("GetScreenPicture", "gets at most " + std::to_string(ScreenPictures::MaxPerAsker) + " pictures at once");
		else if (refused == ScreenPictures::Refused::TooMany)
			Extensions::logOnce("GetScreenPicture", "got none, the extensions have " + std::to_string(ScreenPictures::MaxPictures) + " pictures at once");
		if (picture.texture == 0) return sol::make_object(lua, sol::lua_nil);
		return sol::make_object(lua, lua.create_table_with(
			"texture", picture.texture,
			"width", picture.width,
			"height", picture.height
		));
	}

	sol::object GetScreenPicture(const std::string& screen, sol::optional<sol::table> options, sol::this_state state) {
		int width = ScreenPictures::DefaultWidth;
		if (options) width = options->get_or("width", width);
		return screenPictureTable(screen, width, Screens::now(), state);
	}

	// RegisterKeybind(key, function, modifiers, label), modifiers like
	// KeybindModifiers.Alt, label names the key's row on the Keys tab.
	// The key needs Alt or Ctrl in its modifiers, others are refused, and so are
	// the ones Windows keeps (Alt+Tab, Alt+F4...) and the menu's Alt+`. It works while
	// the game is in front, with the menu open or closed.
	// The id for DeleteKeybind. A key the player hasn't allowed waits and is
	// asked for, and is bound once it's allowed. Empty when the key can't be
	// bound, the log says why. The keybind belongs to the extension and goes
	// with it (Extensions::unload). The key is the default, the player can
	// pick another one. A key another extension already has starts unset here.
	std::string RegisterKeybind(int glfwKey, sol::protected_function callbackFunction, sol::optional<int> modifiers,
		sol::optional<std::string> label) {
		// remember which extension made it, so GetSetting works inside the callback
		Extensions::Extension* owner = Extensions::currentExtension;
		if (!owner) return "";
		Keybindings::KeybindModifiers held = modifiers.value_or(Keybindings::KeybindModifiers_None);
		std::string name = Keybindings::KeyName(glfwKey, held);
		std::string whyNot = Keybindings::WhyNotPickable(glfwKey, held);
		for (const auto& key : owner->keys) {
			if (key.authorName == name) whyNot = name + " is taken already";
		}
		if (owner->keys.size() >= Extensions::MaxKeys) {
			Extensions::logOnce("RegisterKeybind", "has " + std::to_string(Extensions::MaxKeys) + " keys, no more are bound");
			return "";
		}
		if (!whyNot.empty()) {
			std::cerr << "[" << owner->name << "] RegisterKeybind: " << whyNot << std::endl;
			return "";
		}

		Extensions::Extension::HeldKey key;
		key.id = Helpers::GenerateUUID();
		key.label = label.value_or("");
		key.authorKey = glfwKey;
		key.authorModifiers = held;
		key.authorName = name;
		KeyChoices::start(*owner, key);
		key.callback = [owner, callbackFunction]() {
			Extensions::markUsed(*owner, Extensions::ExtensionPermissionTypes::Keybinds);
			Extensions::runForExtension(owner, [owner, &callbackFunction]() {
				PlayerAction action(owner);
				callLua(callbackFunction, "key");
				});
			};
		std::string id = key.id;
		owner->keys.push_back(std::move(key));
		Extensions::bindAllowedKeys(*owner);

		auto it = std::find_if(owner->keys.begin(), owner->keys.end(), [&](const auto& each) { return each.id == id; });
		if (it == owner->keys.end()) return "";
		if (it->bound.empty() && it->key != 0) {
			std::cout << "[" << owner->name << "] " << it->name << " waits until the player allows it" << std::endl;
			Asks::askKeys(*owner, it->name);
		}
		return id;
	}

	// only the extension's own keybinds, bound or waiting
	bool DeleteKeybind(const std::string& id) {
		Extensions::Extension* owner = Extensions::currentExtension;
		if (!owner) return false;
		auto it = std::find_if(owner->keys.begin(), owner->keys.end(), [&](const auto& each) { return each.id == id; });
		if (it == owner->keys.end()) return false;
		std::string bound = it->bound;
		std::string name = it->name;
		owner->keys.erase(it);
		if (!bound.empty()) Keybindings::DeleteKeybind(bound);
		else Asks::forgetKey(*owner, name);
		return true;
	}

	// why a label can't be a menu item's, empty when it can
	std::string WhyNotMenuLabel(const std::string& label) {
		if (label.find_first_not_of(' ') == std::string::npos) return "the label is empty";
		for (unsigned char c : label) {
			if (c < 0x20 || c == 0x7F) return "the label has a line break, tab or other control character";
		}
		// ImGui reads ## as the start of an id
		if (label.find("##") != std::string::npos) return "the label has ##";
		size_t characters = utf8Characters(label);
		if (characters > Extensions::MaxMenuItemLabel) {
			return "the label has " + std::to_string(characters) + " characters, " +
				std::to_string(Extensions::MaxMenuItemLabel) + " at most";
		}
		return "";
	}

	// AddMenuItem(label, function): an item under the line in the extension's
	// own ⋯ menu in the page list, the function runs when the player picks it.
	// Up to 3, the label up to 24 characters. The id for RemoveMenuItem, empty
	// when it's refused, the log says why. The items go with the extension.
	std::string AddMenuItem(const std::string& label, sol::protected_function callbackFunction) {
		Extensions::Extension* owner = Extensions::currentExtension;
		if (!owner) return "";
		std::string whyNot = WhyNotMenuLabel(label);
		if (whyNot.empty() && !callbackFunction.valid()) whyNot = "needs a function after the label";
		if (whyNot.empty() && owner->menuItems.size() >= Extensions::MaxMenuItems) {
			whyNot = "has " + std::to_string(Extensions::MaxMenuItems) + " menu items, no more are added";
		}
		if (!whyNot.empty()) {
			Extensions::logOnce("AddMenuItem", whyNot);
			return "";
		}
		Extensions::Extension::MenuItem item;
		item.id = Helpers::GenerateUUID();
		item.label = label;
		item.callback = [owner, callbackFunction]() {
			// stays false when the guard stopped it and nothing ran
			bool worked = false;
			Extensions::runForExtension(owner, [owner, &callbackFunction, &worked]() {
				PlayerAction action(owner);
				worked = callLua(callbackFunction, "menu item");
				});
			return worked;
			};
		owner->menuItems.push_back(std::move(item));
		return owner->menuItems.back().id;
	}

	// only the extension's own items
	bool RemoveMenuItem(const std::string& id) {
		Extensions::Extension* owner = Extensions::currentExtension;
		if (!owner) return false;
		auto& items = owner->menuItems;
		auto it = std::find_if(items.begin(), items.end(), [&](const auto& each) { return each.id == id; });
		if (it == items.end()) return false;
		items.erase(it);
		return true;
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

	// what the extension's main.lua lets it reach
	static Net::Sites sitesFor(const Extensions::Extension* ext) {
		Net::Sites sites;
		if (ext && (!ext->listRead || ext->sitesOnly)) {
			sites.only = true;
			if (ext->listRead) sites.names = ext->sites;
		}
		return sites;
	}

	// an extension reaches the internet only, and only its sites when it lists
	// them; one log line per function
	static void logRefused(const char* function, const std::string& place, bool notListed) {
		if (place.empty()) return;
		const Extensions::Extension* ext = Extensions::currentExtension;
		if (notListed && ext && !ext->listRead) {
			Extensions::logOnce(function, "can't reach " + place + " while main.lua is still loading: its sites aren't read yet");
			return;
		}
		if (notListed) {
			std::string sites = ext ? Helpers::joined(ext->sites) : "";
			Extensions::logOnce(function, "can't reach " + place + ": its main.lua lists only " + (sites.empty() ? "no sites" : sites));
			return;
		}
		Extensions::logOnce(function, "can't reach " + place + ": extensions reach only the internet, not this PC or the home network");
	}

	// what an extension's request is held to, waitMs for the whole of it
	static Net::Limits limitsFor(const Extensions::Extension* ext, int waitMs) {
		Net::Limits limits;
		limits.publicOnly = true;
		limits.sites = sitesFor(ext);
		limits.maxBody = NetMaxBody;
		limits.deadlineMs = waitMs;
		return limits;
	}

	static void logLimits(const char* function, const Net::Response& response) {
		if (response.tooBig) Extensions::logOnce(function, "got an answer over " + std::to_string(NetMaxBody / (1024 * 1024)) + " MB, it's dropped");
		if (response.timedOut) Extensions::logOnce(function, "waited too long for an answer");
	}

	sol::object NetGet(const std::string& url, sol::this_state state)
	{
		sol::state_view lua(state);
		Net::Response response;

		if (!Net::get(url, response, false, nullptr, nullptr, limitsFor(Extensions::currentExtension, NetWaitMs))) {
			logRefused("Net_Get", response.refused, response.notListed);
			logLimits("Net_Get", response);
			return sol::nil;
		}

		sol::table result = lua.create_table();
		result["status"] = response.status;
		result["body"] = response.body;

		return result;
	}

	sol::object NetPost(const std::string& url, const std::string& data, sol::this_state state)
	{
		sol::state_view lua(state);
		Net::Response response;

		if (!Net::post(url, data, response, false, limitsFor(Extensions::currentExtension, NetWaitMs))) {
			logRefused("Net_Post", response.refused, response.notListed);
			logLimits("Net_Post", response);
			return sol::nil;
		}

		sol::table result = lua.create_table();
		result["status"] = response.status;
		result["body"] = response.body;

		return result;
	}

	// What Net_Start and the file windows wait on: each gives its answer to
	// onDone on the first turn it's done, or to the Result call when it has none.
	// Request has id, owner, onDone, hasOnDone and done().
	template <class Request>
	struct Waiting {
		std::vector<std::unique_ptr<Request>> list;
		int nextId = 1;

		int of(const Extensions::Extension* owner) const {
			return (int)std::count_if(list.begin(), list.end(), [owner](const auto& each) { return each->owner == owner; });
		}
		int add(std::unique_ptr<Request> request, Extensions::Extension* owner, const sol::object& onDone) {
			request->id = nextId++;
			request->owner = owner;
			if (onDone != sol::lua_nil) {
				request->onDone = onDone.as<sol::protected_function>();
				request->hasOnDone = true;
			}
			int id = request->id;
			list.push_back(std::move(request));
			return id;
		}
		// the owner's request without onDone, once it's done
		std::unique_ptr<Request> takeAnswer(int id, const Extensions::Extension* owner) {
			auto it = std::find_if(list.begin(), list.end(), [&](const auto& each) { return each->id == id && each->owner == owner; });
			if (it == list.end() || (*it)->hasOnDone || !(*it)->done()) return nullptr;
			std::unique_ptr<Request> request = std::move(*it);
			list.erase(it);
			return request;
		}
		// the ones with onDone that are done
		std::vector<std::unique_ptr<Request>> takeDue() {
			std::vector<std::unique_ptr<Request>> due;
			for (auto it = list.begin(); it != list.end();) {
				if ((*it)->hasOnDone && (*it)->done()) {
					due.push_back(std::move(*it));
					it = list.erase(it);
				}
				else ++it;
			}
			return due;
		}
		void forget(const Extensions::Extension* owner) {
			list.erase(std::remove_if(list.begin(), list.end(), [owner](const auto& each) { return each->owner == owner; }), list.end());
		}
	};

	// One Net_Start. The thread writes only into answer, the draw thread reads
	// it once done is set.
	struct NetRequest {
		struct Answer {
			Net::Stopper stopper;
			Net::Response response;
			bool ok = false;
			std::atomic<bool> done{ false };
		};
		int id = 0;
		Extensions::Extension* owner = nullptr;
		sol::protected_function onDone;
		bool hasOnDone = false;
		std::shared_ptr<Answer> answer = std::make_shared<Answer>();
		std::thread thread;
		bool done() const { return answer->done; }

		~NetRequest() {
			answer->stopper.stop();
			if (thread.joinable()) thread.join();
		}
	};
	static Waiting<NetRequest> netRequests;

	int netRequestsOf(const Extensions::Extension* owner) {
		return netRequests.of(owner);
	}

	static sol::table answerTable(const NetRequest& request, sol::state_view lua) {
		const Net::Response& response = request.answer->response;
		sol::table result = lua.create_table();
		if (request.answer->ok) {
			result["status"] = response.status;
			result["body"] = response.body;
		}
		else if (response.timedOut) result["error"] = "timeout";
		else if (response.tooBig) result["error"] = "too_big";
		else if (!response.refused.empty()) result["error"] = "refused";
		else result["error"] = "failed";
		return result;
	}

	sol::object NetStart(const std::string& url, sol::object options, sol::object onDone, sol::this_state state)
	{
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext) return sol::nil;
		// Net_Start(url, onDone) works too
		if (options.is<sol::protected_function>() && onDone == sol::lua_nil) std::swap(options, onDone);

		std::string method = "GET";
		std::string body;
		if (options.is<sol::table>()) {
			sol::table table = options;
			method = table.get_or("method", method);
			body = table.get_or("body", body);
		}
		else if (options != sol::lua_nil) {
			Extensions::logOnce("Net_Start", "takes a table as options, like { method = \"POST\", body = \"...\" }");
			return sol::nil;
		}
		if (method != "GET" && method != "POST") {
			Extensions::logOnce("Net_Start", "knows the methods GET and POST, not " + method);
			return sol::nil;
		}
		if (onDone != sol::lua_nil && !onDone.is<sol::protected_function>()) {
			Extensions::logOnce("Net_Start", "takes a function as onDone");
			return sol::nil;
		}
		if (netRequestsOf(ext) >= NetMaxWaiting) {
			Extensions::logOnce("Net_Start", "has " + std::to_string(NetMaxWaiting) + " requests waiting or not collected, no more start");
			return sol::nil;
		}

		auto request = std::make_unique<NetRequest>();
		std::shared_ptr<NetRequest::Answer> answer = request->answer;
		Net::Limits limits = limitsFor(ext, NetStartMs);
		request->thread = std::thread([answer, url, method, body, limits]() {
			answer->ok = method == "POST"
				? Net::post(url, body, answer->response, false, limits, &answer->stopper)
				: Net::get(url, answer->response, false, nullptr, &answer->stopper, limits);
			answer->done = true;
		});
		return sol::make_object(state, netRequests.add(std::move(request), ext, onDone));
	}

	// the request is done: its thread is joined and what went wrong logged, as its extension
	static void collect(NetRequest& request) {
		request.thread.join();
		logRefused("Net_Start", request.answer->response.refused, request.answer->response.notListed);
		logLimits("Net_Start", request.answer->response);
	}

	sol::object NetResult(int id, sol::this_state state)
	{
		std::unique_ptr<NetRequest> request = netRequests.takeAnswer(id, Extensions::currentExtension);
		if (!request) return sol::nil;
		collect(*request);
		return answerTable(*request, sol::state_view(state));
	}

	// The picture at url for the running extension, held to its sites. Logged
	// once when it has as many as it may keep.
	static WebImages::Image webImage(const char* function, const std::string& url) {
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext) return {};
		Net::Limits limits = limitsFor(ext, NetStartMs);
		limits.maxBody = ImageMaxBody;
		WebImages::Image image = WebImages::get(ext, url, limits, function);
		if (image.state == WebImages::State::Refused && WebImages::kept(ext) >= WebImages::MaxKept) {
			Extensions::logOnce(function, "has " + std::to_string(WebImages::MaxKept) + " web images, no more are loaded");
		}
		return image;
	}

	// GetWebImage(url): the handle for DrawImage and ImGUI_Image, and the size;
	// nil while it downloads, false when it couldn't be had. Asked every frame
	// it's drawn, it's freed 30 s after the last ask.
	sol::variadic_results GetWebImage(const std::string& url, sol::this_state state) {
		sol::variadic_results out;
		WebImages::Image image = webImage("GetWebImage", url);
		if (image.state == WebImages::State::Ready) {
			out.push_back(sol::make_object(state, image.handle));
			out.push_back(sol::make_object(state, image.width));
			out.push_back(sol::make_object(state, image.height));
		}
		else if (image.state == WebImages::State::Loading) out.push_back(sol::make_object(state, sol::lua_nil));
		else out.push_back(sol::make_object(state, false));
		return out;
	}

	// downloaded web images become textures, on the draw thread
	static void updateImageDownloads() {
		WebImages::endFrame([](const WebImages::Done& done) {
			Extensions::Extension* owner = (Extensions::Extension*)done.owner;
			Extensions::runForExtension(owner, [&]() {
				const char* function = done.function.c_str();
				logRefused(function, done.response.refused, done.response.notListed);
				if (done.response.tooBig) Extensions::logOnce(function, "got a picture over " + std::to_string(ImageMaxBody / (1024 * 1024)) + " MB, it's dropped");
				if (done.response.timedOut) Extensions::logOnce(function, "waited too long for a picture");
			});
		});
	}

	void UpdateNetRequests()
	{
		updateImageDownloads();
		std::vector<std::unique_ptr<NetRequest>> due = netRequests.takeDue();
		// a callback can start new requests, and its extension can be unloaded
		for (auto& request : due) {
			Extensions::Extension* owner = request->owner;
			Extensions::runForExtension(owner, [&]() {
				collect(*request);
				// Network Access switched off while it waited
				if (!owner->extensionPermissions.count(Permission::NetworkAccess) || !owner->luaState) return;
				Extensions::markUsed(*owner, Permission::NetworkAccess);
				sol::table result;
				{
					Extensions::HostAllocations host(owner->memory.get());
					result = answerTable(*request, sol::state_view(*owner->luaState));
				}
				auto called = request->onDone(result);
				if (!called.valid()) {
					sol::error err = called;
					std::cerr << "[" << owner->name << "] Net_Start onDone: " << err.what() << std::endl;
				}
			});
		}
	}

	struct DialogRequest {
		int id = 0;
		Extensions::Extension* owner = nullptr;
		bool save = false;
		sol::protected_function onDone;
		bool hasOnDone = false;
		std::shared_ptr<FileDialogs::Answer> answer;
		bool done() const { return answer->done; }
	};
	static Waiting<DialogRequest> dialogRequests;

	static sol::table dialogTable(const FileDialogs::Answer& answer, bool save, sol::state_view lua) {
		sol::table result = lua.create_table();
		if (!answer.error.empty()) result["error"] = answer.error;
		else {
			result["name"] = answer.name;
			if (!save) result["data"] = answer.data;
		}
		return result;
	}

	// what both dialogs check before one opens
	static sol::object startDialog(const char* function, bool save, const std::string& name, const std::string& data,
		sol::object filter, sol::object onDone, sol::this_state state) {
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext) return sol::nil;
		if (ext->playerAction == 0) {
			Extensions::logOnce(function, "opens only from the function of its button or key");
			return sol::nil;
		}
		std::optional<FileDialogs::Filter> types;
		if (filter.is<sol::table>()) {
			sol::table table = filter;
			sol::optional<std::string> label = table[1];
			sol::optional<std::string> pattern = table[2];
			if (label && pattern) types = FileDialogs::filterFrom(*label, *pattern);
			if (!types) {
				Extensions::logOnce(function, "takes a filter like { \"JSON\", \"*.json\" }");
				return sol::nil;
			}
		}
		else if (filter != sol::lua_nil) {
			Extensions::logOnce(function, "takes a filter like { \"JSON\", \"*.json\" }");
			return sol::nil;
		}
		if (onDone != sol::lua_nil && !onDone.is<sol::protected_function>()) {
			Extensions::logOnce(function, "takes a function as onDone");
			return sol::nil;
		}
		if (data.size() > FileDialogs::MaxSize) {
			Extensions::logOnce(function, "saves at most 64 MB");
			return sol::nil;
		}
		auto answer = FileDialogs::start(Overlay::Window ? glfwGetWin32Window(Overlay::Window) : nullptr, save, name, data, types);
		if (!answer) {
			std::cerr << "[" << ext->name << "] " << function << ": another file window is open" << std::endl;
			return sol::nil;
		}
		auto request = std::make_unique<DialogRequest>();
		request->save = save;
		request->answer = answer;
		return sol::make_object(state, dialogRequests.add(std::move(request), ext, onDone));
	}

	sol::object OpenFileDialog(sol::object filter, sol::object onDone, sol::this_state state)
	{
		return startDialog("OpenFileDialog", false, "", "", filter, onDone, state);
	}

	sol::object SaveFileDialog(const std::string& name, const std::string& data, sol::object filter, sol::object onDone,
		sol::this_state state)
	{
		return startDialog("SaveFileDialog", true, name, data, filter, onDone, state);
	}

	sol::object FileDialogResult(int id, sol::this_state state)
	{
		std::unique_ptr<DialogRequest> request = dialogRequests.takeAnswer(id, Extensions::currentExtension);
		if (!request) return sol::nil;
		return dialogTable(*request->answer, request->save, sol::state_view(state));
	}

	void UpdateAnswers()
	{
		UpdateFileDialogs();
		UpdateNetRequests();
		UpdateMarks();
		UpdateSockets();
	}

	void UpdateFileDialogs()
	{
		FileDialogs::update();
		std::vector<std::unique_ptr<DialogRequest>> due = dialogRequests.takeDue();
		for (auto& request : due) {
			Extensions::Extension* owner = request->owner;
			Extensions::runForExtension(owner, [&]() {
				if (!owner->luaState) return;
				sol::table result;
				{
					Extensions::HostAllocations host(owner->memory.get());
					result = dialogTable(*request->answer, request->save, sol::state_view(*owner->luaState));
				}
				auto called = request->onDone(result);
				if (!called.valid()) {
					sol::error err = called;
					std::cerr << "[" << owner->name << "] file window onDone: " << err.what() << std::endl;
				}
			});
		}
	}

	void forgetFileDialogs(Extensions::Extension* owner)
	{
		dialogRequests.forget(owner);
	}

	void forgetNetRequests(Extensions::Extension* owner)
	{
		netRequests.forget(owner);
		WebImages::forget(owner);
	}

	// Where the in-game map is looking, or nil when it isn't open. Positions are
	// in overlay pixels, the same ones DrawImage uses. lat/lng is the map
	// position at centerX, centerY. moving is true while the map moves, and
	// after it stops until it's lined up exactly. groupId and floorId are the
	// layer shown, layerFrom says who chose it: "game" (read off the game's screen),
	// "picked" (the overlay's floor picker) or "guess" (from the map's picture). ui is the game's
	// own UI as boxes to keep off, and the overlay's floor picker while it's
	// there. panelLeft is where a panel on the right starts, only while one is open.
	// the overlay window's corner on the screen, 0, 0 without a window
	static POINT overlayCorner() {
		int x = 0, y = 0;
		if (Overlay::Window) glfwGetWindowPos(Overlay::Window, &x, &y);
		return { x, y };
	}

	// the game in overlay pixels, and its map UI and the floor picker in them
	static RECT gameOnOverlay(const MapTracking::MapView& view) {
		POINT window = overlayCorner();
		RECT game = view.gameRect;
		OffsetRect(&game, -window.x, -window.y);
		return game;
	}

	static std::vector<RECT> mapUiBoxes(const MapTracking::MapView& view, const RECT& game) {
		std::vector<RECT> boxes = GameUi::Boxes(game, view.panel);
		RECT picker;
		if (Overlay::PickerBox(picker)) boxes.push_back(picker);
		return boxes;
	}

	// the open panel for GetMapView: none, unsure, or its kind
	static const char* panelName(const GameUi::Panel& panel) {
		if (panel.seen == GameUi::Seen::Unsure) return "unsure";
		if (panel.seen != GameUi::Seen::Read) return "none";
		switch (panel.kind) {
			case GameUi::PanelKind::RegionList: return "regions";
			case GameUi::PanelKind::Info: return "info";
			case GameUi::PanelKind::Settings: return "settings";
			default: return "none";
		}
	}

	static const char* seenName(GameUi::Seen seen) {
		return seen == GameUi::Seen::Read ? "shown" : seen == GameUi::Seen::None ? "gone" : "unsure";
	}

	sol::object GetMapView(sol::this_state state) {
		return mapViewTable(MapTracking::GetView(), state);
	}

	sol::object mapViewTable(const MapTracking::MapView& view, sol::state_view lua) {
		if (!view.visible) return sol::make_object(lua, sol::lua_nil);

		// everything below is placed from the game
		RECT game = gameOnOverlay(view);
		float left = (float)game.left;
		float top = (float)game.top;
		float right = (float)game.right;
		float bottom = (float)game.bottom;

		sol::table ui = lua.create_table();
		auto addBox = [&](const RECT& box) {
			ui.add(lua.create_table_with(
				"left", (float)box.left,
				"top", (float)box.top,
				"right", (float)box.right,
				"bottom", (float)box.bottom));
		};
		for (const RECT& box : mapUiBoxes(view, game)) addBox(box);
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
			"ui", ui,
			"panel", panelName(view.panel),
			"gear", seenName(view.gear)
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

	sol::variadic_results FreeSpace(float y0, float y1, sol::this_state state) {
		sol::variadic_results results;
		MapTracking::MapView view = MapTracking::GetView();
		if (!view.visible) {
			results.push_back(sol::make_object(state, sol::lua_nil));
			return results;
		}
		RECT game = gameOnOverlay(view);
		float from = 0, to = 0;
		GameUi::FreeSpan((float)game.left, (float)game.right, mapUiBoxes(view, game), y0, y1, from, to);
		results.push_back(sol::make_object(state, from));
		results.push_back(sol::make_object(state, to));
		return results;
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
	sol::table GetPlayer(sol::this_state state) {
		return playerTable(MiniMap::GetPlayer(), state);
	}

	sol::table playerTable(const MiniMap::Spot& spot, sol::state_view lua) {
		POINT window = overlayCorner();
		float left = (float)(spot.square.left - window.x);
		float top = (float)(spot.square.top - window.y);
		float right = (float)(spot.square.right - window.x);
		float bottom = (float)(spot.square.bottom - window.y);

		// Left to bottom is the square around the game's minimap on the overlay.
		// The minimap is the circle that just fits in it.
		return lua.create_table_with(
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
		);
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
	void DrawImage(double image, float x0, float y0, float x1, float y1, sol::optional<float> alpha) {
		if (image == 0 || !inFrame("DrawImage")) return;
		GLuint textureID = textureOf("DrawImage", image);
		if (textureID == 0) return;
		int a = (int)(alpha.value_or(1.0f) * 255.0f);
		ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)textureID, ImVec2(x0, y0), ImVec2(x1, y1),
			ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
	}

	// Clicks in the box go to the overlay from the next frame on, while one of
	// the known screens is up. Nothing while the cursor is over the menu or another
	// of the overlay's windows, so they always stay usable.
	std::tuple<bool, bool> ClickArea(float x0, float y0, float x1, float y1) {
		if (!inFrame("ClickArea")) return { false, false };
		Overlay::ClaimMouse(x0, y0, x1, y1, Extensions::currentExtension ? Extensions::currentExtension->name : std::string());
		if (!Screens::now().any()) return { false, false };
		if (ImGui::GetIO().WantCaptureMouse) return { false, false };
		bool hovered = ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y1), false);
		return { hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left), hovered };
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

	// What the socket's thread and the draw thread share. The thread connects,
	// sends what's queued and closes; the draw thread queues and takes.
	struct SocketLink {
		Net::WebSocket socket;
		Net::Stopper stopper;
		std::mutex mutex;
		std::condition_variable wake;
		std::deque<std::string> outgoing;
		bool stop = false;
		bool hasOnMessage = false;
		std::atomic<bool> open{ false };
		// stays true once it opened, onOpen is called even when it closed before the next frame
		std::atomic<bool> opened{ false };
		std::atomic<bool> finished{ false };
		// set before finished
		std::string reason;

		void end() {
			{
				std::lock_guard<std::mutex> lock(mutex);
				stop = true;
			}
			stopper.stop();
			wake.notify_all();
		}
	};

	struct SocketEntry {
		std::shared_ptr<SocketLink> link;
		Extensions::Extension* owner = nullptr;
		sol::protected_function onOpen, onMessage, onClose;
		bool openSaid = false;
		std::thread thread;

		~SocketEntry() {
			link->end();
			if (thread.joinable()) thread.join();
		}
	};
	static std::vector<std::unique_ptr<SocketEntry>> sockets;

	Socket::~Socket() {
		if (link) link->end();
	}

	int socketsOf(const Extensions::Extension* owner) {
		return (int)std::count_if(sockets.begin(), sockets.end(), [owner](const auto& each) { return each->owner == owner; });
	}

	static void runSocket(std::shared_ptr<SocketLink> link, std::string url, Net::Limits limits) {
		Net::WebSocket& socket = link->socket;
		if (!socket.connect(url, false, limits, &link->stopper)) {
			if (!socket.refusedPlace().empty()) link->reason = "refused";
			else if (socket.connectTimedOut()) link->reason = "timeout";
			else link->reason = "failed";
			link->finished = true;
			return;
		}
		link->opened = true;
		link->open = true;
		for (;;) {
			std::string message;
			{
				std::unique_lock<std::mutex> lock(link->mutex);
				// also wakes every 100 ms to see if the receive side ended
				link->wake.wait_for(lock, std::chrono::milliseconds(100), [&]() { return link->stop || !link->outgoing.empty(); });
				if (link->stop || !socket.isConnected()) break;
				if (link->outgoing.empty()) continue;
				message = std::move(link->outgoing.front());
				link->outgoing.pop_front();
			}
			if (!socket.send(message)) break;
		}
		link->reason = socket.overflowed() ? "too_many_messages" : "closed";
		socket.close();
		link->open = false;
		link->finished = true;
	}

	static void callHandler(Extensions::Extension* owner, sol::protected_function& handler, const char* name, sol::object argument = sol::lua_nil) {
		if (!handler.valid()) return;
		auto called = argument == sol::lua_nil ? handler() : handler(argument);
		if (!called.valid()) {
			sol::error err = called;
			std::cerr << "[" << owner->name << "] WebSocket " << name << ": " << err.what() << std::endl;
		}
	}

	static void dropHandlers(SocketEntry& entry) {
		entry.onOpen = entry.onMessage = entry.onClose = sol::protected_function();
		entry.owner = nullptr;
	}

	bool WebSocketConnect(Socket& socket, const std::string& url, sol::optional<sol::table> handlers)
	{
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext) return false;
		if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
			Extensions::logOnce("WebSocket.connect", "takes a ws:// or wss:// address");
			return false;
		}
		if (socket.link) {
			socket.link->end();
			for (auto& entry : sockets) {
				if (entry->link == socket.link) dropHandlers(*entry);
			}
		}
		socket.link.reset();
		if (socketsOf(ext) >= SocketMax) {
			Extensions::logOnce("WebSocket.connect", "has " + std::to_string(SocketMax) + " sockets open or connecting, no more connect");
			return false;
		}

		auto entry = std::make_unique<SocketEntry>();
		entry->owner = ext;
		if (handlers) {
			sol::table table = *handlers;
			auto take = [&](const char* name, sol::protected_function& into) {
				sol::object value = table[name];
				if (value.is<sol::protected_function>()) into = value.as<sol::protected_function>();
			};
			take("onOpen", entry->onOpen);
			take("onMessage", entry->onMessage);
			take("onClose", entry->onClose);
		}
		auto link = std::make_shared<SocketLink>();
		link->hasOnMessage = entry->onMessage.valid();
		link->socket.setMaxQueued(SocketMaxQueued);
		entry->link = link;
		Net::Limits limits = limitsFor(ext, NetStartMs);
		limits.maxBody = 0;
		entry->thread = std::thread(runSocket, link, url, limits);
		socket.link = link;
		sockets.push_back(std::move(entry));
		return true;
	}

	bool WebSocketSend(Socket& socket, const std::string& message)
	{
		if (!socket.link || socket.link->finished) return false;
		{
			std::lock_guard<std::mutex> lock(socket.link->mutex);
			if (socket.link->stop) return false;
			if (socket.link->outgoing.size() >= SocketMaxQueued) {
				Extensions::logOnce("WebSocket.send", "has " + std::to_string(SocketMaxQueued) + " messages waiting to go, this one is dropped");
				return false;
			}
			socket.link->outgoing.push_back(message);
		}
		socket.link->wake.notify_all();
		return true;
	}

	sol::object WebSocketPoll(Socket& socket, sol::this_state state)
	{
		std::string message;

		if (!socket.link || socket.link->hasOnMessage || !socket.link->socket.poll(message))
			return sol::nil;

		return sol::make_object(state, message);
	}

	sol::object WebSocketPollAll(Socket& socket, sol::this_state state)
	{
		sol::state_view lua(state);
		sol::table result = lua.create_table();
		if (!socket.link || socket.link->hasOnMessage) return result;

		auto messages = socket.link->socket.pollAll();

		int index = 1;
		for (const auto& message : messages)
			result[index++] = message;

		return result;
	}

	bool WebSocketIsConnected(Socket& socket)
	{
		return socket.link && socket.link->open && socket.link->socket.isConnected();
	}

	void WebSocketClose(Socket& socket)
	{
		if (socket.link) socket.link->end();
	}

	void UpdateSockets()
	{
		// by index, a handler may add sockets
		for (size_t i = 0; i < sockets.size(); i++) {
			SocketEntry* entry = sockets[i].get();
			Extensions::Extension* owner = entry->owner;
			if (!owner) continue;
			// Network Access switched off
			if (!owner->extensionPermissions.count(Permission::NetworkAccess) || !owner->luaState) {
				entry->link->end();
				dropHandlers(*entry);
				continue;
			}
			// an open socket is in use
			Extensions::markUsed(*owner, Permission::NetworkAccess);
			bool finished = entry->link->finished;
			Extensions::runForExtension(owner, [&]() {
				if (entry->link->opened && !entry->openSaid) {
					entry->openSaid = true;
					callHandler(owner, entry->onOpen, "onOpen");
				}
				if (entry->onMessage.valid()) {
					for (const auto& message : entry->link->socket.pollAll()) {
						sol::object argument;
						{
							Extensions::HostAllocations host(owner->memory.get());
							argument = sol::make_object(*owner->luaState, message);
						}
						callHandler(owner, entry->onMessage, "onMessage", argument);
						if (!entry->owner) return;
					}
				}
				if (finished) {
					const Net::WebSocket& socket = entry->link->socket;
					logRefused("WebSocket.connect", socket.refusedPlace(), socket.refusedNotListed());
					if (entry->link->reason == "too_many_messages")
						Extensions::logOnce("WebSocket", "had " + std::to_string(SocketMaxQueued) + " messages not polled, the socket is closed");
					sol::object reason;
					{
						Extensions::HostAllocations host(owner->memory.get());
						reason = sol::make_object(*owner->luaState, entry->link->reason);
					}
					callHandler(owner, entry->onClose, "onClose", reason);
				}
			});
			// a handler may have unloaded its extension, which dropped these already
			if (finished && entry->owner) dropHandlers(*entry);
		}
		// ended ones are let go of once their thread is done
		std::vector<std::unique_ptr<SocketEntry>> ended;
		for (auto it = sockets.begin(); it != sockets.end();) {
			if (!(*it)->owner && (*it)->link->finished) {
				ended.push_back(std::move(*it));
				it = sockets.erase(it);
			}
			else ++it;
		}
	}

	void forgetSockets(Extensions::Extension* owner)
	{
		for (auto& entry : sockets) {
			if (entry->owner != owner) continue;
			entry->link->end();
			dropHandlers(*entry);
		}
	}

	double CompareImages(GLuint textureID1, GLuint textureID2, int accuracy) {
		if (!ownsTexture(textureID1) || !ownsTexture(textureID2)) {
			Extensions::logOnce("CompareImages", "compares only textures it loaded itself");
			return 0.0;
		}
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
	// a permission by its fixed name, logged once when there's none by that name
	static const Extensions::PermissionInfo* permissionNamed(const char* function, const std::string& name) {
		const Extensions::PermissionInfo* info = Extensions::findPermission(name);
		if (!info && !Asks::findSetting(name)) Extensions::logOnce(function, "knows no permission named " + name);
		return info;
	}

	sol::object PermissionState(const std::string& name, sol::this_state state) {
		Extensions::Extension* ext = Extensions::currentExtension;
		const Extensions::PermissionInfo* info = permissionNamed("PermissionState", name);
		if (!ext || (!info && !Asks::findSetting(name))) return sol::make_object(state, sol::lua_nil);
		std::string answer = info ? Extensions::permissionState(*ext, info->type) : Asks::settingState(*ext, name);
		return sol::make_object(state, answer);
	}

	void AskForPermission(const std::string& name) {
		Extensions::Extension* ext = Extensions::currentExtension;
		const Extensions::PermissionInfo* info = permissionNamed("AskForPermission", name);
		if (!ext) return;
		if (info && info->playerOnly) Extensions::logOnce("AskForPermission", name + " is only turned on by the player, on its Permissions tab; OpenPermissions(\"" + name + "\") opens it there");
		else if (info) Asks::askPermission(*ext, info->type);
		else Asks::askSetting(*ext, name);
	}

	void OpenPermissions(sol::optional<std::string> name) {
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext || !Extensions::inPhase("OpenPermissions", Extensions::Phase::Menu)) return;
		std::string permission;
		if (name) {
			if (Extensions::findPermission(*name)) permission = *name;
			else Extensions::logOnce("OpenPermissions", "knows no permission named " + *name + ", the tab opens at the top");
		}
		Extensions::openPermissionsTab(*ext, permission);
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
			POINT corner = overlayCorner();
			bool anywhere = ext->extensionPermissions.count(Permission::MouseAnywhere) > 0;
			at = mouseFor(cursor, game, corner, anywhere);
			if (anywhere && at && !mouseFor(cursor, game, corner, false)) Extensions::markUsed(*ext, Permission::MouseAnywhere);
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

	static float optionOr(const sol::optional<sol::table>& options, const char* name, float otherwise) {
		if (!options) return otherwise;
		sol::optional<float> value = (*options)[name];
		return value ? *value : otherwise;
	}

	void GDrawText(const std::string& text, float x, float y, sol::optional<float> size, sol::optional<uint32_t> color,
		sol::optional<sol::table> options)
	{
		if (text.empty() || !inFrame("DrawText"))
			return;
		float fontSize = size.value_or(ImGui::GetFontSize());
		float wrap = std::max(0.0f, optionOr(options, "wrap", 0.0f));
		ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), fontSize, ImVec2(x, y),
			Controls::Color(color.value_or(0xFFFFFFFF)), text.c_str(), nullptr, wrap);
	}

	std::tuple<float, float> TextSize(const std::string& text, sol::optional<float> size, sol::optional<sol::table> options)
	{
		if (!inFrame("TextSize")) return { 0.0f, 0.0f };
		float fontSize = size.value_or(ImGui::GetFontSize());
		float wrap = std::max(0.0f, optionOr(options, "wrap", 0.0f));
		ImVec2 measured = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, wrap, text.c_str());
		return { measured.x, measured.y };
	}

	void DrawRect(float x0, float y0, float x1, float y1, sol::optional<uint32_t> color, sol::optional<sol::table> options)
	{
		if (!inFrame("DrawRect")) return;
		ImU32 colour = Controls::Color(color.value_or(0xFFFFFFFF));
		float rounding = std::max(0.0f, optionOr(options, "rounding", 0.0f));
		float outline = optionOr(options, "outline", 0.0f);
		ImDrawList* draw = ImGui::GetBackgroundDrawList();
		if (outline > 0.0f) draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), colour, rounding, 0, outline);
		else draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), colour, rounding);
	}

	void DrawBadge(float x, float y, float radius, sol::optional<GLuint> icon, sol::optional<sol::table> options)
	{
		if (!inFrame("DrawBadge")) return;
		GLuint texture = icon.value_or(0);
		if (texture != 0 && !ownsTexture(texture)) {
			Extensions::logOnce("DrawBadge", "draws only textures it loaded itself");
			texture = 0;
		}
		Controls::BadgeOptions badge;
		if (options) {
			sol::optional<uint32_t> ring = (*options)["ring"];
			if (ring) badge.ring = *ring;
			badge.faded = options->get_or("faded", false);
			std::string layer = options->get_or<std::string>("layer", "");
			badge.layer = layer == "up" ? 1 : layer == "down" ? -1 : 0;
		}
		Controls::Badge(ImGui::GetBackgroundDrawList(), ImVec2(x, y), radius, (ImTextureID)(intptr_t)texture, badge);
	}

	void DrawBubble(float x, float y, float radius, const std::string& text, sol::optional<sol::table> options)
	{
		if (!inFrame("DrawBubble")) return;
		std::optional<unsigned int> fill;
		if (options) {
			sol::optional<uint32_t> color = (*options)["color"];
			if (color) fill = *color;
		}
		// a count or a sign, not a sentence
		std::string shown = text.substr(0, 8);
		// never half a UTF-8 character
		while (!shown.empty() && shown.size() < text.size() && (text[shown.size()] & 0xC0) == 0x80) shown.pop_back();
		Controls::Bubble(ImGui::GetBackgroundDrawList(), ImVec2(x, y), radius, shown.c_str(), fill);
	}

	void DrawCircle(float x, float y, float radius, sol::optional<uint32_t> color, sol::optional<sol::table> options)
	{
		if (radius <= 0.0f || !inFrame("DrawCircle")) return;
		ImU32 colour = Controls::Color(color.value_or(0xFFFFFFFF));
		float outline = optionOr(options, "outline", 0.0f);
		ImDrawList* draw = ImGui::GetBackgroundDrawList();
		if (outline > 0.0f) draw->AddCircle(ImVec2(x, y), radius, colour, 0, outline);
		else draw->AddCircleFilled(ImVec2(x, y), radius, colour);
	}
	// The map items, all read only from the catalog in memory. Unknown ids give
	// nil. Take what you keep again when GetItems().version changes: a type or a
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

		sol::table table(uint64_t version, const ItemFiles::Status& status, sol::state_view lua) {
			return lua.create_table_with(
				"version", version,
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

	// { version, state, reason, percent, built, checking }: version is 0 while
	// there are none and goes up by one with each new list, the rest is how the
	// download goes, for an extension's own "waiting" text
	sol::table GetItems(sol::this_state state) {
		return Items::table(Items::frameCatalog ? Items::frameCatalog->version : 0, ItemFiles::GetStatus(), state);
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

	// made once per type and kept, valid until GetItems().version changes
	sol::object GetItemIcon(int type, sol::this_state state) {
		GLuint texture = ItemIcons::get(Items::frameCatalog.get(), type);
		if (texture == 0) return sol::lua_nil;
		return sol::make_object(state, texture);
	}

	// GetPointGuide(pointId): what the item files' point says about itself, in
	// the overlay's language: { text, picture, summary, failed, comments }, each
	// comment { author, date, text, original, from, translating, picture, likes },
	// the most liked first. nil while it's on its way, false for a point the
	// item files don't have, or when the extension's sites leave out the
	// guide's. Asked every frame it's shown, the answer can change (a
	// translation comes in, it's fetched again after 10 minutes).
	sol::object GetPointGuide(double pointId, sol::this_state state) {
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext || !ext->listRead) return sol::lua_nil;
		Net::Sites sites = sitesFor(ext);
		if (sites.only) {
			for (const std::string& site : PointGuides::sites()) {
				if (Net::onSites(site, sites)) continue;
				Extensions::logOnce("GetPointGuide", "needs " + Helpers::joined(PointGuides::sites()) + " in its main.lua's sites");
				return sol::make_object(state, false);
			}
		}
		const ItemCatalog::Catalog* catalog = Items::frameCatalog.get();
		if (!catalog) return sol::lua_nil;
		const ItemCatalog::Point* point = catalog->point((int64_t)pointId);
		if (!point) return sol::make_object(state, false);

		PointGuides::Guide guide;
		if (!PointGuides::get({ point->id, point->map, point->type, point->appsample }, Language::current(), guide)) return sol::lua_nil;
		Extensions::markUsed(*ext, Permission::NetworkAccess);
		sol::state_view lua(state);
		sol::table comments = lua.create_table((int)guide.comments.size(), 0);
		for (size_t i = 0; i < guide.comments.size(); i++) {
			const PointGuides::Comment& comment = guide.comments[i];
			sol::table each = lua.create_table();
			each["author"] = comment.author;
			each["date"] = comment.date;
			each["text"] = comment.text;
			each["original"] = comment.original;
			if (!comment.from.empty()) each["from"] = comment.from;
			each["translating"] = comment.translating;
			if (!comment.picture.empty()) each["picture"] = comment.picture;
			each["likes"] = comment.likes;
			comments[i + 1] = each;
		}
		sol::table result = lua.create_table();
		result["text"] = guide.text;
		if (!guide.picture.empty()) result["picture"] = guide.picture;
		if (!guide.summary.empty()) result["summary"] = guide.summary;
		result["failed"] = guide.failed;
		result["comments"] = comments;
		return result;
	}

	// Reading the game's traffic, for what needs it. on is the player's switch
	// in the menu. state is off, cant_listen, waiting_for_login, searching,
	// reading, needs_relog or no_key, and reason says why for cant_listen,
	// needs_relog and no_key (npcap_missing, started_late, unknown_key_id and so
	// on), "" otherwise. Codes, not sentences: the menu says what they mean.
	// Needs no permission, it's only a status. An extension can't turn it on.
	sol::table gameTrafficTable(bool on, const GameTraffic::Status& status, sol::state_view lua)
	{
		return lua.create_table_with(
			"on", on,
			"state", std::string(GameTraffic::StateCode(status)),
			"reason", std::string(GameTraffic::ReasonCode(status)));
	}

	sol::table GetGameTraffic(sol::this_state state)
	{
		return gameTrafficTable(Settings::readGameTraffic, GameTraffic::GetStatus(), state);
	}

	static Permission accountPermission(Accounts::Site site) {
		return site == Accounts::Site::HoyolabMap ? Permission::HoyolabMapAccount : Permission::AppsampleAccount;
	}

	static std::optional<Accounts::Site> siteNamed(const char* function, const std::string& name) {
		std::optional<Accounts::Site> site = Accounts::SiteNamed(name);
		if (!site) Extensions::logOnce(function, "knows no site named " + name + ", only hoyolab_map and appsample");
		return site;
	}

	// account only with the site's permission, so a login as another account
	// isn't seen without it
	sol::table accountTable(Accounts::Site site, const Accounts::Status& status, sol::state_view lua)
	{
		sol::table table = lua.create_table_with(
			"site", std::string(Accounts::SiteName(site)),
			"state", std::string(Accounts::StateName(status.state)),
			"problem", status.problem);
		Extensions::Extension* ext = Extensions::currentExtension;
		if (ext && ext->extensionPermissions.count(accountPermission(site)) && !status.account.empty()) {
			Extensions::markUsed(*ext, accountPermission(site));
			table["account"] = status.account;
			if (!status.name.empty()) table["name"] = status.name;
		}
		return table;
	}

	sol::object GetAccount(const std::string& site, sol::this_state state)
	{
		std::optional<Accounts::Site> named = siteNamed("GetAccount", site);
		if (!named) return sol::make_object(state, sol::lua_nil);
		return accountTable(*named, Accounts::Get(*named), state);
	}

	void LogIn(const std::string& site)
	{
		std::optional<Accounts::Site> named = siteNamed("LogIn", site);
		if (!named || !Extensions::inPhase("LogIn", Extensions::Phase::Menu)) return;
		if (!Extensions::allowed("LogIn", accountPermission(*named))) return;
		Accounts::LogIn(*named);
	}

	void LogOut(const std::string& site)
	{
		std::optional<Accounts::Site> named = siteNamed("LogOut", site);
		if (!named || !Extensions::allowed("LogOut", accountPermission(*named))) return;
		Accounts::LogOut(*named);
	}
	struct MarksRequest {
		int id = 0;
		Extensions::Extension* owner = nullptr;
		sol::protected_function onDone;
	};
	// the jobs with onDone, the rest wait for Marks_Result
	static std::vector<MarksRequest> marksRequests;

	// a list of point ids from Lua, false when it's something else
	static bool pointIds(const sol::object& value, std::vector<int64_t>& out) {
		if (value == sol::lua_nil) return true;
		if (!value.is<sol::table>()) return false;
		sol::table table = value;
		for (size_t i = 1; i <= table.size(); i++) {
			sol::object id = table[i];
			if (id.get_type() != sol::type::number) return false;
			double number = id.as<double>();
			if (number < 1 || number > 9.0e15 || number != std::floor(number)) return false;
			out.push_back((int64_t)number);
		}
		return true;
	}

	static sol::table idList(const std::vector<int64_t>& ids, sol::state_view lua) {
		sol::table table = lua.create_table((int)ids.size(), 0);
		for (size_t i = 0; i < ids.size(); i++) table[i + 1] = ids[i];
		return table;
	}

	static sol::table marksTable(const Marks::Result& result, sol::state_view lua) {
		sol::table table = lua.create_table();
		for (const Marks::SiteResult& site : result.sites) {
			sol::table part = lua.create_table_with("ok", site.ok, "added", site.added, "removed", site.removed);
			if (!site.ok) part["error"] = site.error;
			part["marks"] = idList(site.marks, lua);
			part["kept"] = idList(site.kept, lua);
			part["skipped"] = idList(site.skipped, lua);
			if (!site.backup.empty()) part["backup"] = site.backup;
			table[Accounts::SiteName(site.site)] = part;
		}
		return table;
	}

	sol::object MarksStart(sol::object sites, const std::string& action, sol::object add, sol::object remove, sol::object onDone,
		sol::this_state state)
	{
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext) return sol::nil;
		Marks::Request request;
		request.owner = ext;
		std::vector<std::string> names;
		if (sites.is<std::string>()) names.push_back(sites.as<std::string>());
		else if (sites.is<sol::table>()) {
			sol::table list = sites;
			for (size_t i = 1; i <= list.size(); i++) {
				sol::object name = list[i];
				if (name.is<std::string>()) names.push_back(name.as<std::string>());
			}
		}
		if (names.empty()) {
			Extensions::logOnce("Marks_Start", "takes a site name or a list of them, like \"hoyolab_map\" or { \"hoyolab_map\", \"appsample\" }");
			return sol::nil;
		}
		for (const std::string& name : names) {
			std::optional<Accounts::Site> site = siteNamed("Marks_Start", name);
			if (!site) return sol::nil;
			if (std::find(request.sites.begin(), request.sites.end(), *site) == request.sites.end()) request.sites.push_back(*site);
		}
		if (action == "fetch") request.action = Marks::Action::Fetch;
		else if (action == "change") request.action = Marks::Action::Change;
		else if (action == "restore") request.action = Marks::Action::Restore;
		else {
			Extensions::logOnce("Marks_Start", "knows \"fetch\", \"change\" and \"restore\", not " + action);
			return sol::nil;
		}
		if (request.action == Marks::Action::Restore) {
			if (request.sites.size() != 1 || !add.is<std::string>()) {
				Extensions::logOnce("Marks_Start", "restores one site from a backup's name, like Marks_Start(\"appsample\", \"restore\", name)");
				return sol::nil;
			}
			request.backup = add.as<std::string>();
		}
		else if (!pointIds(add, request.add) || !pointIds(remove, request.remove)) {
			Extensions::logOnce("Marks_Start", "takes add and remove as lists of point ids");
			return sol::nil;
		}
		if (request.add.size() + request.remove.size() > MarksMaxIds) {
			Extensions::logOnce("Marks_Start", "takes " + std::to_string(MarksMaxIds) + " point ids at most");
			return sol::nil;
		}
		if (onDone != sol::lua_nil && !onDone.is<sol::protected_function>()) {
			Extensions::logOnce("Marks_Start", "takes a function as onDone");
			return sol::nil;
		}
		for (Accounts::Site site : request.sites) {
			if (!Extensions::allowed("Marks_Start", accountPermission(site))) return sol::nil;
		}
		int id = Marks::Start(request);
		if (!id) {
			Extensions::logOnce("Marks_Start", "has " + std::to_string(Marks::MaxWaiting) + " jobs waiting or not collected, no more start");
			return sol::nil;
		}
		for (Accounts::Site site : request.sites) Extensions::markUsed(*ext, accountPermission(site));
		if (onDone != sol::lua_nil) marksRequests.push_back({ id, ext, onDone.as<sol::protected_function>() });
		return sol::make_object(state, id);
	}

	sol::object MarksResult(int id, sol::this_state state)
	{
		Extensions::Extension* ext = Extensions::currentExtension;
		if (!ext) return sol::nil;
		bool hasOnDone = std::any_of(marksRequests.begin(), marksRequests.end(), [id](const MarksRequest& each) { return each.id == id; });
		if (hasOnDone) return sol::nil;
		std::optional<Marks::Result> result = Marks::Take(id, ext);
		if (!result) return sol::nil;
		return marksTable(*result, sol::state_view(state));
	}

	sol::table MarksBackups(const std::string& site, sol::this_state state)
	{
		sol::state_view lua(state);
		sol::table list = lua.create_table();
		std::optional<Accounts::Site> named = siteNamed("Marks_Backups", site);
		if (!named || !Extensions::allowed("Marks_Backups", accountPermission(*named))) return list;
		int i = 1;
		for (const Marks::Backup& backup : Marks::BackupsOf(*named)) {
			// "<site>-2026-11-16_14-02-09", maybe with _2: the date as the player reads it
			std::string date = backup.name.substr(backup.name.find('-') + 1);
			if (date.size() >= 16) date = date.substr(0, 10) + " " + date.substr(11, 2) + ":" + date.substr(14, 2);
			list[i++] = lua.create_table_with("name", backup.name, "count", backup.count, "date", date);
		}
		return list;
	}

	void UpdateMarks()
	{
		Marks::Frame();
		std::vector<MarksRequest> due;
		for (auto it = marksRequests.begin(); it != marksRequests.end();) {
			if (Marks::Done(it->id)) {
				due.push_back(std::move(*it));
				it = marksRequests.erase(it);
			}
			else ++it;
		}
		// a callback can start new jobs, and its extension can be unloaded
		for (MarksRequest& request : due) {
			Extensions::Extension* owner = request.owner;
			std::optional<Marks::Result> result = Marks::Take(request.id, owner);
			if (!result) continue;
			Extensions::runForExtension(owner, [&]() {
				if (!owner->luaState) return;
				sol::table table;
				{
					Extensions::HostAllocations host(owner->memory.get());
					table = marksTable(*result, sol::state_view(*owner->luaState));
				}
				auto called = request.onDone(table);
				if (!called.valid()) {
					sol::error err = called;
					std::cerr << "[" << owner->name << "] Marks_Start onDone: " << err.what() << std::endl;
				}
			});
		}
	}

	void forgetMarks(Extensions::Extension* owner)
	{
		marksRequests.erase(std::remove_if(marksRequests.begin(), marksRequests.end(), [owner](const MarksRequest& each) { return each.owner == owner; }),
			marksRequests.end());
		Marks::Forget(owner);
	}
}
