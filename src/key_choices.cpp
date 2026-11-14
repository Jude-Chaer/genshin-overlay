#include <Windows.h>
#include "key_choices.hpp"
#include "asks.hpp"
#include "keybindings.hpp"
#include "settings.hpp"
#include "typing.hpp"
#include <imgui.h>
#include <algorithm>
#include <map>
#include <iostream>

namespace KeyChoices {
	using Extensions::Extension;
	using HeldKey = Extension::HeldKey;

	static const int defaultMenuKey = GLFW_KEY_GRAVE_ACCENT;
	static const int defaultMenuModifiers = Keybindings::KeybindModifiers_Alt;
	static int menuGlfwKey = defaultMenuKey;
	static int menuHeld = defaultMenuModifiers;
	static std::function<void()> menuToggle;
	// the keybind's id while it's bound
	static std::string menuBound;

	std::optional<Holder> holderOf(int glfwKey, int modifiers) {
		if (glfwKey == 0) return std::nullopt;
		if (menuGlfwKey == glfwKey && menuHeld == modifiers) {
			return Holder{};
		}
		for (auto& ext : Extensions::registeredExtensions) {
			for (const HeldKey& key : ext->keys) {
				if (key.key == glfwKey && key.modifiers == modifiers) return Holder{ ext.get(), key.id };
			}
		}
		return std::nullopt;
	}

	static HeldKey* findIn(Extension& ext, const std::string& id) {
		auto it = std::find_if(ext.keys.begin(), ext.keys.end(), [&](const HeldKey& key) { return key.id == id; });
		return it == ext.keys.end() ? nullptr : &*it;
	}

	HeldKey* find(const Holder& holder) {
		return holder.ext ? findIn(*holder.ext, holder.id) : nullptr;
	}

	std::string describe(const Holder& holder) {
		if (holder.menu()) return "the menu";
		const HeldKey* key = find(holder);
		if (!key) return "";
		if (key->label.empty()) return holder.ext->name;
		return holder.ext->name + " (" + key->label + ")";
	}

	static void removeAllowed(Extension& ext, const std::string& name) {
		auto& allowed = ext.allowedKeys;
		allowed.erase(std::remove(allowed.begin(), allowed.end(), name), allowed.end());
	}

	static void letGo(Extension& ext, HeldKey& key) {
		if (!key.bound.empty()) Keybindings::DeleteKeybind(key.bound);
		key.bound.clear();
		if (key.name.empty()) return;
		removeAllowed(ext, key.name);
		Asks::forgetKey(ext, key.name);
	}

	static void remember(Extension& ext, const HeldKey& key) {
		if (key.name == key.authorName) ext.pickedKeys.erase(key.authorName);
		else ext.pickedKeys[key.authorName] = key.name;
		Extensions::saveExtensionPermissions(ext);
	}

	static void clear(Extension& ext, HeldKey& key) {
		letGo(ext, key);
		key.key = 0;
		key.modifiers = 0;
		key.name.clear();
	}

	// the player put the key there, so it's allowed
	static void put(Extension& ext, HeldKey& key, int glfwKey, int modifiers) {
		letGo(ext, key);
		key.key = glfwKey;
		key.modifiers = modifiers;
		key.name = Keybindings::KeyName(glfwKey, modifiers);
		if (std::find(ext.allowedKeys.begin(), ext.allowedKeys.end(), key.name) == ext.allowedKeys.end()) ext.allowedKeys.push_back(key.name);
		// a key the player picks turns Keybinds on
		if (!ext.extensionPermissions.count(Extensions::ExtensionPermissionTypes::Keybinds))
			Extensions::switchPermission(ext, Extensions::ExtensionPermissionTypes::Keybinds, true);
		remember(ext, key);
		Extensions::bindAllowedKeys(ext);
	}

	static Result choose(Extension& ext, const std::string& id, int glfwKey, int modifiers, bool takeIt) {
		Result result;
		HeldKey* key = findIn(ext, id);
		if (!key) {
			result.whyNot = "the key is gone";
			return result;
		}
		result.whyNot = Keybindings::WhyNotPickable(glfwKey, modifiers);
		if (!result.whyNot.empty()) return result;
		bool keybinds = ext.extensionPermissions.count(Extensions::ExtensionPermissionTypes::Keybinds) > 0;
		bool allowed = std::find(ext.allowedKeys.begin(), ext.allowedKeys.end(), key->name) != ext.allowedKeys.end();
		if (key->key == glfwKey && key->modifiers == modifiers && keybinds && allowed) return result;
		std::optional<Holder> holder = holderOf(glfwKey, modifiers);
		if (holder && holder->ext == &ext && holder->id == id) holder.reset();
		if (holder) {
			HeldKey* other = find(*holder);
			if (!takeIt || !other) {
				result.takenBy = holder;
				return result;
			}
			clear(*holder->ext, *other);
			remember(*holder->ext, *other);
		}
		put(ext, *key, glfwKey, modifiers);
		return result;
	}

	Result pick(Extension& ext, const std::string& id, int glfwKey, int modifiers) {
		return choose(ext, id, glfwKey, modifiers, false);
	}

	Result give(Extension& ext, const std::string& id, int glfwKey, int modifiers) {
		return choose(ext, id, glfwKey, modifiers, true);
	}

	Result reset(Extension& ext, const std::string& id) {
		HeldKey* key = findIn(ext, id);
		if (!key) return { "the key is gone", std::nullopt };
		return pick(ext, id, key->authorKey, key->authorModifiers);
	}

	void unset(Extension& ext, const std::string& id) {
		HeldKey* key = findIn(ext, id);
		if (!key || key->key == 0) return;
		clear(ext, *key);
		remember(ext, *key);
	}

	static void bindMenuKey() {
		if (!menuBound.empty()) Keybindings::DeleteKeybind(menuBound);
		std::string whyNot;
		menuBound = Keybindings::CreateKeybind(menuGlfwKey, [] { if (menuToggle) menuToggle(); },
			Keybindings::KeybindFlags_ProcessWhileHidden, menuHeld, &whyNot);
		if (menuBound.empty()) std::cerr << "[menu] " << whyNot << std::endl;
	}

	void startMenuKey(std::function<void()> toggle) {
		menuToggle = std::move(toggle);
		int glfwKey = 0;
		Keybindings::KeybindModifiers modifiers = 0;
		bool usable = Keybindings::ParseKeyName(Settings::menuKey, glfwKey, modifiers) &&
			Keybindings::WhyNotPickable(glfwKey, modifiers, true).empty();
		menuGlfwKey = usable ? glfwKey : defaultMenuKey;
		menuHeld = usable ? modifiers : defaultMenuModifiers;
		bindMenuKey();
	}

	int menuKey() { return menuGlfwKey; }
	int menuModifiers() { return menuHeld; }
	std::string menuKeyName() { return Keybindings::KeyName(menuGlfwKey, menuHeld); }

	Result pickMenuKey(int glfwKey, int modifiers, bool takeIt) {
		Result result;
		result.whyNot = Keybindings::WhyNotPickable(glfwKey, modifiers, true);
		if (!result.whyNot.empty()) return result;
		if (glfwKey == menuGlfwKey && modifiers == menuHeld) return result;
		std::optional<Holder> holder = holderOf(glfwKey, modifiers);
		if (holder) {
			HeldKey* other = find(*holder);
			if (!takeIt || !other) {
				result.takenBy = holder;
				return result;
			}
			clear(*holder->ext, *other);
			remember(*holder->ext, *other);
		}
		menuGlfwKey = glfwKey;
		menuHeld = modifiers;
		Settings::menuKey = menuKeyName();
		Settings::Save();
		bindMenuKey();
		return result;
	}

	Result resetMenuKey() {
		return pickMenuKey(defaultMenuKey, defaultMenuModifiers);
	}

	void start(Extension& ext, HeldKey& key) {
		key.key = key.authorKey;
		key.modifiers = key.authorModifiers;
		auto saved = ext.pickedKeys.find(key.authorName);
		if (saved != ext.pickedKeys.end()) {
			int glfwKey = 0;
			Keybindings::KeybindModifiers modifiers = 0;
			bool usable = Keybindings::ParseKeyName(saved->second, glfwKey, modifiers) &&
				Keybindings::WhyNotPickable(glfwKey, modifiers).empty();
			key.key = usable ? glfwKey : 0;
			key.modifiers = usable ? modifiers : 0;
		}
		std::optional<Holder> holder = holderOf(key.key, key.modifiers);
		if (holder) {
			std::cout << "[" << ext.name << "] " << Keybindings::KeyName(key.key, key.modifiers) << " is used by "
				<< describe(*holder) << ", this one starts without a key" << std::endl;
			key.key = 0;
			key.modifiers = 0;
			// the player's own pick stays saved, only the author's key is given up
			if (saved == ext.pickedKeys.end()) {
				ext.pickedKeys[key.authorName] = "";
				if (ext.firstStart) {
					// only the keys, it still counts as never answered
					Settings::Grants grants;
					grants.picked = ext.pickedKeys;
					Settings::SaveGrants(ext.folderKey, grants);
				}
				else Extensions::saveExtensionPermissions(ext);
			}
		}
		key.name = key.key ? Keybindings::KeyName(key.key, key.modifiers) : "";
	}

	// a row on the Keys tab or the menu's
	using Row = Holder;

	static const std::pair<const char*, int> modifierChoices[] = {
		{ "Alt", Keybindings::KeybindModifiers_Alt },
		{ "Ctrl", Keybindings::KeybindModifiers_Ctrl },
		{ "Alt+Shift", Keybindings::KeybindModifiers_Alt | Keybindings::KeybindModifiers_Shift },
		{ "Ctrl+Shift", Keybindings::KeybindModifiers_Ctrl | Keybindings::KeybindModifiers_Shift },
		{ "Ctrl+Alt", Keybindings::KeybindModifiers_Ctrl | Keybindings::KeybindModifiers_Alt },
	};

	static const char* modifierName(int modifiers) {
		for (const auto& [name, held] : modifierChoices) {
			if (held == modifiers) return name;
		}
		return "";
	}

	static std::string rowName(const Row& row) {
		return row.menu() ? std::string("menu") : row.ext->folderKey + "/" + row.id;
	}

	struct Shown {
		int key = 0, modifiers = 0, authorKey = 0, authorModifiers = 0;
		std::string title;
	};

	static bool shown(const Row& row, Shown& out) {
		if (row.menu()) {
			out = { menuGlfwKey, menuHeld, defaultMenuKey, defaultMenuModifiers, "Open and close the menu" };
			return true;
		}
		const HeldKey* key = findIn(*row.ext, row.id);
		if (!key) return false;
		out = { key->key, key->modifiers, key->authorKey, key->authorModifiers, key->label.empty() ? key->authorName : key->label };
		return true;
	}

	static Result choose(const Row& row, int glfwKey, int modifiers, bool takeIt) {
		if (row.menu()) return pickMenuKey(glfwKey, modifiers, takeIt);
		return takeIt ? give(*row.ext, row.id, glfwKey, modifiers) : pick(*row.ext, row.id, glfwKey, modifiers);
	}

	// the pick waiting on the "already used" question
	struct Taken {
		Row row;
		int key = 0, modifiers = 0;
		Holder holder;
	};
	static std::optional<Taken> taken;
	static bool openTaken = false;
	// a line under a row: why its last pick didn't go through
	static std::map<std::string, std::string> refusals;
	// the modifiers picked for a row without a key, used once a key is picked
	static std::map<std::string, int> unsetModifiers;
	// the pick waiting on the "used by another program" question
	struct Pending {
		Row row;
		int key = 0, modifiers = 0;
	};
	static std::optional<Pending> otherProgram;
	static bool openOtherProgram = false;

	// another program has the key, looked at again every few seconds, or now
	static bool otherProgramHas(int glfwKey, int modifiers, bool now = false) {
		struct Look {
			bool has = false;
			double at = -1e9;
		};
		static std::map<std::pair<int, int>, Look> looked;
		Look& look = looked[{ glfwKey, modifiers }];
		double time = ImGui::GetTime();
		if (now || time - look.at > 5.0) look = { Keybindings::OtherProgramHas(glfwKey, modifiers), time };
		return look.has;
	}

	static void tryPick(const Row& row, int glfwKey, int modifiers, bool inQuestion, bool anyway = false) {
		Shown now;
		bool same = shown(row, now) && now.key == glfwKey && now.modifiers == modifiers;
		if (!anyway && !inQuestion && !same && Keybindings::WhyNotPickable(glfwKey, modifiers, row.menu()).empty()
			&& otherProgramHas(glfwKey, modifiers, true)) {
			refusals.erase(rowName(row));
			otherProgram = Pending{ row, glfwKey, modifiers };
			openOtherProgram = true;
			return;
		}
		Result result = choose(row, glfwKey, modifiers, false);
		refusals.erase(rowName(row));
		if (!result.whyNot.empty()) {
			refusals[rowName(row)] = result.whyNot;
		}
		else if (result.takenBy) {
			if (inQuestion) {
				refusals[rowName(row)] = Keybindings::KeyName(glfwKey, modifiers) + " is used by " + describe(*result.takenBy);
			}
			else {
				taken = Taken{ row, glfwKey, modifiers, *result.takenBy };
				openTaken = true;
			}
		}
	}

	// the row Record waits on, the frame it was last drawn, and the frame recording ended
	static std::optional<Row> recordingRow;
	static int recordingDrawn = -1;
	static int recordEnded = -1;

	// A key another program holds as a hotkey never reaches the overlay's
	// window. A low-level hook still sees it go down; it is up only while
	// Record waits, and the key goes on to where it went.
	static HHOOK takenHook = nullptr;
	static DWORD caughtKey = 0;

	static LRESULT CALLBACK seeKey(int code, WPARAM message, LPARAM data) {
		if (code == HC_ACTION && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN))
			caughtKey = ((const KBDLLHOOKSTRUCT*)data)->vkCode;
		return CallNextHookEx(nullptr, code, message, data);
	}

	static void hookTakenKeys(bool on) {
		if (on && !takenHook) takenHook = SetWindowsHookExW(WH_KEYBOARD_LL, seeKey, GetModuleHandleW(nullptr), 0);
		else if (!on && takenHook) {
			UnhookWindowsHookEx(takenHook);
			takenHook = nullptr;
		}
		if (!takenHook) caughtKey = 0;
	}

	void stopRecording() {
		if (recordingRow) recordEnded = ImGui::GetFrameCount();
		recordingRow.reset();
		hookTakenKeys(false);
	}

	bool recording() {
		// its row isn't drawn anymore, another page or tab is shown
		if (recordingRow && recordingDrawn < ImGui::GetFrameCount() - 1) stopRecording();
		return recordingRow.has_value() || recordEnded == ImGui::GetFrameCount();
	}

	// The keys only come to ImGui while the overlay has the focus, the hotkeys
	// are let go of then. The first key that isn't a modifier is taken, with
	// the modifiers held.
	static void readRecorded(const Row& row, bool inQuestion) {
		// keys typed in another window are not the player's pick
		if (!Typing::focus.hasFocus()) {
			hookTakenKeys(false);
			return;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			stopRecording();
			return;
		}
		const ImGuiIO& io = ImGui::GetIO();
		int held = 0;
		if (io.KeyCtrl) held |= Keybindings::KeybindModifiers_Ctrl;
		if (io.KeyAlt) held |= Keybindings::KeybindModifiers_Alt;
		if (io.KeyShift) held |= Keybindings::KeybindModifiers_Shift;
		for (int key : Keybindings::PickableKeys()) {
			if (!ImGui::IsKeyPressed(Keybindings::ImGuiKeyFor(key), false)) continue;
			stopRecording();
			tryPick(row, key, held, inQuestion);
			return;
		}
		hookTakenKeys(Settings::recordTakenKeys);
		if (int key = Keybindings::PickableKeyFor(caughtKey)) {
			stopRecording();
			tryPick(row, key, held, inQuestion);
		}
	}

	static void drawRow(const Row& row, float controlsAt, bool inQuestion) {
		Shown now;
		if (!shown(row, now)) return;
		std::string name = rowName(row);
		ImGui::PushID(name.c_str());

		Controls::RowName(now.title.c_str());
		ImGui::SameLine(controlsAt);

		int modifiers = now.modifiers;
		if (now.key == 0) {
			auto chosen = unsetModifiers.find(name);
			modifiers = chosen != unsetModifiers.end() ? chosen->second : now.authorModifiers;
		}
		float width = ImGui::GetFontSize() * 6.0f;
		if (Controls::BeginDropDown("##modifiers", modifierName(modifiers), width, (int)std::size(modifierChoices))) {
			for (const auto& [label, held] : modifierChoices) {
				if (ImGui::Selectable(label, held == modifiers)) {
					if (now.key == 0) unsetModifiers[name] = held;
					else tryPick(row, now.key, held, inQuestion);
				}
			}
			Controls::EndDropDown();
		}
		ImGui::SameLine();
		ImGui::TextUnformatted("+");
		ImGui::SameLine();
		std::string keyName = now.key ? Keybindings::KeyName(now.key, 0) : "None";
		const auto& pickable = Keybindings::PickableKeys();
		if (Controls::BeginDropDown("##key", keyName.c_str(), width, (int)pickable.size())) {
			for (int key : pickable) {
				if (ImGui::Selectable(Keybindings::KeyName(key, 0).c_str(), key == now.key)) tryPick(row, key, modifiers, inQuestion);
				if (key == now.key) ImGui::SetItemDefaultFocus();
			}
			Controls::EndDropDown();
		}
		ImGui::SameLine();
		if (recordingRow && *recordingRow == row) {
			recordingDrawn = ImGui::GetFrameCount();
			if (Controls::Button("Press keys... (Esc cancels)##record")) stopRecording();
			else readRecorded(row, inQuestion);
		}
		else if (Controls::Button("Record##record")) {
			recordingRow = row;
			recordingDrawn = ImGui::GetFrameCount();
			refusals.erase(name);
		}
		bool changed = now.key != now.authorKey || now.modifiers != now.authorModifiers;
		if (changed) {
			ImGui::SameLine();
			if (Controls::Button("Reset")) tryPick(row, now.authorKey, now.authorModifiers, inQuestion);
		}

		if (now.key == 0) {
			Controls::Empty("No key.");
			std::optional<Holder> holder = holderOf(now.authorKey, now.authorModifiers);
			std::string authorName = Keybindings::KeyName(now.authorKey, now.authorModifiers);
			if (holder && !inQuestion) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s is used by %s.", authorName.c_str(), describe(*holder).c_str());
				if (!holder->menu()) {
					ImGui::SameLine();
					if (ImGui::SmallButton(("Give it " + authorName).c_str())) choose(row, now.authorKey, now.authorModifiers, true);
				}
			}
		}
		if (modifiers & Keybindings::KeybindModifiers_Ctrl) ImGui::TextDisabled("Pressing Ctrl also switches between walking and running in game.");
		if (now.key != 0 && otherProgramHas(now.key, now.modifiers)) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("Another program uses %s. It does nothing here until that program lets go of it.",
				Keybindings::KeyName(now.key, now.modifiers).c_str());
			ImGui::PopStyleColor();
		}
		auto refused = refusals.find(name);
		if (refused != refusals.end()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", refused->second.c_str());
		ImGui::PopID();
	}

	static float controlsColumn(const std::vector<Row>& rows) {
		float widest = 0.0f;
		for (const Row& row : rows) {
			Shown now;
			if (shown(row, now)) widest = (std::max)(widest, Controls::RowNameWidth(now.title.c_str()));
		}
		return ImGui::GetCursorPosX() + widest + ImGui::GetStyle().ItemSpacing.x * 2.0f;
	}

	// "Another program uses Alt+M": keep it for when that one lets go, or pick another
	static void drawOtherProgramQuestion() {
		const char* title = "Key used by another program";
		if (openOtherProgram) {
			ImGui::OpenPopup(title);
			openOtherProgram = false;
		}
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
		if (!otherProgram) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		std::string keyName = Keybindings::KeyName(otherProgram->key, otherProgram->modifiers);
		ImGui::Text("Another program on this PC uses %s.", keyName.c_str());
		ImGui::TextDisabled("While it does, %s does nothing here.", keyName.c_str());
		ImGui::Separator();
		if (Controls::Button("Use it anyway")) {
			Pending pick = *otherProgram;
			otherProgram.reset();
			ImGui::CloseCurrentPopup();
			tryPick(pick.row, pick.key, pick.modifiers, false, true);
		}
		ImGui::SameLine();
		if (Controls::Button("Cancel")) {
			otherProgram.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	// "Alt+M is used by Minimap": that one's row to change it there, or take the key
	static void drawTakenQuestion() {
		const char* title = "Key already used";
		if (openTaken) {
			ImGui::OpenPopup(title);
			openTaken = false;
		}
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
		if (!taken) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		std::string keyName = Keybindings::KeyName(taken->key, taken->modifiers);
		std::optional<Holder> holder = holderOf(taken->key, taken->modifiers);
		bool stillTaken = holder && *holder == taken->holder;
		if (!stillTaken) {
			// the other one was changed here, the key is free
			tryPick(taken->row, taken->key, taken->modifiers, true);
			taken.reset();
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		ImGui::Text("%s is used by %s.", keyName.c_str(), describe(taken->holder).c_str());
		ImGui::TextDisabled("Change that one here, and this one gets %s.", keyName.c_str());
		Row other{ taken->holder.ext, taken->holder.id };
		drawRow(other, controlsColumn({ other }), true);
		ImGui::Separator();
		if (!taken->holder.menu()) {
			ImGui::TextDisabled("Or take it, %s is left without a key.", describe(taken->holder).c_str());
			if (Controls::Button("Take it")) {
				choose(taken->row, taken->key, taken->modifiers, true);
				refusals.erase(rowName(taken->row));
				taken.reset();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
		}
		if (Controls::Button("Cancel")) {
			taken.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void drawKeysTab(Extension& ext) {
		bool allowed = ext.extensionPermissions.count(Extensions::ExtensionPermissionTypes::Keybinds) > 0;
		if (!allowed) ImGui::TextWrapped("Keybinds is off, these keys do nothing. Picking a key turns it on.");
		std::vector<Row> rows;
		for (const HeldKey& key : ext.keys) rows.push_back({ &ext, key.id });
		float controlsAt = controlsColumn(rows);
		for (const Row& row : rows) drawRow(row, controlsAt, false);
		drawOtherProgramQuestion();
		drawTakenQuestion();
	}

	void drawMenuKeyRow() {
		Row menu;
		drawRow(menu, controlsColumn({ menu }), false);
		drawOtherProgramQuestion();
		drawTakenQuestion();
	}
}
