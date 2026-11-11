#include <Windows.h>
#include "extensions.hpp"
#include "files.hpp"
#include "helpers.hpp"
#include "keybindings.hpp"
#include "lua_functions.hpp"
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
		if (permission == ExtensionPermissionTypes::None) return true;
		Extension* ext = currentExtension;
		if (!ext) return false;
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

	// The file a UTF-8 path means, relative to root (canonical). Nothing for a
	// network or device path (\\server\share, //server, \\?\): looking one up
	// already connects to the server. Nothing for a path that leaves root, and
	// outside says so; one that leaves it by its text isn't looked up.
	static std::optional<fs::path> resolve(const fs::path& root, const std::string& path, bool& outside) {
		outside = false;
		try {
			fs::path asked = Helpers::fromUtf8(path);
			if (asked.has_root_name()) {
				std::wstring name = asked.root_name().native();
				if (name.size() >= 2 && (name[0] == L'\\' || name[0] == L'/') && (name[1] == L'\\' || name[1] == L'/')) return std::nullopt;
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

	// the members go after this, the Lua state last
	Extension::~Extension() {
		unload(*this);
	}

	void destroyExtensions() {
		for (auto& ext : registeredExtensions) {
			runForExtension(ext.get(), ext->shutdownFunction);
		}
		registeredExtensions.clear();
	}

	void frameUpdateExtensions() {
		LuaFunctions::Items::beginFrame();
		LuaFunctions::UpdateIntervals();
		for (auto& ext : registeredExtensions) {
			runForExtension(ext.get(), ext->updateFunction);
		}
	}

	// kept per extension folder in settings.json
	static void saveSettings() {
		nlohmann::json all = Settings::LoadExtensions();
		for (auto& ext : registeredExtensions) {
			nlohmann::json& saved = all[ext->folderKey];
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
			runForExtension(&ext, ext.initializeFunction);
			loaded++;
		}
		return loaded;
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

	// require() looks in the extension's own folder. Lua's own searcher opens
	// files by code page names, which can't hold every folder name.
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

			sol::state_view view(state);
			std::string code = Files::readText(file);
			sol::load_result chunk = view.load_buffer(code.data(), code.size(), "@" + relative);
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

	std::unique_ptr<sol::state> newLuaState(const fs::path& folder) {
		auto lua = std::make_unique<sol::state>();
		lua->open_libraries(
			sol::lib::base,
			sol::lib::package,
			sol::lib::math,
			sol::lib::string,
			sol::lib::table
		);
		lua->globals()["load"] = sol::nil;
		lua->globals()["loadfile"] = sol::nil;
		lua->globals()["dofile"] = sol::nil;
		lua->globals()["loadstring"] = sol::nil;
		lua->globals()["package"]["path"] = "";
		lua->globals()["package"]["cpath"] = "";
		lua->globals()["package"]["loadlib"] = sol::nil;
		requireFromFolder(*lua, folder);
		LuaFunctions::registerLuaFunctions(*lua);
		return lua;
	}

	// ImGui hides what comes after ## in a button, the name is shown whole
	static std::string shownName(std::string name) {
		for (size_t at = name.find("##"); at != std::string::npos; at = name.find("##", at)) name.replace(at, 2, "# #");
		return name;
	}

	void loadLuaExtension(const fs::path& scriptPath, const fs::path& folderPath) {
		auto ext = std::make_unique<Extension>();
		ext->folder = folderPath;
		ext->folderKey = Helpers::utf8(folderPath.filename());
		ext->name = shownName(ext->folderKey);

		ext->luaState = newLuaState(folderPath);

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
				return;
			}

			luaFileHashes.emplace_back(relative, fileHash);
		}
		// a folder it can't look through can't be hashed whole
		if (walkError) {
			std::cerr << "Failed to hash " << Helpers::utf8(folderPath) << ": " << walkError.message() << std::endl;
			return;
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

		env["WORKING_DIR"] = Helpers::utf8(folderPath) + "/";

		// read here, Lua's own loader opens files by code page names
		std::string code = Files::readText(scriptPath);
		sol::load_result script = ext->luaState->load_buffer(code.data(), code.size(),
			"@" + ext->folderKey + "/" + Helpers::utf8(scriptPath.filename()));
		if (!script.valid()) {
			sol::error err = script;
			std::cerr << "Failed to load " << Helpers::utf8(scriptPath) << ": " << err.what() << std::endl;
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
			std::cerr << "Failed to run " << Helpers::utf8(scriptPath) << ": " << err.what() << std::endl;
			return;
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
