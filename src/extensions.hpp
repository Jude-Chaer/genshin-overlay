#pragma once
#include <GL/glew.h>
#include "controls.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <sol/sol.hpp>
#include <unordered_set>

namespace Extensions {
	enum class ExtensionSettingTypes {
		Bool,
		Int,
		Float,
		String
	};

	enum class ExtensionPermissionTypes {
		None,
		InternalReadfile,
		InternalWritefile,
		NetworkAccess,
		DisplayOverlay,
		DisplayRead,
		Keybinds
	};

	// Saved in settings.json by name, shown to the player by label. A wording
	// change or a new permission anywhere in the list leaves saved grants alone.
	struct PermissionInfo {
		ExtensionPermissionTypes type;
		const char* name;
		const char* label;
	};

	inline const std::vector<PermissionInfo> permissionList = {
		{ExtensionPermissionTypes::DisplayOverlay, "display_overlay", "Display Overlay"},
		{ExtensionPermissionTypes::DisplayRead, "display_read", "Display Read"},
		{ExtensionPermissionTypes::Keybinds, "keybinds", "Keybinds"},
		{ExtensionPermissionTypes::InternalReadfile, "internal_read", "Internal File Reading"},
		{ExtensionPermissionTypes::InternalWritefile, "internal_write", "Internal File Writing"},
		{ExtensionPermissionTypes::NetworkAccess, "network", "Network Access"},
	};

	// saved names of permissions that no longer exist, dropped when grants are loaded
	inline const std::vector<std::string> retiredPermissions = { "external_read", "external_write" };

	// what a new extension may do without asking: everything but the internet
	inline const std::unordered_set<ExtensionPermissionTypes> defaultPermissions = {
		ExtensionPermissionTypes::DisplayOverlay,
		ExtensionPermissionTypes::DisplayRead,
		ExtensionPermissionTypes::Keybinds,
		ExtensionPermissionTypes::InternalReadfile,
		ExtensionPermissionTypes::InternalWritefile
	};

	// nullptr for None and for a name this overlay doesn't know
	const PermissionInfo* findPermission(ExtensionPermissionTypes type);
	const PermissionInfo* findPermission(const std::string& name);

	inline const int ExtensionWriteLimit = 1024 * 1024 * 512; // 512MB. Write limits will be enforced only on every load, so restarting the program will reset the limit.  This is simply so that extensions don't write a ton of data to disk at once as a security measure.

	struct ExtensionSetting {
		std::string id;
		std::string label;
		ExtensionSettingTypes type;

		std::variant<bool, int, float, std::string> defaultValue;
		std::variant<bool, int, float, std::string> value;
		// a number with both of these set gets a slider
		std::optional<float> min, max;
		// how far - and + move it, none: the number piece's own
		std::optional<float> step;
		// a typed box only, no slider
		bool field = false;
		// a text typed and not saved yet, saved once the box is left
		bool edited = false;
	};

	// the number piece's options for a number setting, its range, step and default
	Controls::NumberOptions numberOptions(const ExtensionSetting& setting);
	// a number outside its range comes back inside it
	void fitToRange(ExtensionSetting& setting);
	// the setting's piece; true once a change is done (let go of the slider,
	// clicked, typed and left the box), so it gets saved then
	bool drawSetting(ExtensionSetting& setting);

	class Extension {
	public:
		Extension() = default;
		Extension(const Extension&) = delete;
		Extension& operator=(const Extension&) = delete;
		// unloads it first, see unload
		~Extension();

		std::unique_ptr<sol::state> luaState;
		std::string name;
		std::string description;
		std::string author;
		std::string version;
		std::string extensionHash;
		std::unordered_set<ExtensionPermissionTypes> extensionPermissions;
		// grants saved by a newer overlay that this one doesn't know, written back as they are
		std::vector<std::string> unknownGrants;
		// what was allowed before the extension changed (its files have a new hash), for
		// asking "Allow the same again?". Empty when nothing changed. Until then it runs
		// with the defaults it still had.
		std::vector<std::string> grantsBeforeChange;
		std::string hashBeforeChange;
		// switched off while it runs, nothing of it is called anymore
		bool stopped = false;
		// the functions refused for a missing permission, so each is logged once
		std::unordered_set<std::string> refusalsLogged;
		// the ids of the keybinds it made, removed with it
		std::vector<std::string> keybinds;
		// the textures it loaded with LoadTexture, freed with it
		std::vector<GLuint> textures;
		std::filesystem::path folder;
		// its folder's name, the key for everything it keeps in settings.json
		std::string folderKey;
		std::unordered_map<std::string, ExtensionSetting> settings;
		std::vector<std::string> settingsOrder;
		std::function<void()> initializeFunction;
		std::function<void()> shutdownFunction;
		std::function<void()> registerSettings;
		std::function<void()> updateFunction;
		std::function<void()> menuFunction;
		int bytesToDiskWritten = 0;
		GLuint extensionImage = 0;
		int extensionImageWidth = 0;
		int extensionImageHeight = 0;
	};


	extern std::vector<std::unique_ptr<Extension>> registeredExtensions;
	extern sol::state globalLuaState;
	extern Extension* currentExtension;
	extern void initExtensions();
	extern void destroyExtensions();
	extern void frameUpdateExtensions();
	extern void drawExtensionMenus(int extensionIndex);
	extern void findAndLoadExtensions();
	// Folders in root with a main.lua that no loaded extension came from are
	// loaded and started, once main.lua has gone unwritten for settled. How
	// many were.
	extern int findNewExtensions(const std::filesystem::path& root, std::chrono::seconds settled);
	extern void configureSettings();
	// a Lua state as every extension gets one: the safe libraries, require() from
	// its folder, and every overlay function
	extern std::unique_ptr<sol::state> newLuaState(const std::filesystem::path& folder);
	extern void loadLuaExtension(const std::filesystem::path& scriptPath, const std::filesystem::path& folderPath);
	extern void runForExtension(Extension* ext, const std::function<void()>& function);
	extern void drawExtensionPermissionMenu();
	extern void loadExtensionPermissions(Extension* ext);
	extern void saveExtensionPermissions(const Extension& ext);

	// Everything the extension registered (keybinds, intervals) is removed, so
	// nothing calls into its Lua state after it closes. Runs before the state
	// closes: on shutdown, and when a load fails. Its Lua sockets close with the
	// state itself.
	extern void unload(Extension& ext);

	// The file an extension means by a path, relative to its folder, with the
	// internal permission for access. Nothing (and one log line) for a path
	// outside its folder or without the permission. Paths from Lua are UTF-8.
	enum class Access { Read, Write };
	extern std::optional<std::filesystem::path> pathFor(Extension& ext, const std::string& path, Access access);
	// for the extension running now
	extern std::optional<std::filesystem::path> pathFor(const std::string& path, Access access);

	// One log line per function per extension when a call is refused. True when
	// the running extension has the permission (None always passes).
	extern bool allowed(const char* function, ExtensionPermissionTypes permission);
}
