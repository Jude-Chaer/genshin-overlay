#include <Windows.h>
#include "extensions.hpp"
#include "notices.hpp"
#include "overlay.hpp"
#include "asks.hpp"
#include "files.hpp"
#include "helpers.hpp"
#include "keybindings.hpp"
#include "lua_functions.hpp"
#include "lua_patterns.hpp"
#include "settings.hpp"
#include "triggers.hpp"
#include "key_choices.hpp"
#include "typing.hpp"
#include "controls.hpp"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>

namespace fs = std::filesystem;


namespace Extensions {
	std::vector<std::unique_ptr<Extension>> registeredExtensions;
	Extension* currentExtension = nullptr;

	Phase phase = Phase::None;
	int clipFloor = 0;

	static void noteMemoryHit(Extension& ext) {
		if (ext.stopReason.empty() && ext.memory && ext.memory->hit) {
			ext.stopReason = "it held more than " + std::to_string(ext.memory->limit / (1024 * 1024)) + " MB";
			ext.stoppedForMemory = true;
		}
	}

	// Runs every LuaStepCheck steps of the running extension's Lua. Once it's
	// stopped it fails on every step until the call returns, so a pcall in a
	// loop can't catch it: the loop's own steps fail outside the pcall.
	// counts the steps, true once the running extension is stopped
	static __declspec(noinline) bool stopsNow() {
		Extension* ext = currentExtension;
		if (!ext || ext->closing || ext->callDepth == 0) return false;
		noteMemoryHit(*ext);
		if (ext->stopReason.empty()) {
			ext->luaSteps += LuaStepCheck;
			if (ext->luaSteps <= ext->stepLimit) return false;
			ext->stopReason = "one call ran more than " + std::to_string(ext->stepLimit / 1'000'000) + " million Lua steps";
		}
		return true;
	}

	// luaL_error longjmps out of this frame, so nothing in it has a destructor
	static void guardHook(lua_State* lua, lua_Debug*) {
		if (!stopsNow()) return;
		lua_sethook(lua, guardHook, LUA_MASKCOUNT, 1);
		luaL_error(lua, "stopped: %s", currentExtension->stopReason.c_str());
	}

	// steps a string search took in C, counted like the extension's Lua steps
	static void patternSteps(lua_State* lua, long long steps) {
		Extension* ext = currentExtension;
		if (!ext || ext->closing || ext->callDepth == 0 || !ext->stopReason.empty()) return;
		ext->luaSteps += steps;
		if (ext->luaSteps > ext->stepLimit) lua_sethook(lua, guardHook, LUA_MASKCOUNT, 1);
	}

	// Lua's allocator, counting into memory. Growth past the limit is refused.
	// Lua then collects everything and asks once more, a second refusal with
	// no growth fitting in between means it really holds too much: the guard
	// stops it at its next step.
	static void* guardedAlloc(void* data, void* block, size_t oldSize, size_t newSize) {
		LuaMemory* memory = (LuaMemory*)data;
		size_t had = block ? oldSize : 0;
		if (newSize == 0) {
			free(block);
			memory->used -= had;
			return nullptr;
		}
		bool grows = newSize > had;
		if (grows && memory->used - had + newSize > memory->limit && memory->hostSide) {
			memory->hit = true;
			if (memory->lua) lua_sethook(memory->lua, guardHook, LUA_MASKCOUNT, 1);
		}
		else if (grows && memory->used - had + newSize > memory->limit) {
			if (memory->refused) memory->hit = true;
			memory->refused = true;
			if (memory->hit && memory->lua) lua_sethook(memory->lua, guardHook, LUA_MASKCOUNT, 1);
			return nullptr;
		}
		void* moved = realloc(block, newSize);
		if (!moved) return nullptr;
		// the collect in between frees and shrinks, only a growth that fits clears it
		if (grows) memory->refused = false;
		memory->used = memory->used - had + newSize;
		return moved;
	}

	using Clock = std::chrono::steady_clock;

	// the extension whose call runs now, its time since then not counted yet
	static Extension* timed = nullptr;
	static Clock::time_point timedSince;

	static void countTimed() {
		Clock::time_point now = Clock::now();
		if (timed) timed->cost.ms += std::chrono::duration<double, std::milli>(now - timedSince).count();
		timedSince = now;
	}

	bool endSecond(Cost& cost, int frames) {
		cost.secondMs[cost.nextSecond] = cost.ms;
		cost.secondFrames[cost.nextSecond] = frames;
		cost.nextSecond = (cost.nextSecond + 1) % CostSecondsShown;
		cost.secondsKept = std::min(cost.secondsKept + 1, CostSecondsShown);
		double ms = 0;
		int allFrames = 0;
		for (int i = 0; i < cost.secondsKept; i++) {
			ms += cost.secondMs[i];
			allFrames += cost.secondFrames[i];
		}
		cost.perFrame = ms / allFrames;

		bool slowSecond = frames > 0 && cost.ms / frames > SlowMsPerFrame;
		cost.ms = 0;
		cost.slowSeconds = slowSecond ? cost.slowSeconds + 1 : 0;
		if (cost.slow || cost.slowSeconds < SlowSeconds) return false;
		cost.slow = true;
		return true;
	}

	void skipSecond(Cost& cost) {
		cost.ms = 0;
		cost.slowSeconds = 0;
	}

	void tellSlow(const Extension& ext) {
		std::string folder = ext.folderKey;
		Notices::Add(folder, ext.name, Notices::Kind::Plain, ext.name + " is slowing the overlay down.", glfwGetTime(), "",
			"Open its page", [folder] {
				for (size_t i = 0; i < registeredExtensions.size(); i++) {
					if (registeredExtensions[i]->folderKey == folder) Overlay::ShowExtension((int)i);
				}
			});
	}

	void countTurn() {
		static Clock::time_point secondStart = Clock::now();
		static int turns = 0;

		Clock::time_point now = Clock::now();
		turns++;
		if (now - secondStart < std::chrono::seconds(1)) return;

		for (auto& ext : registeredExtensions) {
			if (!ext->cost.started) {
				skipSecond(ext->cost);
				ext->cost.started = true;
			}
			else if (endSecond(ext->cost, turns)) {
				std::cerr << "[" << ext->name << "] slow: more than " << SlowMsPerFrame << " ms per frame for "
					<< SlowSeconds << " s" << std::endl;
				tellSlow(*ext);
			}
		}
		secondStart = now;
		turns = 0;
	}

	// Lua functions like DefineExtensionSetting need to know which extension called them
	void runForExtension(Extension* ext, const std::function<void()>& function) {
		if (!function) return;
		// stopped by the guard: only its closing still runs
		if (ext && !ext->stopReason.empty() && !ext->closing) return;

		Extension* previous = currentExtension;
		int previousFloor = clipFloor;
		currentExtension = ext;
		bool inFrame = phase != Phase::None && ImGui::GetCurrentContext();
		ImDrawList* list = inFrame ? ImGui::GetBackgroundDrawList() : nullptr;
		if (list) clipFloor = list->_ClipRectStack.Size;

		if (ext) {
			if (ext->callDepth == 0) ext->luaSteps = 0;
			ext->callDepth++;
		}
		countTimed();
		Extension* previousTimed = timed;
		timed = ext;

		auto restore = [&]() {
			countTimed();
			timed = previousTimed;
			if (ext) {
				ext->callDepth--;
				// Lua's memory error can end the call before the guard's next step
				noteMemoryHit(*ext);
				// a header's card still open when Menu() returns or stops on an error
				if (ext->callDepth == 0 && ext->headerCard) {
					ext->headerCard = false;
					Controls::EndSection();
				}
			}
			// a clip left open would cut everything drawn after it
			if (list) {
				while (list->_ClipRectStack.Size > clipFloor) list->PopClipRect();
			}
			clipFloor = previousFloor;
			currentExtension = previous;
		};

		try {
			function();
		}
		catch (...) {
			restore();
			throw;
		}

		restore();
	}

	void logOnce(const char* function, const std::string& why) {
		Extension* ext = currentExtension;
		if (ext && ext->refusalsLogged.insert(std::string(function) + " " + why).second) {
			std::cerr << "[" << ext->name << "] " << function << " " << why << std::endl;
		}
	}

	bool inPhase(const char* function, Phase need) {
		if (phase == need || (need == Phase::Update && phase == Phase::Menu)) return true;
		logOnce(function, need == Phase::Menu ? "works only in Menu()" : "works only in Update() and Menu()");
		return false;
	}

	// settings are registered before Init so Init can already read them
	void initExtensions() {
		findAndLoadExtensions();
		configureSettings();

		for (auto& ext : registeredExtensions) runInit(*ext);
	}

	// unloaded and its Lua state closed, without Shutdown
	static void closeLua(Extension& ext) {
		unload(ext);
		ext.stopped = true;
		ext.closing = true;
		runForExtension(&ext, [&]() { ext.luaState.reset(); });
		ext.closing = false;
	}

	static void failStart(Extension& ext, const std::string& error) {
		if (ext.stopReason.empty()) ext.startError = errorLine(ext, error);
		if (ext.luaState) closeLua(ext);
	}

	void runInit(Extension& ext) {
		if (!ext.initializeFunction) return;
		ext.lastError.clear();
		ext.stepLimit = LuaStartStepLimit;
		runForExtension(&ext, ext.initializeFunction);
		ext.stepLimit = LuaStepLimit;
		if (!ext.lastError.empty() || !ext.stopReason.empty()) failStart(ext, ext.lastError);
	}

	std::string errorLine(const Extension& ext, const std::string& error) {
		std::string text = error.substr(0, error.find("\nstack traceback:"));
		// file.lua:12: what went wrong
		size_t lua = text.find(".lua:");
		if (lua == std::string::npos) return text;
		size_t digits = lua + 5, end = digits;
		while (end < text.size() && isdigit((unsigned char)text[end])) end++;
		if (end == digits || text.compare(end, 2, ": ") != 0) return text;
		std::string place = text.substr(0, end);
		std::string folder = ext.folderKey + "/";
		if (place.rfind(folder, 0) == 0) place.erase(0, folder.size());
		return text.substr(end + 2) + " (" + place + ")";
	}

	const PermissionInfo* findPermission(ExtensionPermissionTypes type) {
		for (const auto& permission : permissionList) {
			if (permission.type == type) return &permission;
		}
		return nullptr;
	}

	const PermissionInfo* findPermission(const std::string& name) {
		for (const auto& permission : permissionList) {
			if (name == permission.name) return &permission;
		}
		return nullptr;
	}

	bool allowed(const char* function, ExtensionPermissionTypes permission) {
		Extension* ext = currentExtension;
		if (!ext || ext->closing) return false;
		if (permission == ExtensionPermissionTypes::None) return true;
		if (ext->extensionPermissions.count(permission)) {
			markUsed(*ext, permission);
			return true;
		}

		if (ext->refusalsLogged.insert(function).second) {
			const PermissionInfo* info = findPermission(permission);
			std::cerr << "[" << ext->name << "] " << function << " needs " << (info ? info->label : "a permission")
				<< ", which it isn't allowed" << std::endl;
		}
		wanted(*ext, permission);
		return false;
	}

	void wanted(Extension& ext, ExtensionPermissionTypes permission) {
		const PermissionInfo* info = findPermission(permission);
		if (!info) return;
		if (ext.uses.insert(permission).second && !ext.folderKey.empty()) {
			std::vector<std::string> saved = Settings::LoadCalls(ext.folderKey);
			if (std::find(saved.begin(), saved.end(), info->name) == saved.end()) {
				saved.push_back(info->name);
				Settings::SaveCalls(ext.folderKey, saved);
			}
		}
		if (permission == ExtensionPermissionTypes::Keybinds) return;
		bool loaded = std::any_of(registeredExtensions.begin(), registeredExtensions.end(),
			[&](const auto& each) { return each.get() == &ext; });
		if (loaded) Asks::askPermission(ext, permission);
		else if (std::find(ext.askWhenLoaded.begin(), ext.askWhenLoaded.end(), permission) == ext.askWhenLoaded.end())
			ext.askWhenLoaded.push_back(permission);
	}

	// Lua's long brackets: [[, [=[ and so on
	static bool longOpen(const std::string& code, size_t at, size_t& level, size_t& after) {
		if (at >= code.size() || code[at] != '[') return false;
		size_t i = at + 1;
		while (i < code.size() && code[i] == '=') i++;
		if (i >= code.size() || code[i] != '[') return false;
		level = i - at - 1;
		after = i + 1;
		return true;
	}

	// where the text inside ends, and where the bracket after it ends
	static std::pair<size_t, size_t> longClose(const std::string& code, size_t from, size_t level) {
		std::string close = "]" + std::string(level, '=') + "]";
		size_t end = code.find(close, from);
		if (end == std::string::npos) return { code.size(), code.size() };
		return { end, end + close.size() };
	}

	std::unordered_set<ExtensionPermissionTypes> callsFor(const std::string& code) {
		// a function by its name, a usertype's (WebSocket) by the type's
		std::unordered_map<std::string, ExtensionPermissionTypes> names;
		for (const auto& function : LuaFunctions::boundFunctions()) {
			if (function.permission == ExtensionPermissionTypes::None) continue;
			size_t dot = function.name.find('.');
			names[dot == std::string::npos ? function.name : function.name.substr(0, dot)] = function.permission;
		}
		names["RegisterKeybind"] = ExtensionPermissionTypes::Keybinds;

		std::unordered_set<ExtensionPermissionTypes> found;
		auto fromString = [&](const std::string& text) {
			if (const PermissionInfo* info = findPermission(text)) found.insert(info->type);
			ExtensionPermissionTypes trigger = Triggers::permissionFor(text);
			if (trigger != ExtensionPermissionTypes::None) found.insert(trigger);
		};
		auto word = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
		size_t i = 0;
		while (i < code.size()) {
			char c = code[i];
			size_t level = 0, after = 0;
			if (c == '-' && i + 1 < code.size() && code[i + 1] == '-') {
				if (longOpen(code, i + 2, level, after)) i = longClose(code, after, level).second;
				else {
					size_t end = code.find('\n', i);
					i = end == std::string::npos ? code.size() : end + 1;
				}
			}
			else if (longOpen(code, i, level, after)) {
				auto [end, next] = longClose(code, after, level);
				fromString(code.substr(after, end - after));
				i = next;
			}
			else if (c == '"' || c == '\'') {
				std::string text;
				size_t j = i + 1;
				while (j < code.size() && code[j] != c && code[j] != '\n') {
					if (code[j] == '\\' && j + 1 < code.size()) j++;
					text += code[j++];
				}
				fromString(text);
				i = j + 1;
			}
			else if (word(c)) {
				size_t j = i;
				while (j < code.size() && word(code[j])) j++;
				if (!std::isdigit((unsigned char)c)) {
					auto name = names.find(code.substr(i, j - i));
					if (name != names.end()) found.insert(name->second);
				}
				i = j;
			}
			else i++;
		}
		return found;
	}

	// what its Lua files call for, and what it was refused before that they didn't show
	static void findUses(Extension& ext, const fs::path& folderPath) {
		ext.uses.clear();
		for (const auto& [path, hash] : ext.luaFiles) {
			for (ExtensionPermissionTypes permission : callsFor(Files::readText(folderPath / fs::u8path(path)))) ext.uses.insert(permission);
		}
		for (const std::string& name : Settings::LoadCalls(ext.folderKey)) {
			if (const PermissionInfo* info = findPermission(name)) ext.uses.insert(info->type);
		}
	}

	void markUsed(Extension& ext, ExtensionPermissionTypes permission) {
		ext.lastUsed[(int)permission] = (long long)std::time(nullptr);
	}

	std::string lastUsedLine(const Extension& ext, ExtensionPermissionTypes permission, long long now) {
		long long used = ext.lastUsed[(int)permission];
		if (used == 0) return ext.extensionPermissions.count(permission) ? "Not used yet" : "";
		long long minutes = std::max(0LL, now - used) / 60;
		if (minutes == 0) return "Used just now";
		if (minutes < 60) return "Used " + std::to_string(minutes) + " min ago";
		if (minutes < 48 * 60) return "Used " + std::to_string(minutes / 60) + " h ago";
		return "Used " + std::to_string(minutes / (24 * 60)) + " days ago";
	}

	void saveLastUsed() {
		std::map<std::string, std::map<std::string, long long>> all;
		for (auto& ext : registeredExtensions) {
			if (ext->folderKey.empty()) continue;
			std::map<std::string, long long>& times = all[ext->folderKey];
			for (const auto& permission : permissionList) {
				if (ext->lastUsed[(int)permission.type]) times[permission.name] = ext->lastUsed[(int)permission.type];
			}
		}
		Settings::SaveLastUsed(all);
	}

	// file is inside root, or root itself; both canonical
	static bool inside(const fs::path& root, const fs::path& file) {
		fs::path relative = file.lexically_relative(root);
		return !relative.empty() && *relative.begin() != "..";
	}

	// Windows opens these as devices in any folder: NUL waits for nothing, COM1
	// talks to a port, CON is the console. With an extension after them too
	// (nul.txt), and with spaces before it.
	static bool isDeviceName(const fs::path& part) {
		std::wstring name = part.native();
		name = name.substr(0, name.find(L'.'));
		while (!name.empty() && name.back() == L' ') name.pop_back();
		for (const wchar_t* device : { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" }) {
			if (_wcsicmp(name.c_str(), device) == 0) return true;
		}
		// COM0 to COM9, LPT0 to LPT9, and with ¹ ² ³
		if (name.size() == 4 && (_wcsnicmp(name.c_str(), L"COM", 3) == 0 || _wcsnicmp(name.c_str(), L"LPT", 3) == 0)) {
			wchar_t n = name[3];
			return (n >= L'0' && n <= L'9') || n == L'\u00B9' || n == L'\u00B2' || n == L'\u00B3';
		}
		return false;
	}

	// The file a UTF-8 path means, relative to root (canonical). Nothing for a
	// path that starts with anything but a drive letter (\\server\share, //server,
	// \\?\, \??\UNC\server): looking one up already connects to the server.
	// Nothing for a device name or a ':' after the drive (a file's hidden
	// stream, which no hash sees). Nothing for a path that leaves root, and
	// outside says so; one that leaves it by its text isn't looked up.
	static std::optional<fs::path> resolve(const fs::path& root, const std::string& path, bool& outside) {
		outside = false;
		try {
			fs::path asked = Helpers::fromUtf8(path);
			if (asked.has_root_name()) {
				std::wstring name = asked.root_name().native();
				bool drive = name.size() == 2 && name[1] == L':' && iswalpha(name[0]);
				if (!drive) return std::nullopt;
			}
			if (asked.relative_path().native().find(L':') != std::wstring::npos) return std::nullopt;
			for (const fs::path& part : asked.relative_path()) {
				if (isDeviceName(part)) return std::nullopt;
			}
			fs::path file = (root / asked).lexically_normal();
			if (!inside(root, file)) {
				outside = true;
				return std::nullopt;
			}
			std::error_code ec;
			file = fs::weakly_canonical(file, ec);
			if (ec) return std::nullopt;
			// a link inside can still point out
			outside = !inside(root, file);
			if (outside) return std::nullopt;
			return file;
		}
		catch (const std::exception&) {
			return std::nullopt;
		}
	}

	std::optional<fs::path> pathFor(Extension& ext, const std::string& path, Access access) {
		ExtensionPermissionTypes needed = access == Access::Read ? ExtensionPermissionTypes::InternalReadfile : ExtensionPermissionTypes::InternalWritefile;

		std::error_code ec;
		fs::path root = fs::weakly_canonical(ext.folder, ec);
		bool outside = false;
		std::optional<fs::path> file;
		if (!ec) file = resolve(root, path, outside);
		if (outside) {
			if (ext.refusalsLogged.insert("path outside").second) {
				std::cerr << "[" << ext.name << "] " << path << " is outside its folder, which extensions can't use" << std::endl;
			}
			return std::nullopt;
		}
		if (!file) {
			if (ext.refusalsLogged.insert("path " + path).second) {
				std::cerr << "[" << ext.name << "] can't use the path " << path << std::endl;
			}
			return std::nullopt;
		}
		if (ext.extensionPermissions.count(needed)) {
			markUsed(ext, needed);
			return file;
		}

		const PermissionInfo* info = findPermission(needed);
		std::string label = info ? info->label : "a permission";
		if (ext.refusalsLogged.insert("path " + label).second) {
			std::cerr << "[" << ext.name << "] " << path << " needs " << label << ", which it isn't allowed" << std::endl;
		}
		wanted(ext, needed);
		return std::nullopt;
	}

	std::optional<fs::path> pathFor(const std::string& path, Access access) {
		if (!currentExtension) return std::nullopt;
		return pathFor(*currentExtension, path, access);
	}

	void unload(Extension& ext) {
		Asks::forget(ext);
		for (const Extension::HeldKey& held : ext.keys) {
			if (!held.bound.empty()) Keybindings::DeleteKeybind(held.bound);
		}
		ext.keys.clear();
		LuaFunctions::forgetIntervals(&ext);
		Triggers::forget(&ext);
		LuaFunctions::forgetNetRequests(&ext);
		LuaFunctions::forgetFileDialogs(&ext);
		LuaFunctions::forgetSockets(&ext);
		if (!ext.textures.empty()) {
			glDeleteTextures((GLsizei)ext.textures.size(), ext.textures.data());
			ext.textures.clear();
		}
		ext.webImages.clear();
		ext.dots.clear();

		ext.initializeFunction = nullptr;
		ext.shutdownFunction = nullptr;
		ext.registerSettings = nullptr;
		ext.updateFunction = nullptr;
		ext.menuFunction = nullptr;

		if (ext.extensionImage != 0) {
			glDeleteTextures(1, &ext.extensionImage);
			ext.extensionImage = 0;
		}
	}

	// What it registered goes first: their Lua functions are let go while the
	// state is still open. The other members go after the state.
	Extension::~Extension() {
		closing = true;
		unload(*this);
		runForExtension(this, [this]() { luaState.reset(); });
	}

	void destroyExtensions() {
		for (auto& ext : registeredExtensions) {
			runForExtension(ext.get(), ext->shutdownFunction);
		}
		saveLastUsed();
		registeredExtensions.clear();
	}

	void unloadGuardStopped() {
		for (auto& ext : registeredExtensions) {
			if (ext->stopReason.empty() || !ext->luaState) continue;
			std::cerr << "[" << ext->name << "] stopped: " << ext->stopReason << std::endl;
			// its memory goes too, Shutdown doesn't run: it would run into the same limit
			closeLua(*ext);
		}
	}

	void frameUpdateExtensions() {
		unloadGuardStopped();
		startAgainPressed();
		InPhase update(Phase::Update);
		LuaFunctions::Items::beginFrame();
		LuaFunctions::UpdateIntervals();
		for (auto& ext : registeredExtensions) {
			if (!ext->stopped) runForExtension(ext.get(), ext->updateFunction);
		}
	}

	// kept per extension folder in settings.json
	void saveSettings() {
		nlohmann::json all = Settings::LoadExtensions();
		for (auto& ext : registeredExtensions) {
			nlohmann::json& saved = all[ext->folderKey];
			if (!saved.is_object()) saved = nlohmann::json::object();
			for (const auto& [id, setting] : ext->settings) {
				if (setting.value == setting.defaultValue) saved.erase(id);
				else std::visit([&](auto&& value) { saved[id] = value; }, setting.value);
			}
			if (saved.empty()) all.erase(ext->folderKey);
		}
		Settings::SaveExtensions(all);
	}

	// a saved value that isn't the setting's type anymore is left out
	static void loadSettings() {
		nlohmann::json all = Settings::LoadExtensions();
		for (auto& ext : registeredExtensions) {
			auto saved = all.find(ext->folderKey);
			if (saved == all.end() || !saved->is_object()) continue;
			for (auto& [id, setting] : ext->settings) {
				auto it = saved->find(id);
				if (it == saved->end()) continue;
				std::visit([&](auto&& current) {
					try { setting.value = it->get<std::decay_t<decltype(current)>>(); }
					catch (const nlohmann::json::exception&) {}
					}, setting.value);
				fitToRange(setting);
			}
		}
	}

	Controls::NumberOptions numberOptions(const ExtensionSetting& setting) {
		Controls::NumberOptions options;
		if (setting.min && setting.max) {
			options.min = *setting.min;
			options.max = *setting.max;
		}
		if (setting.step) options.step = *setting.step;
		options.field = setting.field;
		options.whole = setting.type == ExtensionSettingTypes::Int;
		if (setting.type == ExtensionSettingTypes::Int) options.defaultValue = std::get<int>(setting.defaultValue);
		else if (setting.type == ExtensionSettingTypes::Float) options.defaultValue = std::get<float>(setting.defaultValue);
		return options;
	}

	// a number saved outside its range, by hand or before the range changed, comes back inside it
	void fitToRange(ExtensionSetting& setting) {
		auto options = numberOptions(setting);
		if (auto* i = std::get_if<int>(&setting.value)) *i = static_cast<int>(Controls::Fit(*i, options));
		else if (auto* f = std::get_if<float>(&setting.value)) *f = static_cast<float>(Controls::Fit(*f, options));
	}

	bool drawSetting(ExtensionSetting& setting) {
		bool done = false;
		switch (setting.type) {
		case ExtensionSettingTypes::Bool:
			done = Controls::Toggle(setting.label.c_str(), &std::get<bool>(setting.value));
			break;
		case ExtensionSettingTypes::Int: {
			double value = std::get<int>(setting.value);
			auto result = Controls::Number(setting.label.c_str(), value, numberOptions(setting));
			std::get<int>(setting.value) = static_cast<int>(value);
			done = result.done;
			break;
		}
		case ExtensionSettingTypes::Float: {
			double value = std::get<float>(setting.value);
			auto result = Controls::Number(setting.label.c_str(), value, numberOptions(setting));
			std::get<float>(setting.value) = static_cast<float>(value);
			done = result.done;
			break;
		}
		case ExtensionSettingTypes::String: {
			Controls::TextOptions options;
			options.defaultValue = std::get<std::string>(setting.defaultValue);
			options.maxBytes = 256;
			setting.edited |= Controls::Text(setting.label.c_str(), std::get<std::string>(setting.value), options);
			done = setting.edited && !ImGui::IsAnyItemActive();
			if (done) setting.edited = false;
			break;
		}
		}
		return done;
	}

	// one section per extension: icon, info, settings, then its own Menu()
	void drawExtensionMenus(int extensionIndex) {
		if (extensionIndex < 0 ||
			extensionIndex >= static_cast<int>(registeredExtensions.size())) {
			Controls::Empty("Extension not found");
			return;
		}

		auto& ext = registeredExtensions[extensionIndex];

		ImGui::PushID(ext.get());

		if (ext->extensionImage != 0) {
			float size = ImGui::GetFontSize() * 3.2f;

			ImGui::Image(
				(ImTextureID)(intptr_t)ext->extensionImage,
				ImVec2(size, size)
			);

			ImGui::SameLine();
		}

		ImGui::BeginGroup();

		Controls::PageTitle(ext->name.c_str());

		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped(
			"%s",
			ext->description.c_str()
		);
		ImGui::PopStyleColor();

		ImGui::EndGroup();
		ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));

		std::vector<std::string> tabs = { "General" };
		if (!::Notices::Of(ext->folderKey).empty()) tabs.push_back("Notices");
		if (!ext->keys.empty()) tabs.push_back("Keys");
		tabs.push_back("Permissions");
		tabs.push_back("About");
		const std::string& tab = tabs[Controls::Tabs("##extensionTabs", tabs)];
		{
			if (tab == "General") {
				if (!ext->stopReason.empty() || !ext->startError.empty()) {
					if (!ext->stopReason.empty())
						ImGui::TextWrapped("Stopped: %s", ext->stoppedForMemory ? "it used too much memory" : "it ran too long");
					else
						ImGui::TextWrapped("Couldn't start: %s", ext->startError.c_str());
					if (Controls::Button("Start again")) ext->startAgain = true;
				}
				else if (ext->stopped) {
					ImGui::TextDisabled("Switched off.");
				}
				for (const auto& id : ext->settingsOrder) {
					auto it = ext->settings.find(id);

					if (it != ext->settings.end() && drawSetting(it->second)) {
						saveSettings();
					}
				}

				{
					InPhase menu(Phase::Menu);
					runForExtension(ext.get(), ext->menuFunction);
				}
			}
			else if (tab == "Notices") ::Notices::DrawList(ext->folderKey);
			else if (tab == "Keys") KeyChoices::drawKeysTab(*ext);
			else if (tab == "Permissions") drawPermissionsTab(*ext);
			else drawAboutTab(*ext);
		}

		ImGui::PopID();
	}

	void configureSettings() {
		for (auto& ext : registeredExtensions) {
			runForExtension(ext.get(), ext->registerSettings);
		}
		loadSettings();
	}

	// every folder in root with a main.lua in it is an extension
	static std::vector<fs::path> extensionFolders(const fs::path& root, bool quiet) {
		std::vector<fs::path> found;
		std::error_code ec;
		// an entry that can't be looked at is skipped, the others still load
		fs::directory_iterator folders(root, ec);
		for (; !ec && folders != fs::directory_iterator(); folders.increment(ec)) {
			const fs::directory_entry& dirEntry = *folders;
			std::error_code entryError;
			if (!dirEntry.is_directory(entryError)) continue;
			if (fs::is_regular_file(dirEntry.path() / "main.lua", entryError)) found.push_back(dirEntry.path());
		}
		if (ec && !quiet) std::cerr << "Couldn't look through " << Helpers::utf8(root) << ": " << ec.message() << std::endl;
		return found;
	}

	static void loadFolder(const fs::path& folder) {
		// before anything of it runs, so a crash while it loads names it
		std::cout << "Loading extension: " << Helpers::utf8(folder.filename()) << std::endl;
		loadLuaExtension(folder / "main.lua", folder);
	}

	void findAndLoadExtensions() {
		std::filesystem::path extensionsRoot = Helpers::exeDirectory() / "extensions";

		std::error_code ec;
		if (!std::filesystem::exists(extensionsRoot, ec)) {
			std::filesystem::create_directory(extensionsRoot, ec);
			return;
		}
		for (const fs::path& folder : extensionFolders(extensionsRoot, false)) loadFolder(folder);
	}

	// each file's path, size and write time
	static std::string folderContents(const fs::path& folder) {
		std::string contents;
		std::error_code ec;
		for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code fileError;
			if (!it->is_regular_file(fileError)) continue;
			contents += Helpers::utf8(it->path()) + "|" + std::to_string(it->file_size(fileError)) + "|"
				+ std::to_string(it->last_write_time(fileError).time_since_epoch().count()) + "\n";
		}
		return contents;
	}

	struct Arriving {
		std::string contents;
		std::chrono::steady_clock::time_point since;
	};
	static std::map<fs::path, Arriving> arriving;

	int findNewExtensions(const fs::path& root, std::chrono::seconds settled) {
		int loaded = 0;
		auto now = std::chrono::steady_clock::now();
		for (const fs::path& folder : extensionFolders(root, true)) {
			bool known = std::any_of(registeredExtensions.begin(), registeredExtensions.end(),
				[&](const auto& ext) { return ext->folder == folder; });
			if (known) continue;
			std::string contents = folderContents(folder);
			auto seen = arriving.find(folder);
			if (seen == arriving.end() || seen->second.contents != contents) {
				arriving[folder] = { contents, now };
				if (settled.count() > 0) continue;
			}
			else if (now - seen->second.since < settled) continue;
			arriving.erase(folder);

			loadFolder(folder);
			Extension& ext = *registeredExtensions.back();
			runForExtension(&ext, ext.registerSettings);
			loadSettings();
			runInit(ext);
			loaded++;
		}
		return loaded;
	}

	// wraps a Lua function so an error gets printed instead of taking the overlay down,
	// once while the same error repeats
	static std::function<void()> getLuaFunction(sol::environment& env, const char* luaName, Extension* ext) {
		sol::optional<sol::protected_function> function = env[luaName];
		if (!function) return nullptr;

		auto lastLogged = std::make_shared<std::string>();
		return [function, luaName, ext, lastLogged]() {
			auto result = function->call();
			if (!result.valid()) {
				sol::error err = result;
				std::string error = err.what();
				if (error != *lastLogged)
					std::cerr << "[" << ext->name << "] " << luaName << ": " << error << std::endl;
				*lastLogged = error;
				ext->lastError = error;
			}
			};
	}

	// the hash kept at load for a file in the folder, nullptr when it wasn't
	// hashed. Windows opens Helper.lua for helper.lua, so case doesn't count.
	static const std::string* loadedHash(const Extension& ext, const fs::path& relative) {
		const std::wstring& name = relative.native();
		for (const auto& [path, hash] : ext.luaFiles) {
			std::wstring hashed = Helpers::fromUtf8(path).make_preferred().native();
			if (CompareStringOrdinal(hashed.c_str(), (int)hashed.size(), name.c_str(), (int)name.size(), TRUE) == CSTR_EQUAL) return &hash;
		}
		return nullptr;
	}

	// require() looks in the extension's own folder. Lua's own searcher opens
	// files by code page names, which can't hold every folder name. Only Lua
	// text is loaded: precompiled code can get around Lua's checks. Only files
	// that were in the hash, as they were then: one written or changed since
	// would run under what the player allowed the old files.
	static void requireFromFolder(sol::state& lua, const fs::path& folder) {
		std::error_code ec;
		fs::path root = fs::weakly_canonical(folder, ec);
		sol::table searchers = lua["package"]["searchers"];
		searchers[2] = [root](sol::this_state state, const std::string& module) -> sol::variadic_results {
			sol::variadic_results results;
			std::string relative = module;
			std::replace(relative.begin(), relative.end(), '.', '/');
			relative += ".lua";

			bool outside = false;
			fs::path file = resolve(root, relative, outside).value_or(fs::path());
			std::error_code ec;
			if (!fs::is_regular_file(file, ec)) file.clear();
			if (file.empty()) {
				results.push_back(sol::make_object(state, "no file " + relative + " in the extension's folder"));
				return results;
			}

			Extension* ext = currentExtension;
			const std::string* hash = ext ? loadedHash(*ext, file.lexically_relative(root)) : nullptr;
			if (!hash) {
				results.push_back(sol::make_object(state, relative + " wasn't there when the extension loaded"));
				return results;
			}
			sol::state_view view(state);
			std::string code = Files::readText(file);
			if (Helpers::hashString(code) != *hash) {
				throw sol::error(relative + " changed since the extension loaded, it runs again after a restart");
			}
			sol::load_result chunk = view.load_buffer(code.data(), code.size(), "@" + relative, sol::load_mode::text);
			if (!chunk.valid()) {
				sol::error err = chunk;
				throw sol::error(err.what());
			}
			sol::protected_function loaded = chunk;
			results.push_back(sol::make_object(state, loaded));
			results.push_back(sol::make_object(state, Helpers::utf8(file)));
			return results;
		};
	}

	// Lua's print, at most MaxPrintsPerMinute lines a minute for each extension
	static int cappedPrint(lua_State* lua) {
		Extension* ext = currentExtension;
		if (ext) {
			unsigned long long now = GetTickCount64();
			if (now - ext->printMinute >= 60000) {
				ext->printMinute = now;
				ext->printsThisMinute = 0;
			}
			if (++ext->printsThisMinute > MaxPrintsPerMinute) {
				if (ext->printsThisMinute == MaxPrintsPerMinute + 1)
					std::cerr << "[" << ext->name << "] print: more than " << MaxPrintsPerMinute << " lines this minute, the rest are left out" << std::endl;
				return 0;
			}
		}
		// the line is put together in Lua, a __tostring that fails leaves nothing of C++ behind
		int count = lua_gettop(lua);
		luaL_Buffer line;
		luaL_buffinit(lua, &line);
		for (int i = 1; i <= count; i++) {
			if (i > 1) luaL_addchar(&line, '\t');
			luaL_tolstring(lua, i, nullptr);
			luaL_addvalue(&line);
		}
		luaL_pushresult(&line);
		size_t size = 0;
		const char* text = lua_tolstring(lua, -1, &size);
		std::cout.write(text, (std::streamsize)size);
		std::cout << std::endl;
		return 0;
	}

	std::unique_ptr<sol::state> newLuaState(const fs::path& folder, LuaMemory* memory) {
		auto lua = memory ? std::make_unique<sol::state>(sol::default_at_panic, guardedAlloc, memory) : std::make_unique<sol::state>();
		if (memory) memory->lua = lua->lua_state();
		lua_sethook(lua->lua_state(), guardHook, LUA_MASKCOUNT, LuaStepCheck);
		lua->open_libraries(
			sol::lib::base,
			sol::lib::package,
			sol::lib::math,
			sol::lib::string,
			sol::lib::table
		);
		lua->globals()["print"] = cappedPrint;
		lua->globals()["load"] = sol::nil;
		lua->globals()["loadfile"] = sol::nil;
		lua->globals()["dofile"] = sol::nil;
		lua->globals()["loadstring"] = sol::nil;
		lua->globals()["package"]["path"] = "";
		lua->globals()["package"]["cpath"] = "";
		lua->globals()["package"]["loadlib"] = sol::nil;
		// it opens any file it's given, to see whether it's there
		lua->globals()["package"]["searchpath"] = sol::nil;
		// it makes precompiled code, which is never loaded (text only, see requireFromFolder)
		lua->globals()["string"]["dump"] = sol::nil;
		LuaPatterns::guard(lua->lua_state(), patternSteps);
		// the searchers for C libraries, an extension only gets Lua files
		sol::table searchers = lua->globals()["package"]["searchers"];
		searchers[4] = sol::nil;
		searchers[3] = sol::nil;
		// Lua runs no hooks in a finalizer, or in an error handler for an error
		// the guard raised: the guard couldn't stop a loop there. xpcall's
		// handler runs once the call is undone (there's no debug library to
		// see the difference). No table gets a finalizer, and getmetatable
		// gives only the metatables the extension set itself: the overlay's
		// userdata and tables have finalizers it could change.
		lua->safe_script(R"(
			local pcall, setmetatable, getmetatable, rawget, type, error, pack, unpack = pcall, setmetatable, getmetatable, rawget, type, error, table.pack, table.unpack
			local given = setmetatable({}, { __mode = "k" })
			_G.getmetatable = function(value)
				local meta = getmetatable(value)
				if type(value) == "table" and given[meta] then return meta end
				if type(value) == "string" or type(meta) ~= "table" then return meta end
				return nil
			end
			function xpcall(f, handler, ...)
				local results = pack(pcall(f, ...))
				if results[1] then return unpack(results, 1, results.n) end
				local _, message = pcall(handler, results[2])
				return false, message
			end
			_G.setmetatable = function(t, meta)
				if type(meta) == "table" and rawget(meta, "__gc") ~= nil then
					error("an extension can't use __gc, a finalizer can't be stopped when it runs too long", 2)
				end
				if type(meta) == "table" then
					given[meta] = true
					local shown = rawget(meta, "__metatable")
					if type(shown) == "table" then given[shown] = true end
				end
				return setmetatable(t, meta)
			end
		)", sol::script_default_on_error, "=guard");
		requireFromFolder(*lua, folder);
		LuaFunctions::registerLuaFunctions(*lua);
		return lua;
	}

	// ImGui hides what comes after ## in a button, the name is shown whole
	static std::string shownName(std::string name) {
		for (size_t at = name.find("##"); at != std::string::npos; at = name.find("##", at)) name.replace(at, 2, "# #");
		return name;
	}

	// What the author wrote, at most `most` letters with "..." where it's
	// cut. One line unless lines, then only its line breaks are kept.
	static std::string authorText(const std::string& text, size_t most, bool lines) {
		std::string shown;
		size_t letters = 0;
		for (size_t i = 0; i < text.size(); i++) {
			unsigned char c = (unsigned char)text[i];
			if ((c & 0xC0) != 0x80 && ++letters > most) {
				shown += "...";
				break;
			}
			shown += c == '\n' && lines ? '\n' : c < 0x20 ? ' ' : (char)c;
		}
		return shownName(shown);
	}

	static std::string authorLine(const std::string& text) {
		return authorText(text, 200, false);
	}

	// Every folder gives an extension: one that failed has startError (or
	// stopReason) and no Lua state, and is shown with it.
	static std::unique_ptr<Extension> loadExtension(const fs::path& scriptPath, const fs::path& folderPath) {
		auto ext = std::make_unique<Extension>();
		ext->folder = folderPath;
		ext->folderKey = Helpers::utf8(folderPath.filename());
		ext->name = shownName(ext->folderKey);

		ext->memory = std::make_unique<LuaMemory>();
		ext->luaState = newLuaState(folderPath, ext->memory.get());
		// read before the hash, so what runs is what was hashed (checked below)
		std::string code = Files::readText(scriptPath);

		// Names as UTF-8 with '/', any language works. The same as the code page
		// names for plain English ones, so their hash didn't change.
		std::vector<std::pair<std::string, std::string>> luaFileHashes;
		std::error_code walkError;
		fs::recursive_directory_iterator entries(folderPath, walkError);
		for (; !walkError && entries != fs::recursive_directory_iterator(); entries.increment(walkError)) {
			const fs::directory_entry& entry = *entries;
			std::error_code ec;
			if (!entry.is_regular_file(ec))
				continue;

			// Windows opens helper.LUA for require("helper") too
			if (_wcsicmp(entry.path().extension().c_str(), L".lua") != 0)
				continue;

			std::string relative = Helpers::utf8(entry.path().lexically_relative(folderPath));
			std::replace(relative.begin(), relative.end(), '\\', '/');

			std::string fileHash = Helpers::hashFile(entry.path());

			if (fileHash.empty()) {
				std::cerr << "Failed to hash " << Helpers::utf8(entry.path()) << std::endl;
				failStart(*ext, relative + " can't be read");
				return ext;
			}

			luaFileHashes.emplace_back(relative, fileHash);
		}
		// a folder it can't look through can't be hashed whole
		if (walkError) {
			std::cerr << "Failed to hash " << Helpers::utf8(folderPath) << ": " << walkError.message() << std::endl;
			failStart(*ext, "its folder can't be read: " + walkError.message());
			return ext;
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
		ext->luaFiles = luaFileHashes;
		const std::string* mainHash = loadedHash(*ext, fs::path(L"main.lua"));
		if (!mainHash || Helpers::hashString(code) != *mainHash) {
			std::cerr << "Failed to load " << Helpers::utf8(scriptPath) << ": it changed while it was loading" << std::endl;
			failStart(*ext, "main.lua changed while it was loading");
			return ext;
		}
		loadExtensionPermissions(ext.get());
		findUses(*ext, folderPath);
		// switched off: on its page with nothing of it run, so it can be switched on
		if (!ext->enabled) {
			ext->stopped = true;
			std::cout << "Extension switched off: " << ext->folderKey << std::endl;
			return ext;
		}

		sol::environment env(
			*ext->luaState,
			sol::create,
			ext->luaState->globals()
		);

		env["WORKING_DIR"] = Helpers::utf8(folderPath) + "/";

		// read above, Lua's own loader opens files by code page names
		sol::load_result script = ext->luaState->load_buffer(code.data(), code.size(),
			"@" + ext->folderKey + "/" + Helpers::utf8(scriptPath.filename()), sol::load_mode::text);
		if (!script.valid()) {
			sol::error err = script;
			std::cerr << "Failed to load " << Helpers::utf8(scriptPath) << ": " << err.what() << std::endl;
			failStart(*ext, err.what());
			return ext;
		}

		sol::protected_function scriptFunction = script;
		env.set_on(scriptFunction);

		sol::protected_function_result result;

		runForExtension(ext.get(), [&]() {
			ext->stepLimit = LuaStartStepLimit;
			result = scriptFunction();
			ext->stepLimit = LuaStepLimit;
			});

		if (!result.valid()) {
			sol::error err = result;
			std::cerr << "Failed to run " << Helpers::utf8(scriptPath) << ": " << err.what() << std::endl;
			failStart(*ext, err.what());
			return ext;
		}

		sol::optional<sol::table> meta = env["metadata"];
		if (meta) {
			ext->name = shownName(meta->get_or("name", ext->name));
			if (ext->name.empty()) ext->name = shownName(ext->folderKey);
			ext->description = authorText(meta->get_or("description", std::string("No description.")), DescriptionLetters, false);
			ext->about = authorText(meta->get_or("about", std::string()), AboutLetters, true);
			ext->author = meta->get_or("author", std::string("Unknown"));
			ext->version = meta->get_or("version", std::string("1.0.0"));
			sol::optional<sol::table> listed = (*meta)["permissions"];
			if (listed) readListedPermissions(*ext, *listed);
		}
		ext->listRead = true;

		std::filesystem::path iconPath = folderPath / "icon.png";
		if (std::filesystem::exists(iconPath)) {
			ext->extensionImage = Helpers::loadTextureFromFile(
				iconPath,
				ext->extensionImageWidth,
				ext->extensionImageHeight
			);
		}


		ext->initializeFunction = getLuaFunction(env, "Init", ext.get());
		ext->updateFunction = getLuaFunction(env, "Update", ext.get());
		ext->menuFunction = getLuaFunction(env, "Menu", ext.get());
		ext->registerSettings = getLuaFunction(env, "RegisterSettings", ext.get());
		ext->shutdownFunction = getLuaFunction(env, "Shutdown", ext.get());
		std::cout << "Loaded extension: " << ext->name << " Hash: " << ext->extensionHash << std::endl;
		return ext;
	}

	// its questions go out once it's in registeredExtensions
	static void askOnceLoaded(Extension& ext) {
		if (!ext.luaState || ext.stopped) return;
		askListedPermissions(ext);
		for (ExtensionPermissionTypes permission : ext.askWhenLoaded) {
			if (!ext.extensionPermissions.count(permission)) Asks::askPermission(ext, permission);
		}
		ext.askWhenLoaded.clear();
		Asks::askChanged(ext);
	}

	void loadLuaExtension(const fs::path& scriptPath, const fs::path& folderPath) {
		registeredExtensions.push_back(loadExtension(scriptPath, folderPath));
		askOnceLoaded(*registeredExtensions.back());
	}

	void startAgainPressed() {
		for (auto& slot : registeredExtensions) {
			if (!slot->startAgain) continue;
			fs::path folder = slot->folder;
			std::cout << "Starting again: " << slot->name << std::endl;
			long long lastUsed[PermissionCount];
			std::copy(std::begin(slot->lastUsed), std::end(slot->lastUsed), lastUsed);
			slot.reset();
			slot = loadExtension(folder / "main.lua", folder);
			for (int i = 0; i < PermissionCount; i++) slot->lastUsed[i] = std::max(slot->lastUsed[i], lastUsed[i]);
			askOnceLoaded(*slot);
			runForExtension(slot.get(), slot->registerSettings);
			loadSettings();
			runInit(*slot);
		}
	}

	// a host name: letters, digits, '-' and dots between them, like hoyolab.com
	static bool siteName(const std::string& name) {
		if (name.empty() || name.size() > 253 || name.front() == '.' || name.back() == '.') return false;
		if (name.find("..") != std::string::npos) return false;
		for (char c : name) {
			if (!(std::islower((unsigned char)c) || std::isdigit((unsigned char)c) || c == '-' || c == '.')) return false;
		}
		return true;
	}

	static void readSites(Extension& ext, const sol::table& sites) {
		ext.sitesOnly = true;
		ext.sites.clear();
		for (const auto& [index, value] : sites) {
			std::string name = value.get_type() == sol::type::string ? value.as<std::string>() : "";
			for (char& c : name) c = (char)std::tolower((unsigned char)c);
			if (!siteName(name)) {
				std::cerr << "[" << ext.name << "] metadata.permissions.network.sites: " << authorLine(name)
					<< " isn't a site name, like hoyolab.com" << std::endl;
				continue;
			}
			if (std::find(ext.sites.begin(), ext.sites.end(), name) == ext.sites.end()) ext.sites.push_back(name);
		}
	}

	std::string reasonLine(const Extension& ext, const PermissionInfo& permission) {
		if (permission.type != ExtensionPermissionTypes::NetworkAccess || !ext.sitesOnly) return permission.reason;
		if (ext.sites.empty()) return "It can't talk to any website.";
		return "It can talk to: " + Helpers::joined(ext.sites);
	}

	void readListedPermissions(Extension& ext, const sol::table& list) {
		for (const auto& [key, value] : list) {
			if (key.get_type() != sol::type::string) continue;
			std::string name = key.as<std::string>();
			const PermissionInfo* info = findPermission(name);
			if (!info) {
				std::cerr << "[" << ext.name << "] metadata.permissions: knows no permission named " << name << std::endl;
				continue;
			}
			std::string reason;
			if (value.get_type() == sol::type::table) {
				sol::table entry = value.as<sol::table>();
				sol::optional<std::string> written = entry["reason"];
				if (written) reason = authorLine(*written);
				sol::object sites = entry["sites"];
				if (info->type == ExtensionPermissionTypes::NetworkAccess && sites.get_type() == sol::type::table) {
					readSites(ext, sites.as<sol::table>());
				}
				else if (info->type == ExtensionPermissionTypes::NetworkAccess && sites.valid() && sites.get_type() != sol::type::lua_nil) {
					// a list it can't read lets it reach none
					std::cerr << "[" << ext.name << "] metadata.permissions.network.sites should be a list, like { \"hoyolab.com\" }" << std::endl;
					ext.sitesOnly = true;
					ext.sites.clear();
				}
			}
			else {
				std::cerr << "[" << ext.name << "] metadata.permissions." << name << " should be a table, like { reason = \"...\" }" << std::endl;
			}
			ext.reasons[info->type] = reason;
		}
	}

	void askListedPermissions(Extension& ext) {
		for (const PermissionInfo& permission : permissionList) {
			if (!ext.reasons.count(permission.type) || ext.extensionPermissions.count(permission.type)) continue;
			// keys are asked for one by one, as they're registered
			if (permission.type == ExtensionPermissionTypes::Keybinds) continue;
			bool hadIt = std::find(ext.grantsBeforeChange.begin(), ext.grantsBeforeChange.end(), permission.name) != ext.grantsBeforeChange.end();
			bool newSinceChange = !ext.grantsBeforeChange.empty() && !hadIt;
			if (ext.firstStart || newSinceChange) Asks::askPermission(ext, permission.type);
		}
	}

	// whether the Permissions tab has a row for it: it has it, may have it
	// without asking, its code calls for it, main.lua lists it, it asked, or
	// it's Keybinds and it has keys
	static bool shownOnTab(const Extension& ext, const PermissionInfo& permission) {
		if (permission.type == ExtensionPermissionTypes::Keybinds && !ext.keys.empty()) return true;
		return ext.extensionPermissions.count(permission.type) || defaultPermissions.count(permission.type) ||
			ext.uses.count(permission.type) ||
			ext.reasons.count(permission.type) || ext.refused.count(permission.type) ||
			ext.neverAsk.count(permission.name) || ext.switchedOff.count(permission.name);
	}

	// The player answers on the tab: a waiting "Allow the same again" is
	// answered by what the tab shows.
	static void changedOnTab(Extension& ext) {
		if (!ext.grantsBeforeChange.empty()) {
			ext.grantsBeforeChange.clear();
			ext.hashBeforeChange.clear();
			Asks::forget(ext, Asks::Kind::Changed);
		}
		saveExtensionPermissions(ext);
	}

	void setRunning(Extension& ext, bool on) {
		ext.enabled = on;
		if (!on) {
			ext.startAgain = false;
			stopExtension(ext);
		}
		// a stopped one with an error keeps its [Start again]
		else if (ext.stopped && ext.startError.empty() && ext.stopReason.empty()) ext.startAgain = true;
		saveExtensionPermissions(ext);
	}

	void stopExtension(Extension& ext) {
		if (ext.stopped) return;
		runForExtension(&ext, ext.shutdownFunction);
		unload(ext);
		ext.stopped = true;
		std::cout << "[" << ext.name << "] switched off" << std::endl;
	}

	// The overlay's tab on every extension's page, the extension can't draw in
	// it: the switch for the whole extension, then each permission with its
	// switch and reason line, and what it won't ask for again.
	void drawAboutTab(const Extension& ext) {
		if (!ext.about.empty()) {
			ImGui::TextWrapped("%s", ext.about.c_str());
			ImGui::Separator();
		}
		ImGui::Text("Version: %s", ext.version.c_str());
		ImGui::Text("Author: %s", ext.author.c_str());
		ImGui::TextDisabled("As the extension names it. Nobody checks it.");
		ImGui::Text("Folder: %s", ext.folderKey.c_str());
	}

	void drawPermissionsTab(Extension& ext) {
		// its files weren't read, so what's saved for it wasn't either
		if (ext.extensionHash.empty()) {
			ImGui::TextDisabled("Its files can't be read.");
			return;
		}
		bool on = ext.enabled;
		if (Controls::Toggle("Run this extension", &on)) setRunning(ext, on);
		if (!ext.stopped && ext.stopReason.empty() && ext.cost.perFrame >= 0) {
			if (ext.cost.perFrame < 0.05) ImGui::Text("Time it takes: less than 0.1 ms per frame");
			else if (ext.cost.perFrame > SlowMsPerFrame) ImGui::Text("Time it takes: about %.1f ms per frame, it's slowing the overlay down", ext.cost.perFrame);
			else ImGui::Text("Time it takes: about %.1f ms per frame", ext.cost.perFrame);
		}
		ImGui::Separator();

		for (const auto& permission : permissionList) {
			if (!shownOnTab(ext, permission)) continue;
			bool has = ext.extensionPermissions.count(permission.type) > 0;
			// the extension's own words first, in white, then ours in grey
			Controls::ToggleOptions row;
			auto reason = ext.reasons.find(permission.type);
			if (reason != ext.reasons.end() && !reason->second.empty()) {
				// its reason on one line, in quotes
				std::string said = reason->second;
				std::replace(said.begin(), said.end(), '\n', ' ');
				std::replace(said.begin(), said.end(), '\r', ' ');
				row.lines.push_back({ "\"" + said + "\"", false });
			}
			if (permission.type == ExtensionPermissionTypes::Keybinds && !ext.allowedKeys.empty())
				row.lines.push_back({ "Keys: " + Helpers::joined(ext.allowedKeys), false });
			row.lines.push_back({ reasonLine(ext, permission) });
			if (!has && ext.refused.count(permission.type)) row.lines.push_back({ "Refused at this start." });
			std::string used = lastUsedLine(ext, permission.type, (long long)std::time(nullptr));
			if (!used.empty()) row.lines.push_back({ used });
			if (Controls::Toggle((std::string(permission.label) + "##" + permission.name).c_str(), &has, row)) {
				switchPermission(ext, permission.type, has);
				changedOnTab(ext);
			}
		}

		std::vector<std::string> never(ext.neverAsk.begin(), ext.neverAsk.end());
		std::sort(never.begin(), never.end());
		for (const std::string& name : never) {
			const PermissionInfo* permission = findPermission(name);
			ImGui::PushID(name.c_str());
			ImGui::Text("Won't ask again: %s", permission ? permission->label : name.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Let it ask")) {
				letAsk(ext, name);
				changedOnTab(ext);
			}
			ImGui::PopID();
		}
	}

	static bool isRetired(const std::string& name) {
		return std::find(retiredPermissions.begin(), retiredPermissions.end(), name) != retiredPermissions.end();
	}

	// Saved grants by folder. None saved: the defaults. Saved for other files
	// (the extension changed since): the defaults too, and the old ones are kept
	// for asking to allow the same again. Never, off and enabled are by folder.
	void loadExtensionPermissions(Extension* ext) {
		Settings::Grants grants = Settings::LoadGrants(ext->folderKey, ext->extensionHash);
		ext->extensionPermissions.clear();
		ext->unknownGrants.clear();
		ext->grantsBeforeChange.clear();
		ext->hashBeforeChange.clear();
		ext->neverAsk = { grants.never.begin(), grants.never.end() };
		ext->switchedOff = { grants.off.begin(), grants.off.end() };
		ext->refused.clear();
		ext->allowedKeys = grants.keys;
		ext->pickedKeys = grants.picked;
		ext->refusedKeys.clear();
		ext->refusedSettings.clear();
		ext->openHeaders = Settings::LoadHeaders(ext->folderKey);
		for (const auto& [name, time] : Settings::LoadLastUsed(ext->folderKey)) {
			const PermissionInfo* permission = findPermission(name);
			if (permission) ext->lastUsed[(int)permission->type] = std::max(ext->lastUsed[(int)permission->type], time);
		}
		ext->enabled = grants.enabled;
		ext->firstStart = !grants.saved;

		if (!grants.saved) {
			ext->extensionPermissions = defaultPermissions;
			return;
		}
		// the defaults, but none the player switched off; a default added in a
		// newer overlay is on for extensions saved before it
		for (const PermissionInfo& permission : permissionList) {
			if (defaultPermissions.count(permission.type) && !ext->switchedOff.count(permission.name)) ext->extensionPermissions.insert(permission.type);
		}
		if (grants.hash != ext->extensionHash) {
			for (const std::string& name : grants.names) {
				if (!isRetired(name)) ext->grantsBeforeChange.push_back(name);
			}
			ext->hashBeforeChange = grants.hash;
			std::cout << "[" << ext->folderKey << "] changed since its permissions were set, it runs with the defaults it had" << std::endl;
			return;
		}
		for (const std::string& name : grants.names) {
			const PermissionInfo* permission = findPermission(name);
			if (permission) ext->extensionPermissions.insert(permission->type);
			else if (!isRetired(name)) ext->unknownGrants.push_back(name);
		}
	}

	static std::vector<std::string> sorted(const std::unordered_set<std::string>& names) {
		std::vector<std::string> list(names.begin(), names.end());
		std::sort(list.begin(), list.end());
		return list;
	}

	// Asked to allow the same again, the old hash and grants stay saved until
	// it's answered, with what the player allowed since added to them.
	void saveExtensionPermissions(const Extension& ext) {
		Settings::Grants grants = Settings::LoadGrants(ext.folderKey, ext.extensionHash);
		bool waitingForSame = !ext.grantsBeforeChange.empty();
		if (!waitingForSame) {
			grants.hash = ext.extensionHash;
			grants.names.clear();
		}
		for (const auto& permission : permissionList) {
			bool saved = std::find(grants.names.begin(), grants.names.end(), permission.name) != grants.names.end();
			if (ext.extensionPermissions.count(permission.type) && !saved) grants.names.push_back(permission.name);
		}
		if (!waitingForSame) {
			for (const std::string& name : ext.unknownGrants) grants.names.push_back(name);
		}
		grants.saved = true;
		grants.never = sorted(ext.neverAsk);
		grants.off = sorted(ext.switchedOff);
		grants.keys = ext.allowedKeys;
		grants.picked = ext.pickedKeys;
		grants.enabled = ext.enabled;
		Settings::SaveGrants(ext.folderKey, grants);
	}

	std::string permissionState(const Extension& ext, ExtensionPermissionTypes permission) {
		const PermissionInfo* info = findPermission(permission);
		if (ext.extensionPermissions.count(permission)) return "allowed";
		if (info && ext.switchedOff.count(info->name)) return "off";
		if (info && ext.neverAsk.count(info->name)) return "never";
		if (ext.refused.count(permission)) return "refused";
		return "not_asked";
	}

	bool mayAsk(const Extension& ext, ExtensionPermissionTypes permission) {
		return findPermission(permission) && permissionState(ext, permission) == "not_asked";
	}

	void allow(Extension& ext, ExtensionPermissionTypes permission) {
		const PermissionInfo* info = findPermission(permission);
		if (!info) return;
		ext.extensionPermissions.insert(permission);
		ext.refused.erase(permission);
		ext.neverAsk.erase(info->name);
		ext.switchedOff.erase(info->name);
	}

	void refuse(Extension& ext, ExtensionPermissionTypes permission, bool never) {
		const PermissionInfo* info = findPermission(permission);
		if (!info) return;
		ext.refused.insert(permission);
		if (never) ext.neverAsk.insert(info->name);
	}

	void askWaitingKeys(Extension& ext) {
		for (const Extension::HeldKey& held : ext.keys) {
			if (held.bound.empty() && held.key != 0) Asks::askKeys(ext, held.name);
		}
	}

	void bindAllowedKeys(Extension& ext) {
		if (ext.stopped || !ext.extensionPermissions.count(ExtensionPermissionTypes::Keybinds)) return;
		for (Extension::HeldKey& held : ext.keys) {
			bool allowedKey = std::find(ext.allowedKeys.begin(), ext.allowedKeys.end(), held.name) != ext.allowedKeys.end();
			if (!held.bound.empty() || held.key == 0 || !allowedKey) continue;
			std::string whyNot;
			held.bound = Keybindings::CreateKeybind(held.key, held.callback, Keybindings::KeybindFlags_ProcessWhileHidden, held.modifiers, &whyNot);
			if (held.bound.empty()) std::cerr << "[" << ext.name << "] RegisterKeybind: " << whyNot << std::endl;
		}
	}

	void switchPermission(Extension& ext, ExtensionPermissionTypes permission, bool on) {
		const PermissionInfo* info = findPermission(permission);
		if (!info) return;
		if (on) {
			allow(ext, permission);
		}
		else {
			ext.extensionPermissions.erase(permission);
			ext.switchedOff.insert(info->name);
		}
		if (permission != ExtensionPermissionTypes::Keybinds) return;
		if (on) {
			// switched on by the player: the keys said no to are asked again
			ext.refusedKeys.clear();
			bindAllowedKeys(ext);
			askWaitingKeys(ext);
		}
		else {
			// they wait again, and come back with the switch
			for (Extension::HeldKey& held : ext.keys) {
				if (!held.bound.empty()) Keybindings::DeleteKeybind(held.bound);
				held.bound.clear();
			}
		}
	}

	void letAsk(Extension& ext, const std::string& name) {
		ext.neverAsk.erase(name);
		const PermissionInfo* info = findPermission(name);
		if (info) ext.refused.erase(info->type);
	}
}
