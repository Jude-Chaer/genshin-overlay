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
		Keybinds,
		MouseAnywhere
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
		{ExtensionPermissionTypes::MouseAnywhere, "mouse_anywhere", "Mouse anywhere"},
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

	// The guard: one call into an extension (Update, a key, Init...) may run
	// this many Lua steps, counted every LuaStepCheck. Time inside overlay
	// functions isn't counted, so a slow Net_Get doesn't stop it. Its Lua may
	// hold LuaMemoryLimit at most. Past either it's stopped (see stopReason).
	// main.lua and Init run once at start and get LuaStartStepLimit.
	inline const long long LuaStepLimit = 50'000'000;
	inline const long long LuaStartStepLimit = 500'000'000;
	inline const int LuaStepCheck = 10000;
	inline const size_t LuaMemoryLimit = (size_t)256 * 1024 * 1024;

	// what an extension's Lua holds, counted by its allocator
	struct LuaMemory {
		size_t used = 0;
		size_t limit = LuaMemoryLimit;
		// the last growth was refused, Lua asks again once after a full collect
		bool refused = false;
		// refused twice in a row: it holds too much
		bool hit = false;
		// the overlay builds values outside a protected call, a refusal there
		// can't be caught: it's let through and counted as a hit instead
		bool hostSide = false;
		lua_State* lua = nullptr;
	};

	// Wraps the overlay building Lua values for an extension outside a Lua call
	struct HostAllocations {
		LuaMemory* memory;
		explicit HostAllocations(LuaMemory* memory) : memory(memory) { if (memory) memory->hostSide = true; }
		~HostAllocations() { if (memory) memory->hostSide = false; }
		HostAllocations(const HostAllocations&) = delete;
		HostAllocations& operator=(const HostAllocations&) = delete;
	};

	// How many of each one extension may have. Past that the call is refused
	// and the log says so once.
	inline const size_t MaxKeys = 32;
	inline const size_t MaxIntervals = 64;
	inline const size_t MaxTextures = 256;
	inline const size_t MaxSettings = 128;
	// a text setting's default, the same as the box the player types in
	inline const size_t MaxSettingText = 256;
	// print lines a minute, the ones past it are left out
	inline const int MaxPrintsPerMinute = 200;

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
		// Unloads it first (see unload), then closes its Lua state as this
		// extension with closing set: Lua's finalizers (__gc) run then, and
		// every overlay function refuses them, or an interval or keybind they
		// made would call into the closed state.
		~Extension();
		bool closing = false;

		// before luaState, its allocator counts here until the state is gone
		std::unique_ptr<LuaMemory> memory;
		std::unique_ptr<sol::state> luaState;
		// Set by the guard: why it was stopped. Nothing of it is called from
		// then on, and at the next frame it's unloaded and its Lua state closed.
		std::string stopReason;
		// the guard stopped it for its memory, not its steps
		bool stoppedForMemory = false;
		// main.lua, Init or its files failed: the error with its file and line,
		// shown on its page. Its Lua state is closed.
		std::string startError;
		// the last error one of its functions gave, logged once while it repeats
		std::string lastError;
		// [Start again] was pressed: it's loaded again at the next frame
		bool startAgain = false;
		// calls into it running now, nested ones too, and the Lua steps the
		// outermost one took
		int callDepth = 0;
		long long luaSteps = 0;
		long long stepLimit = LuaStepLimit;
		std::string name;
		std::string description;
		std::string author;
		std::string version;
		std::string extensionHash;
		// each .lua file in the hash (its path in the folder, UTF-8 with '/') and
		// its own hash: require runs only these, and only as they were at load
		std::vector<std::pair<std::string, std::string>> luaFiles;
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
		// print lines in the minute since printMinute (GetTickCount64)
		unsigned long long printMinute = 0;
		int printsThisMinute = 0;
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
	// its Init, with the steps of a start
	extern void runInit(Extension& ext);
	extern void destroyExtensions();
	extern void frameUpdateExtensions();
	extern void drawExtensionMenus(int extensionIndex);
	extern void findAndLoadExtensions();
	// Folders in root with a main.lua that no loaded extension came from are
	// loaded and started, once main.lua has gone unwritten for settled. How
	// many were.
	extern int findNewExtensions(const std::filesystem::path& root, std::chrono::seconds settled);
	extern void configureSettings();
	// every extension's settings into settings.json, only the ones not at their default
	extern void saveSettings();
	// a Lua state as every extension gets one: the safe libraries, require() from
	// its folder, the guard, and every overlay function. With memory, the state counts
	// what it holds there and holds no more than memory->limit.
	extern std::unique_ptr<sol::state> newLuaState(const std::filesystem::path& folder, LuaMemory* memory = nullptr);
	// the extensions the guard stopped since the last frame are unloaded and
	// their Lua states closed (frameUpdateExtensions runs it first)
	extern void unloadGuardStopped();
	extern void loadLuaExtension(const std::filesystem::path& scriptPath, const std::filesystem::path& folderPath);
	// "attempt to call a nil value (main.lua:3)" from a Lua error in its folder
	extern std::string errorLine(const Extension& ext, const std::string& error);
	// the extensions whose [Start again] was pressed are loaded again from their
	// folder, settings and Init too (frameUpdateExtensions runs it)
	extern void startAgainPressed();
	// Lua code runs as ext. Inside the frame, the clips it leaves open on the
	// background list are closed after it, and it can't close older ones.
	extern void runForExtension(Extension* ext, const std::function<void()>& function);

	// Which part of the frame runs now. ImGui has a frame only in Update and Menu,
	// and a window for widgets only in Menu. Anywhere else (Init, keys, loading,
	// closing) its functions would write through nothing.
	enum class Phase { None, Update, Menu };
	extern Phase phase;
	struct InPhase {
		Phase before;
		InPhase(Phase now) : before(phase) { phase = now; }
		~InPhase() { phase = before; }
	};
	// true in need's phase (Update also takes Menu, it's in the frame too),
	// else one log line per extension and function
	extern bool inPhase(const char* function, Phase need);
	// the background list's clips below this aren't the running callback's
	extern int clipFloor;
	// one line in the log the first time an extension hits it
	extern void logOnce(const char* function, const std::string& why);
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
	// the running extension has the permission (None always passes). Always
	// false with no extension running or while it closes.
	extern bool allowed(const char* function, ExtensionPermissionTypes permission);
}
