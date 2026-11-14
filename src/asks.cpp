#include "asks.hpp"
#include "helpers.hpp"
#include "overlay.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <iostream>

namespace Asks {
	using Extensions::Extension;
	using Permission = Extensions::ExtensionPermissionTypes;

	static std::deque<Ask> waiting;
	static std::vector<OverlaySetting> settings;

	void addSetting(const OverlaySetting& setting) {
		auto same = [&](const OverlaySetting& s) { return s.name == setting.name; };
		settings.erase(std::remove_if(settings.begin(), settings.end(), same), settings.end());
		settings.push_back(setting);
	}

	const OverlaySetting* findSetting(const std::string& name) {
		for (const auto& setting : settings) {
			if (setting.name == name) return &setting;
		}
		return nullptr;
	}

	std::vector<std::string> settingNames() {
		std::vector<std::string> names;
		for (const auto& setting : settings) names.push_back(setting.name);
		return names;
	}

	std::string settingState(const Extension& ext, const std::string& name) {
		const OverlaySetting* setting = findSetting(name);
		if (setting && setting->isOn && setting->isOn()) return "allowed";
		if (ext.neverAsk.count(name)) return "never";
		if (ext.refusedSettings.count(name)) return "refused";
		return "not_asked";
	}

	static Ask* queued(const Extension& ext, Kind kind) {
		for (Ask& ask : waiting) {
			if (ask.ext == &ext && ask.kind == kind) return &ask;
		}
		return nullptr;
	}

	static void add(std::vector<Permission>& list, Permission permission) {
		if (std::find(list.begin(), list.end(), permission) == list.end()) list.push_back(permission);
	}

	static Ask& questionFor(Extension& ext, Kind kind) {
		if (Ask* ask = queued(ext, kind)) return *ask;
		Ask ask;
		ask.ext = &ext;
		ask.kind = kind;
		waiting.push_back(ask);
		return waiting.back();
	}

	// it had it before its files changed: "Allow the same again" gives it back
	static bool hadBeforeChange(const Extension& ext, Permission permission) {
		const Extensions::PermissionInfo* info = Extensions::findPermission(permission);
		const auto& before = ext.grantsBeforeChange;
		return info && std::find(before.begin(), before.end(), info->name) != before.end();
	}

	void askPermission(Extension& ext, Permission permission) {
		if (ext.stopped || !Extensions::mayAsk(ext, permission) || hadBeforeChange(ext, permission)) return;
		add(questionFor(ext, Kind::Permissions).permissions, permission);
	}

	void askKeys(Extension& ext, const std::string& key) {
		if (ext.stopped || ext.refusedKeys.count(key)) return;
		if (std::find(ext.allowedKeys.begin(), ext.allowedKeys.end(), key) != ext.allowedKeys.end()) return;
		bool hasKeybinds = ext.extensionPermissions.count(Permission::Keybinds) > 0;
		// it had keys before its files changed: they wait for "Allow the same"
		if (!hasKeybinds && hadBeforeChange(ext, Permission::Keybinds)) return;
		if (!hasKeybinds && !Extensions::mayAsk(ext, Permission::Keybinds)) return;
		// after Never ask again it asks for no more keys either
		const Extensions::PermissionInfo* keybinds = Extensions::findPermission(Permission::Keybinds);
		if (hasKeybinds && ext.neverAsk.count(keybinds->name)) return;

		Ask& ask = questionFor(ext, Kind::Permissions);
		if (!hasKeybinds) add(ask.permissions, Permission::Keybinds);
		if (std::find(ask.keys.begin(), ask.keys.end(), key) == ask.keys.end()) ask.keys.push_back(key);
	}

	// what it had before beyond the defaults, the part the question is about
	static std::vector<const Extensions::PermissionInfo*> askedBefore(const Extension& ext) {
		std::vector<const Extensions::PermissionInfo*> list;
		for (const std::string& name : ext.grantsBeforeChange) {
			const Extensions::PermissionInfo* info = Extensions::findPermission(name);
			if (info && !Extensions::defaultPermissions.count(info->type)) list.push_back(info);
		}
		return list;
	}

	void askChanged(Extension& ext) {
		if (ext.stopped || ext.grantsBeforeChange.empty()) return;
		// it had only the defaults, which it still has: nothing to ask
		if (askedBefore(ext).empty()) {
			ext.grantsBeforeChange.clear();
			ext.hashBeforeChange.clear();
			Extensions::saveExtensionPermissions(ext);
			return;
		}
		Ask changed;
		changed.ext = &ext;
		changed.kind = Kind::Changed;
		// ahead of what it asks for new
		auto it = std::find_if(waiting.begin(), waiting.end(), [&](const Ask& a) { return a.ext == &ext && a.kind == Kind::Permissions; });
		waiting.insert(it, changed);
	}

	void askSetting(Extension& ext, const std::string& name) {
		const OverlaySetting* setting = findSetting(name);
		if (ext.stopped || !setting || settingState(ext, name) != "not_asked") return;
		if (queued(ext, Kind::Setting)) return;
		Ask& ask = questionFor(ext, Kind::Setting);
		ask.setting = name;
	}

	void forget(const Extension& ext) {
		waiting.erase(std::remove_if(waiting.begin(), waiting.end(), [&](const Ask& ask) { return ask.ext == &ext; }), waiting.end());
	}

	void forget(const Extension& ext, Kind kind) {
		waiting.erase(std::remove_if(waiting.begin(), waiting.end(), [&](const Ask& ask) { return ask.ext == &ext && ask.kind == kind; }), waiting.end());
	}

	void forgetKey(const Extension& ext, const std::string& key) {
		Ask* ask = queued(ext, Kind::Permissions);
		if (!ask) return;
		ask->keys.erase(std::remove(ask->keys.begin(), ask->keys.end(), key), ask->keys.end());
		// Keybinds asked for only for this key
		if (ask->keys.empty()) {
			auto& list = ask->permissions;
			list.erase(std::remove(list.begin(), list.end(), Permission::Keybinds), list.end());
		}
		if (ask->permissions.empty() && ask->keys.empty()) forget(ext, Kind::Permissions);
	}

	const std::deque<Ask>& queue() {
		return waiting;
	}

	void clear() {
		waiting.clear();
		settings.clear();
	}

	const Ask* showing(bool mapOpen, bool menuOpen) {
		if (waiting.empty() || (!mapOpen && !menuOpen)) return nullptr;
		return &waiting.front();
	}

	static void allowKeys(Extension& ext, const std::vector<std::string>& keys) {
		for (const std::string& key : keys) {
			if (std::find(ext.allowedKeys.begin(), ext.allowedKeys.end(), key) == ext.allowedKeys.end()) ext.allowedKeys.push_back(key);
		}
	}

	void answer(Answer answer) {
		if (waiting.empty()) return;
		Ask ask = waiting.front();
		waiting.pop_front();
		Extension& ext = *ask.ext;

		switch (ask.kind) {
		case Kind::Permissions:
			if (answer == Answer::Allow) {
				for (Permission permission : ask.permissions) Extensions::allow(ext, permission);
				allowKeys(ext, ask.keys);
			}
			else {
				bool never = answer == Answer::Never;
				for (Permission permission : ask.permissions) Extensions::refuse(ext, permission, never);
				for (const std::string& key : ask.keys) ext.refusedKeys.insert(key);
				// more keys after Keybinds was allowed: Never is about asking for keys
				if (never && ask.permissions.empty()) ext.neverAsk.insert(Extensions::findPermission(Permission::Keybinds)->name);
			}
			break;
		case Kind::Changed:
			if (answer == Answer::Allow) {
				for (const std::string& name : ext.grantsBeforeChange) {
					const Extensions::PermissionInfo* info = Extensions::findPermission(name);
					if (info && !ext.switchedOff.count(name) && !ext.neverAsk.count(name)) ext.extensionPermissions.insert(info->type);
				}
				ext.grantsBeforeChange.clear();
				ext.hashBeforeChange.clear();
			}
			break;
		case Kind::Setting: {
			const OverlaySetting* setting = findSetting(ask.setting);
			if (answer == Answer::Allow && setting && setting->turnOn) setting->turnOn();
			else if (answer != Answer::Allow) {
				ext.refusedSettings.insert(ask.setting);
				if (answer == Answer::Never) ext.neverAsk.insert(ask.setting);
			}
			break;
		}
		}

		if (ask.kind != Kind::Setting || answer == Answer::Never) Extensions::saveExtensionPermissions(ext);
		if (answer == Answer::Allow) {
			Extensions::bindAllowedKeys(ext);
			Extensions::askWaitingKeys(ext);
		}
		if (answer == Answer::Allow) std::cout << "[" << ext.name << "] allowed what it asked for" << std::endl;
	}

	// a permission as a bullet: its label, the author's reason after it, the
	// overlay's reason line under it
	static void bullet(const Extension& ext, const Extensions::PermissionInfo& info, const std::vector<std::string>& keys) {
		auto reason = ext.reasons.find(info.type);
		if (reason != ext.reasons.end() && !reason->second.empty()) ImGui::BulletText("%s: %s", info.label, reason->second.c_str());
		else ImGui::BulletText("%s", info.label);
		ImGui::Indent();
		if (info.type == Permission::Keybinds && !keys.empty()) ImGui::Text("Keys: %s", Helpers::joined(keys).c_str());
		ImGui::TextDisabled("%s", Extensions::reasonLine(ext, info).c_str());
		ImGui::Unindent();
	}

	static void drawPermissions(const Ask& ask, Answer& answered, bool& done) {
		const Extension& ext = *ask.ext;
		if (ask.permissions.empty()) {
			Extensions::drawName(ext);
			ImGui::SameLine();
			ImGui::Text("asks for more keys: %s", Helpers::joined(ask.keys).c_str());
			ImGui::TextDisabled("%s", Extensions::findPermission(Permission::Keybinds)->reason);
		}
		else if (ask.permissions.size() == 1 && !ext.reasons.count(ask.permissions[0])) {
			const Extensions::PermissionInfo* info = Extensions::findPermission(ask.permissions[0]);
			Extensions::drawName(ext);
			ImGui::SameLine();
			ImGui::Text("asks for: %s", info->label);
			if (info->type == Permission::Keybinds && !ask.keys.empty()) ImGui::Text("Keys: %s", Helpers::joined(ask.keys).c_str());
			ImGui::TextDisabled("%s", Extensions::reasonLine(ext, *info).c_str());
		}
		else {
			Extensions::drawName(ext);
			ImGui::SameLine();
			ImGui::TextUnformatted("asks for:");
			for (Permission permission : ask.permissions) bullet(ext, *Extensions::findPermission(permission), ask.keys);
		}
		if (Controls::Button("Allow")) { answered = Answer::Allow; done = true; }
		ImGui::SameLine();
		if (Controls::Button("Don't allow")) { answered = Answer::DontAllow; done = true; }
		ImGui::SameLine();
		if (Controls::Button("Never ask again")) { answered = Answer::Never; done = true; }
	}

	static void drawChanged(const Ask& ask, Answer& answered, bool& done) {
		const Extension& ext = *ask.ext;
		Extensions::drawName(ext);
		ImGui::SameLine();
		ImGui::TextUnformatted("changed since you allowed it. Allow the same again?");
		std::vector<std::string> keys = ext.allowedKeys;
		for (const Extensions::PermissionInfo* info : askedBefore(ext)) bullet(ext, *info, keys);
		ImGui::TextDisabled("Until then what needs them waits.");
		if (Controls::Button("Allow the same")) { answered = Answer::Allow; done = true; }
		ImGui::SameLine();
		if (Controls::Button("Not now")) { answered = Answer::NotNow; done = true; }
	}

	static void drawSetting(const Ask& ask, Answer& answered, bool& done) {
		const OverlaySetting* setting = findSetting(ask.setting);
		Extensions::drawName(*ask.ext);
		ImGui::SameLine();
		ImGui::Text("asks to turn on: %s", setting ? setting->turnOnLabel.c_str() : ask.setting.c_str());
		if (setting) ImGui::TextDisabled("%s", setting->reason.c_str());
		if (Controls::Button("Turn on")) { answered = Answer::Allow; done = true; }
		ImGui::SameLine();
		if (Controls::Button("Don't turn on")) { answered = Answer::DontAllow; done = true; }
		ImGui::SameLine();
		if (Controls::Button("Never ask again")) { answered = Answer::Never; done = true; }
	}

	void draw(bool mapOpen, bool menuOpen) {
		const Ask* ask = showing(mapOpen, menuOpen);
		if (!ask) return;

		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		// focused when it comes up, not every frame, so a text box keeps it
		if (!ask->shown) {
			ImGui::SetNextWindowFocus();
			waiting.front().shown = true;
		}
		ImGui::Begin("##question", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
		// a window drawn after the menu stays over it
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

		Answer answered = Answer::NotNow;
		bool done = false;
		switch (ask->kind) {
		case Kind::Permissions: drawPermissions(*ask, answered, done); break;
		case Kind::Changed: drawChanged(*ask, answered, done); break;
		case Kind::Setting: drawSetting(*ask, answered, done); break;
		}

		ImVec2 position = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
		Overlay::ClaimMouse(position.x, position.y, position.x + size.x, position.y + size.y);
		ImGui::End();
		if (done) answer(answered);
	}
}
