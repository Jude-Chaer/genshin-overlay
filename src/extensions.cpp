#include <Windows.h>
#include "extensions.hpp"
#include "files.hpp"
#include "helpers.hpp"
#include "keybindings.hpp"
#include "lua_functions.hpp"
#include "lua_patterns.hpp"
#include "settings.hpp"
#include "typing.hpp"
#include "controls.hpp"
#include <imgui.h>
#include <algorithm>
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

		auto restore = [&]() {
			if (ext) {
				ext->callDepth--;
				// Lua's memory error can end the call before the guard's next step
				noteMemoryHit(*ext);
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
		if (ext->extensionPermissions.count(permission)) return true;

		if (ext->refusalsLogged.insert(function).second) {
			const PermissionInfo* info = findPermission(permission);
			std::cerr << "[" << ext->name << "] " << function << " needs " << (info ? info->label : "a permission")
				<< ", which it isn't allowed" << std::endl;
		}
		return false;
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
		if (ext.extensionPermissions.count(needed)) return file;

		const PermissionInfo* info = findPermission(needed);
		std::string label = info ? info->label : "a permission";
		if (ext.refusalsLogged.insert("path " + label).second) {
			std::cerr << "[" << ext.name << "] " << path << " needs " << label << ", which it isn't allowed" << std::endl;
		}
		return std::nullopt;
	}

	std::optional<fs::path> pathFor(const std::string& path, Access access) {
		if (!currentExtension) return std::nullopt;
		return pathFor(*currentExtension, path, access);
	}

	void unload(Extension& ext) {
		for (const std::string& id : ext.keybinds) {
			Keybindings::DeleteKeybind(id);
		}
		ext.keybinds.clear();
		LuaFunctions::forgetIntervals(&ext);
		if (!ext.textures.empty()) {
			glDeleteTextures((GLsizei)ext.textures.size(), ext.textures.data());
			ext.textures.clear();
		}

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
			runForExtension(ext.get(), ext->updateFunction);
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

		ImGui::TextDisabled(
			"%s  v%s",
			ext->author.c_str(),
			ext->version.c_str()
		);

		ImGui::EndGroup();
		ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));

		ImGui::Separator();

		if (!ext->stopReason.empty() || !ext->startError.empty()) {
			if (!ext->stopReason.empty())
				ImGui::TextWrapped("Stopped: %s", ext->stoppedForMemory ? "it used too much memory" : "it ran too long");
			else
				ImGui::TextWrapped("Couldn't start: %s", ext->startError.c_str());
			if (Controls::Button("Start again")) ext->startAgain = true;
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
			ext->description = meta->get_or("description", std::string("No description."));
			ext->author = meta->get_or("author", std::string("Unknown"));
			ext->version = meta->get_or("version", std::string("1.0.0"));
		}

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

	void loadLuaExtension(const fs::path& scriptPath, const fs::path& folderPath) {
		registeredExtensions.push_back(loadExtension(scriptPath, folderPath));
	}

	void startAgainPressed() {
		for (auto& slot : registeredExtensions) {
			if (!slot->startAgain) continue;
			fs::path folder = slot->folder;
			std::cout << "Starting again: " << slot->name << std::endl;
			slot.reset();
			slot = loadExtension(folder / "main.lua", folder);
			runForExtension(slot.get(), slot->registerSettings);
			loadSettings();
			runInit(*slot);
		}
	}

	void drawExtensionPermissionMenu() {
		ImGui::TextWrapped(
			"Extensions can be given permissions to access certain features.  Some of these are sensitive and should be granted with caution.");
		for (auto& ext : registeredExtensions) {
			ImGui::Separator();
			ImGui::Text("%s", ext->name.c_str());
			// the name is the extension's own word, its folder tells two with the same name apart
			ImGui::SameLine();
			ImGui::TextDisabled("(folder: %s)", ext->folderKey.c_str());
			ImGui::TextDisabled("%s  v%s", ext->author.c_str(), ext->version.c_str());
			ImGui::TextWrapped("%s", ext->description.c_str());
			ImGui::Separator();
			ImGui::PushID(ext.get());
			for (const auto& permission : permissionList) {
				bool hasPermission = ext->extensionPermissions.count(permission.type) > 0;
				if (ImGui::Checkbox(permission.label, &hasPermission)) {
					if (hasPermission) {
						ext->extensionPermissions.insert(permission.type);
					}
					else {
						ext->extensionPermissions.erase(permission.type);
					}
					saveExtensionPermissions(*ext);
				}
			}
			ImGui::PopID();
		}
	}

	static bool isRetired(const std::string& name) {
		return std::find(retiredPermissions.begin(), retiredPermissions.end(), name) != retiredPermissions.end();
	}

	// Saved grants by folder. None saved: the defaults. Saved for other files
	// (the extension changed since): the defaults too, and the old ones are kept
	// for asking to allow the same again.
	void loadExtensionPermissions(Extension* ext) {
		Settings::Grants grants = Settings::LoadGrants(ext->folderKey, ext->extensionHash);
		ext->extensionPermissions.clear();
		ext->unknownGrants.clear();
		ext->grantsBeforeChange.clear();
		ext->hashBeforeChange.clear();

		if (!grants.saved) {
			ext->extensionPermissions = defaultPermissions;
			return;
		}
		if (grants.hash != ext->extensionHash) {
			// the defaults, but nothing the player had taken away
			for (const std::string& name : grants.names) {
				const PermissionInfo* permission = findPermission(name);
				if (permission && defaultPermissions.count(permission->type)) ext->extensionPermissions.insert(permission->type);
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

	void saveExtensionPermissions(const Extension& ext) {
		Settings::Grants grants;
		grants.saved = true;
		grants.hash = ext.extensionHash;
		for (const auto& permission : permissionList) {
			if (ext.extensionPermissions.count(permission.type)) grants.names.push_back(permission.name);
		}
		for (const std::string& name : ext.unknownGrants) grants.names.push_back(name);
		Settings::SaveGrants(ext.folderKey, grants);
	}
}
