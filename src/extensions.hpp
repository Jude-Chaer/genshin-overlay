#pragma once
#include <GL/glew.h>
#include "controls.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <map>
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
		MouseAnywhere,
		Notices,
		Dots,
		TakeClicks,
		MapInfo
	};

	// Saved in settings.json and named in Lua by name, shown to the player by
	// label with the reason line under it. A wording change or a new permission
	// anywhere in the list leaves saved grants alone.
	struct PermissionInfo {
		ExtensionPermissionTypes type;
		const char* name;
		const char* label;
		const char* reason;
	};

	inline const std::vector<PermissionInfo> permissionList = {
		{ExtensionPermissionTypes::DisplayOverlay, "display_overlay", "Display Overlay", "It can draw on the overlay."},
		{ExtensionPermissionTypes::MapInfo, "map_info", "Map info", "It can see where the map is, where you are and the map items."},
		{ExtensionPermissionTypes::DisplayRead, "display_read", "Display Read", "It can see the game's screen."},
		{ExtensionPermissionTypes::Keybinds, "keybinds", "Keybinds", "It takes these keys while you play. The game won't see them."},
		{ExtensionPermissionTypes::InternalReadfile, "internal_read", "Internal File Reading", "It can read files in its own folder."},
		{ExtensionPermissionTypes::InternalWritefile, "internal_write", "Internal File Writing", "It can change files in its own folder."},
		{ExtensionPermissionTypes::NetworkAccess, "network", "Network Access", "It can send and get things from any website."},
		{ExtensionPermissionTypes::MouseAnywhere, "mouse_anywhere", "Mouse anywhere", "It can see where your mouse is, even outside the game."},
		{ExtensionPermissionTypes::Notices, "notices", "Notices", "It can show you short messages."},
		{ExtensionPermissionTypes::Dots, "dots", "Attention dots", "It can mark what needs your attention."},
		{ExtensionPermissionTypes::TakeClicks, "take_clicks", "Take clicks", "Clicks on parts of what it draws go to it instead of the game, while the map or the menu is open."},
	};

	// saved names of permissions that no longer exist, dropped when grants are loaded
	inline const std::vector<std::string> retiredPermissions = { "external_read", "external_write" };

	// what a new extension may do without asking
	inline const std::unordered_set<ExtensionPermissionTypes> defaultPermissions = {
		ExtensionPermissionTypes::DisplayOverlay,
		ExtensionPermissionTypes::DisplayRead,
		ExtensionPermissionTypes::MapInfo,
		ExtensionPermissionTypes::InternalReadfile,
		ExtensionPermissionTypes::InternalWritefile,
		ExtensionPermissionTypes::Notices,
		ExtensionPermissionTypes::Dots
	};

	inline const int PermissionCount = (int)ExtensionPermissionTypes::MapInfo + 1;

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
	// metadata.description and metadata.about are cut past this many letters
	inline const size_t DescriptionLetters = 150;
	inline const size_t AboutLetters = 4000;
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
	inline const size_t MaxTabs = 16;
	// headers whose open state is kept, and their labels' length
	inline const size_t MaxHeaders = 64;
	inline const size_t MaxHeaderLabel = 128;
	// headers past those, kept open or closed for the session only
	inline const size_t MaxSessionHeaders = 256;
	// attention dots on at once, and how long their part is open before one goes
	inline const size_t MaxDots = 3;
	inline const double DotSeenAfter = 2.0;
	// a text setting's default, the same as the box the player types in
	inline const size_t MaxSettingText = 256;
	// print lines a minute, the ones past it are left out
	inline const int MaxPrintsPerMinute = 200;

	inline const int ExtensionWriteLimit = 1024 * 1024 * 512; // 512MB. Write limits will be enforced only on every load, so restarting the program will reset the limit.  This is simply so that extensions don't write a ton of data to disk at once as a security measure.

	// What an extension's calls take, overlay functions inside them too (a
	// blocking Net_Get counts). Added up per second of the overlay's turns,
	// drawn frames and the turns with nothing drawn alike: keys and triggers
	// run in both.
	inline const double SlowMsPerFrame = 2.0;
	inline const int SlowSeconds = 10;
	inline const int CostSecondsShown = 3;
	struct Cost {
		// this second so far
		double ms = 0;
		// the last seconds counted, for perFrame
		double secondMs[CostSecondsShown] = {};
		int secondFrames[CostSecondsShown] = {};
		int secondsKept = 0;
		int nextSecond = 0;
		// ms per frame over the seconds kept, -1 before the first one
		double perFrame = -1;
		// seconds in a row over SlowMsPerFrame
		int slowSeconds = 0;
		// it was slow SlowSeconds in a row at this start
		bool slow = false;
		// its first second is over; that one has its start in it and isn't counted
		bool started = false;
	};
	// One counted second with this many turns. True the first time it makes
	// the extension slow.
	extern bool endSecond(Cost& cost, int frames);
	// a second that isn't counted: the slow run ends, what's shown stays
	extern void skipSecond(Cost& cost);

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
		// its button or key function runs now: the player started it, so it
		// may open a file dialog
		int playerAction = 0;
		Cost cost;
		// when each permission was last used, seconds since 1970, 0 for not yet
		long long lastUsed[PermissionCount] = {};
		// its headers the player opened or closed, by label, saved by folder
		std::map<std::string, bool> openHeaders;
		// the ones past MaxHeaders or with longer labels, for this session
		std::map<std::string, bool> sessionHeaders;
		// an open header's card is being drawn, ended by the next header,
		// ImGUI_EndHeader or the end of the call
		bool headerCard = false;
		struct DotSeen {
			// when its part opened, and the frame it was last open
			double since = -1;
			int lastFrame = -1;
		};
		// its attention dots by the tab or header they're on, for this session
		std::map<std::string, DotSeen> dots;
		std::string name;
		// metadata.description under its name, metadata.about on its About tab
		std::string description, about;
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
		// nothing was saved for its folder yet: the first start
		bool firstStart = false;
		// by permission name, saved: "Never ask again", and switched off on its
		// Permissions tab (never asked again either)
		std::unordered_set<std::string> neverAsk;
		std::unordered_set<std::string> switchedOff;
		// "Don't allow" or "Not now", asked again at the next start
		std::unordered_set<ExtensionPermissionTypes> refused;
		// the keys it may take by their shown name (Ctrl+F5), saved, and the
		// ones refused this start
		std::vector<std::string> allowedKeys;
		std::unordered_set<std::string> refusedKeys;
		// the player's own keys by the author's key name, "" for none, saved
		std::map<std::string, std::string> pickedKeys;
		// overlay settings it asked for and was refused this start
		std::unordered_set<std::string> refusedSettings;
		// the permissions its Lua files call for, and the ones it was refused
		// that they didn't show (saved): its Permissions tab lists them
		std::unordered_set<ExtensionPermissionTypes> uses;
		// refused while main.lua's top level ran, asked for once it has loaded
		std::vector<ExtensionPermissionTypes> askWhenLoaded;
		// the author's reason for each permission metadata.permissions lists
		std::unordered_map<ExtensionPermissionTypes, std::string> reasons;
		// network's sites in metadata.permissions: it reaches only those and
		// the names under them, when sitesOnly
		bool sitesOnly = false;
		std::vector<std::string> sites;
		// main.lua has run and its metadata was read: until then it reaches no site
		bool listRead = false;
		// false: switched off on its Permissions tab, it doesn't load at start
		bool enabled = true;
		// switched off while it runs, nothing of it is called anymore
		bool stopped = false;
		// print lines in the minute since printMinute (GetTickCount64)
		unsigned long long printMinute = 0;
		int printsThisMinute = 0;
		// the functions refused for a missing permission, so each is logged once
		std::unordered_set<std::string> refusalsLogged;
		// A key it registered, by the id RegisterKeybind gave. It's bound once
		// the player allowed Keybinds and this key, until then it waits.
		struct HeldKey {
			std::string id;
			// RegisterKeybind's label for its row on the Keys tab, can be empty
			std::string label;
			// the author's key, the default
			int authorKey = 0;
			int authorModifiers = 0;
			std::string authorName;
			// the key it has now, the author's or the player's, 0 and "" for none
			std::string name;
			int key = 0;
			int modifiers = 0;
			std::function<void()> callback;
			// the keybind's id while it's bound
			std::string bound;
		};
		std::vector<HeldKey> keys;
		// the textures it loaded with LoadTexture or from a web link, freed with it
		std::vector<GLuint> textures;
		// ImGUI_Image's web links this session, 0 while downloading
		struct WebImage {
			GLuint texture = 0;
			bool failed = false;
		};
		std::map<std::string, WebImage> webImages;
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
	// the notice that the extension slows the overlay down, with a button to its page
	extern void tellSlow(const Extension& ext);
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
	// One turn of the overlay's loop for the cost. Each second the costs are
	// counted, and an extension that turns slow is logged. Each extension's
	// first second, with its start in it, isn't counted, Start again included.
	extern void countTurn();

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

	// What PermissionState gives for a permission: "allowed", "off" (switched
	// off on its Permissions tab), "never" (Never ask again), "refused" (this
	// start), or "not_asked".
	extern std::string permissionState(const Extension& ext, ExtensionPermissionTypes permission);
	// Whether asking for it would show a question: not allowed, off, never or
	// refused already.
	extern bool mayAsk(const Extension& ext, ExtensionPermissionTypes permission);
	// the player's answers, saved
	extern void allow(Extension& ext, ExtensionPermissionTypes permission);
	extern void refuse(Extension& ext, ExtensionPermissionTypes permission, bool never);
	// its switch on the Permissions tab
	extern void switchPermission(Extension& ext, ExtensionPermissionTypes permission, bool on);
	// "Let it ask"
	extern void letAsk(Extension& ext, const std::string& name);
	// the Permissions tab on its page
	extern void drawPermissionsTab(Extension& ext);
	// its About tab: metadata.about, version, author, folder
	extern void drawAboutTab(const Extension& ext);
	// Switched off on its Permissions tab: Shutdown runs, everything it
	// registered goes (unload), and nothing of it is called again.
	extern void stopExtension(Extension& ext);
	// The switch on its Permissions tab, saved. Off stops it now; on loads it
	// again from its folder at the next frame, as [Start again] does.
	extern void setRunning(Extension& ext, bool on);
	// binds its waiting keys the player allowed; one that can't be bound is
	// dropped with a log line
	extern void bindAllowedKeys(Extension& ext);
	// asks for its keys that wait, the ones not refused at this start
	extern void askWaitingKeys(Extension& ext);
	extern void loadExtensionPermissions(Extension* ext);
	// metadata.permissions = { network = { reason = "..." } }: the author's
	// reasons go in ext.reasons, a name nothing has is logged
	extern void readListedPermissions(Extension& ext, const sol::table& list);
	// At the first start it's asked for what it lists and doesn't have. After
	// its files changed, a new one is asked after "Allow the same again".
	extern void askListedPermissions(Extension& ext);
	// the line under a permission in the question and on the tab, like "It
	// can talk to: hoyolab.com" for network with sites
	extern std::string reasonLine(const Extension& ext, const PermissionInfo& permission);
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
	// A call that needs this was refused: it's in uses from now on, and the
	// player is asked for it (keys are asked one by one instead).
	extern void wanted(Extension& ext, ExtensionPermissionTypes permission);
	// the permissions Lua code calls for: the functions it names, the triggers
	// and permission names in its strings; comments are left out
	extern std::unordered_set<ExtensionPermissionTypes> callsFor(const std::string& code);
	// For "Used 5 min ago": allowed counts each use, and the overlay calls this
	// where a permission is used outside a function (a key pressed, an answer
	// from a website, a trigger that reads the map).
	extern void markUsed(Extension& ext, ExtensionPermissionTypes permission);
	// "Used 5 min ago" for its row on the Permissions tab, "Not used yet" or ""
	// (not allowed and never used) at now (seconds since 1970)
	extern std::string lastUsedLine(const Extension& ext, ExtensionPermissionTypes permission, long long now);
	// every extension's last uses into settings.json (destroyExtensions runs it)
	extern void saveLastUsed();
}
