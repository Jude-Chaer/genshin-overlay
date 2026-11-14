#include "triggers.hpp"
#include "screen_pictures.hpp"
#include "asks.hpp"
#include "item_catalog.hpp"
#include "lua_functions.hpp"
#include "overlay.hpp"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>

namespace Triggers {
	using Extensions::Extension;
	using Permission = Extensions::ExtensionPermissionTypes;

	enum class Kind { Screens, ScreenPicture, MapView, Player, Items, Page, Setting, Permission, Look };

	static const std::vector<std::pair<std::string, Kind>> kinds = {
		{ "Screens", Kind::Screens },
		{ "ScreenPicture", Kind::ScreenPicture },
		{ "MapView", Kind::MapView },
		{ "Player", Kind::Player },
		{ "Items", Kind::Items },
		{ "Page", Kind::Page },
		{ "Setting", Kind::Setting },
		{ "Permission", Kind::Permission },
		{ "Look", Kind::Look },
	};

	const std::vector<std::string>& names() {
		static const std::vector<std::string> list = [] {
			std::vector<std::string> all;
			for (const auto& [name, kind] : kinds) all.push_back(name);
			return all;
		}();
		return list;
	}

	static std::vector<std::string> changes(Kind kind) {
		switch (kind) {
		case Kind::Screens:
		case Kind::ScreenPicture: {
			std::vector<std::string> all;
			for (const Screens::Known& known : Screens::list) {
				if (kind == Kind::Screens || known.picture) all.push_back(known.name);
			}
			return all;
		}
		case Kind::MapView: return { "view", "floor" };
		case Kind::Player: return { "found", "lost", "moved" };
		case Kind::Items: return { "list", "loading" };
		case Kind::Page: return { "shown", "hidden" };
		case Kind::Look: return { "look", "dark", "accent", "size" };
		default: return {};
		}
	}

	std::vector<std::string> changes(const std::string& name) {
		auto kind = std::find_if(kinds.begin(), kinds.end(), [&](const auto& each) { return each.first == name; });
		return kind == kinds.end() ? std::vector<std::string>{} : changes(kind->second);
	}

	struct Trigger {
		int id = 0;
		Kind kind = Kind::Screens;
		std::string name;
		// runs only when this changed, "" for every change
		std::string only;
		Extension* owner = nullptr;
		sol::protected_function function;
		// what it saw at the last check, by part. Empty until the first check.
		bool started = false;
		std::map<std::string, std::string> seen;
	};

	static std::vector<Trigger> triggers;
	static int nextId = 1;

	// the map, the player and the items are what Map info lets it see
	static bool needsMapInfo(Kind kind) {
		return kind == Kind::MapView || kind == Kind::Player || kind == Kind::Items;
	}

	static Permission needs(Kind kind) {
		if (kind == Kind::ScreenPicture) return Permission::DisplayRead;
		return needsMapInfo(kind) ? Permission::MapInfo : Permission::None;
	}

	Permission permissionFor(const std::string& name) {
		for (const auto& [each, kind] : kinds) {
			if (each == name) return needs(kind);
		}
		return Permission::None;
	}

	static bool mayRun(const Trigger& trigger) {
		const Extension* ext = trigger.owner;
		if (!ext || ext->stopped || ext->closing || !ext->stopReason.empty()) return false;
		if (trigger.kind == Kind::ScreenPicture) return ext->extensionPermissions.count(Permission::DisplayRead) > 0;
		return !needsMapInfo(trigger.kind) || ext->extensionPermissions.count(Permission::MapInfo) > 0;
	}

	static int pageIndex(const Extension* ext) {
		auto& all = Extensions::registeredExtensions;
		for (size_t i = 0; i < all.size(); ++i) {
			if (all[i].get() == ext) return (int)i;
		}
		return -1;
	}

	static std::string text(const std::variant<bool, int, float, std::string>& value) {
		return std::visit([](auto&& each) -> std::string {
			using V = std::decay_t<decltype(each)>;
			if constexpr (std::is_same_v<V, std::string>) return "s" + each;
			else return "n" + std::to_string(each);
			}, value);
	}

	// everything GetMapView gives, the ui boxes too
	static std::string viewText(const State& now) {
		const MapTracking::MapView& view = now.view;
		if (!view.visible) return "";
		int panelLeft = view.panel.seen == GameUi::Seen::Read ? view.panel.left : -1;
		const RECT& picker = now.picker;
		char text[512];
		snprintf(text, sizeof(text), "%d %d %d %d %.9g %.9g %.9g %ld %ld %ld %ld %d %d %d %d %ld %ld %ld %ld",
			view.result.mapId, view.result.groupId, view.result.floorId, (int)view.layerFrom,
			view.result.lat, view.result.lng, view.result.unitsPerPixel,
			view.gameRect.left, view.gameRect.top, view.gameRect.right, view.gameRect.bottom,
			view.moving ? 1 : 0, (int)view.panel.seen, (int)view.panel.kind, panelLeft,
			picker.left, picker.top, picker.right, picker.bottom);
		return text;
	}

	// a part as it was at the last check, nothing when it wasn't seen yet
	static void keep(std::map<std::string, std::string>& parts, const Trigger& trigger, const std::string& part) {
		auto it = trigger.seen.find(part);
		if (it != trigger.seen.end()) parts[part] = it->second;
	}

	// what the trigger looks at now, by part
	static std::map<std::string, std::string> look(const Trigger& trigger, const State& now) {
		const Extension& ext = *trigger.owner;
		std::map<std::string, std::string> parts;
		switch (trigger.kind) {
		case Kind::Screens:
			for (const Screens::Known& known : Screens::list) parts[known.name] = now.screens[known.screen] ? "1" : "0";
			break;
		case Kind::ScreenPicture:
			// a new picture while the screen is up
			for (const Screens::Known& known : Screens::list) {
				if (!known.picture) continue;
				uint64_t frame = now.pictures[(size_t)known.screen];
				parts[known.name] = now.screens[known.screen] && frame != 0 ? std::to_string(frame) : "";
			}
			break;
		case Kind::MapView:
			parts["view"] = viewText(now);
			// nil while it moves isn't a new floor, the last one shown stays
			if (now.view.visible) parts["floor"] = std::to_string(now.view.result.groupId) + " " + std::to_string(now.view.result.floorId);
			else keep(parts, trigger, "floor");
			break;
		case Kind::Player: {
			const MiniMap::Spot& spot = now.player;
			parts["found"] = spot.found ? "1" : "0";
			// while lost the spot is where it was last seen
			if (spot.found) {
				char text[256];
				snprintf(text, sizeof(text), "%d %d %d %.9g %.9g", spot.mapId, spot.groupId, spot.floorId, spot.lat, spot.lng);
				parts["spot"] = text;
			}
			else keep(parts, trigger, "spot");
			break;
		}
		case Kind::Items: {
			const ItemFiles::Status& items = now.items;
			parts["list"] = std::to_string(now.itemsVersion);
			parts["loading"] = std::to_string((int)items.state) + " " + std::to_string((int)items.reason) + " " +
				std::to_string(items.percent) + " " + items.built + " " + (items.started ? "1" : "0") + (items.checking ? "1" : "0");
			break;
		}
		case Kind::Page:
			parts["shown"] = now.pageShown >= 0 && now.pageShown == pageIndex(&ext) ? "1" : "0";
			break;
		case Kind::Setting:
			for (const auto& [id, setting] : ext.settings) parts[id] = text(setting.value);
			break;
		case Kind::Permission:
			for (const auto& info : Extensions::permissionList) parts[info.name] = Extensions::permissionState(ext, info.type);
			for (const auto& name : Asks::settingNames()) parts[name] = Asks::settingState(ext, name);
			break;
		case Kind::Look:
			parts["look"] = Controls::LookName(now.look);
			parts["dark"] = now.dark ? "1" : "0";
			parts["accent"] = std::to_string(now.accent);
			parts["size"] = std::to_string(std::lround(now.font * 100));
			break;
		}
		return parts;
	}

	// What changed between two looks. A screen, a setting or a permission is
	// one change each. For the map view, the player and the items the biggest
	// comes first: a new floor before view, found or lost before moved, a new
	// list before loading.
	static std::vector<std::string> changed(Kind kind, const std::map<std::string, std::string>& before,
		const std::map<std::string, std::string>& now) {
		std::vector<std::string> parts;
		for (const auto& [part, value] : now) {
			auto was = before.find(part);
			// a part that wasn't there before is new, not a change
			if (was != before.end() && was->second != value) parts.push_back(part);
		}
		auto has = [&](const char* part) { return std::find(parts.begin(), parts.end(), part) != parts.end(); };
		std::vector<std::string> events;
		switch (kind) {
		case Kind::MapView:
			if (has("floor")) events.push_back("floor");
			if (has("view")) events.push_back("view");
			return events;
		case Kind::Player:
			if (has("found")) events.push_back(now.at("found") == "1" ? "found" : "lost");
			if (has("spot")) events.push_back("moved");
			return events;
		case Kind::Items:
			if (has("list")) events.push_back("list");
			if (has("loading")) events.push_back("loading");
			return events;
		case Kind::Page:
			if (has("shown")) events.push_back(now.at("shown") == "1" ? "shown" : "hidden");
			return events;
		case Kind::ScreenPicture:
			for (const std::string& part : parts) if (!now.at(part).empty()) events.push_back(part);
			return events;
		default:
			return parts;
		}
	}

	// one topic, one run: the biggest change, or the one asked for with only
	static bool oneRun(Kind kind) {
		return kind == Kind::MapView || kind == Kind::Player || kind == Kind::Items;
	}

	struct Due {
		int id;
		std::string changed;
	};

	// the first argument: what the topic's call gives
	static sol::object given(Kind kind, const std::string& changed, const State& now, sol::state_view lua) {
		switch (kind) {
		case Kind::Screens: return LuaFunctions::screensTable(now.screens, lua);
		case Kind::ScreenPicture: return LuaFunctions::screenPictureTable(changed, ScreenPictures::DefaultWidth, now.screens, lua);
		case Kind::MapView: return LuaFunctions::mapViewTable(now.view, lua);
		case Kind::Player: return LuaFunctions::playerTable(now.player, lua);
		case Kind::Items: return LuaFunctions::Items::table(now.itemsVersion, now.items, lua);
		case Kind::Page: return sol::make_object(lua, changed == "shown");
		case Kind::Setting: return LuaFunctions::GetExtensionSetting(changed, sol::this_state{ lua.lua_state() });
		case Kind::Permission: return LuaFunctions::PermissionState(changed, sol::this_state{ lua.lua_state() });
		case Kind::Look: return LuaFunctions::themeTable(Controls::GetTheme(now.look, now.dark), lua);
		}
		return sol::make_object(lua, sol::lua_nil);
	}

	static void run(const Due& due, const State& now) {
		auto it = std::find_if(triggers.begin(), triggers.end(), [&](const Trigger& t) { return t.id == due.id; });
		if (it == triggers.end() || !mayRun(*it)) return;
		Extension* owner = it->owner;
		sol::protected_function function = it->function;
		Kind kind = it->kind;
		std::string what = "trigger " + it->name;
		if (needsMapInfo(kind)) Extensions::markUsed(*owner, Permission::MapInfo);
		if (kind == Kind::ScreenPicture) Extensions::markUsed(*owner, Permission::DisplayRead);
		Extensions::runForExtension(owner, [&]() {
			sol::state_view lua(function.lua_state());
			sol::protected_function_result result = function(given(kind, due.changed, now, lua), due.changed);
			if (!result.valid()) {
				sol::error err = result;
				std::cerr << "[" << owner->name << "] " << what << ": " << err.what() << std::endl;
			}
			});
	}

	void check(const State& now) {
		std::vector<Due> due;
		bool itemsDue = false;
		for (Trigger& trigger : triggers) {
			if (!mayRun(trigger)) {
				// it starts again from what it sees once it may, nothing from before runs
				trigger.started = false;
				trigger.seen.clear();
				continue;
			}
			std::map<std::string, std::string> seen = look(trigger, now);
			if (!trigger.started) {
				trigger.started = true;
				trigger.seen = std::move(seen);
				continue;
			}
			for (const std::string& each : changed(trigger.kind, trigger.seen, seen)) {
				if (!trigger.only.empty() && each != trigger.only) continue;
				due.push_back({ trigger.id, each });
				if (trigger.kind == Kind::Items) itemsDue = true;
				if (oneRun(trigger.kind)) break;
			}
			trigger.seen = std::move(seen);
		}
		if (due.empty()) return;
		// GetItems and the other item calls read the frame's catalog
		if (itemsDue) LuaFunctions::Items::beginFrame();
		for (const Due& each : due) run(each, now);
	}

	bool any() {
		return !triggers.empty();
	}

	static bool wants(Kind kind) {
		for (const Trigger& trigger : triggers) {
			if (trigger.kind == kind && mayRun(trigger)) return true;
		}
		return false;
	}

	bool wantsPlayer() {
		return wants(Kind::Player);
	}

	// whether a ScreenPicture trigger that may run wants this screen
	static bool wantsPicture(const char* screen) {
		for (const Trigger& trigger : triggers) {
			if (trigger.kind == Kind::ScreenPicture && (trigger.only.empty() || trigger.only == screen) && mayRun(trigger)) return true;
		}
		return false;
	}

	State current(bool wantsPlayer) {
		State now;
		now.screens = Screens::now();
		// as GetScreenPicture does, asking keeps the picture made
		for (const Screens::Known& known : Screens::list) {
			if (!known.picture || !now.screens[known.screen] || !wantsPicture(known.name)) continue;
			now.pictures[(size_t)known.screen] = ScreenPictures::get(known.screen, ScreenPictures::DefaultWidth, now.screens).frame;
		}
		now.view = MapTracking::GetView();
		// as GetPlayer does, asking keeps the minimap watched
		if (wantsPlayer) now.player = MiniMap::GetPlayer();
		if (!Overlay::PickerBox(now.picker)) now.picker = {};
		now.itemsVersion = ItemCatalog::version();
		now.items = ItemFiles::GetStatus();
		now.pageShown = Overlay::ShownExtension();
		Controls::Theme theme = Controls::GetTheme();
		now.look = theme.look;
		now.dark = theme.dark;
		now.accent = theme.accent;
		now.font = theme.font;
		return now;
	}

	void checkNow() {
		if (triggers.empty()) return;
		check(current(wantsPlayer()));
	}

	int add(const std::string& name, sol::protected_function function, sol::optional<std::string> only) {
		Extension* owner = Extensions::currentExtension;
		auto kind = std::find_if(kinds.begin(), kinds.end(), [&](const auto& each) { return each.first == name; });
		if (kind == kinds.end()) {
			Extensions::logOnce("AddTrigger", "knows no trigger named " + name);
			return 0;
		}
		if (!owner || !function.valid()) return 0;
		std::vector<std::string> known = changes(kind->second);
		if (only && !known.empty() && std::find(known.begin(), known.end(), *only) == known.end()) {
			Extensions::logOnce("AddTrigger", name + " has no change named " + *only);
			return 0;
		}
		if (count(owner) >= MaxTriggers) {
			Extensions::logOnce("AddTrigger", "has " + std::to_string(MaxTriggers) + " triggers, no more are added");
			return 0;
		}
		Trigger trigger;
		trigger.id = nextId++;
		trigger.kind = kind->second;
		trigger.name = name;
		trigger.only = only.value_or("");
		trigger.owner = owner;
		trigger.function = std::move(function);
		Permission needed = needs(trigger.kind);
		if (needed != Permission::None && !owner->extensionPermissions.count(needed)) {
			const Extensions::PermissionInfo* info = Extensions::findPermission(needed);
			Extensions::logOnce("AddTrigger", name + " needs " + info->label + ", it runs once that's allowed");
			Extensions::wanted(*owner, needed);
		}
		triggers.push_back(std::move(trigger));
		return triggers.back().id;
	}

	void remove(int id) {
		Extension* owner = Extensions::currentExtension;
		triggers.erase(std::remove_if(triggers.begin(), triggers.end(),
			[&](const Trigger& t) { return t.id == id && t.owner == owner; }), triggers.end());
	}

	void forget(Extension* owner) {
		triggers.erase(std::remove_if(triggers.begin(), triggers.end(),
			[&](const Trigger& t) { return t.owner == owner; }), triggers.end());
	}

	size_t count(const Extension* owner) {
		return (size_t)std::count_if(triggers.begin(), triggers.end(), [&](const Trigger& t) { return t.owner == owner; });
	}
}
