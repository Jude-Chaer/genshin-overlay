#pragma once
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "extensions.hpp"

// The questions extensions make the overlay ask the player, one window at a
// time in a queue. A question shows only while the big map or the menu is
// open, when the cursor is free. Each extension has one question of a kind in
// the queue, a second ask joins it.

namespace Asks {
	enum class Kind {
		// permissions it asks for, and keys
		Permissions,
		// its files changed: allow the same again. What it asks for new is
		// its own Permissions question, after this one
		Changed,
		// an overlay setting it asks to turn on
		Setting
	};

	struct Ask {
		Extensions::Extension* ext = nullptr;
		Kind kind = Kind::Permissions;
		std::vector<Extensions::ExtensionPermissionTypes> permissions;
		// keys by their shown name, when it asks for Keybinds or more keys
		std::vector<std::string> keys;
		std::string setting;
		bool shown = false;
	};

	// An overlay setting extensions may ask for, like reading the game's
	// traffic. The overlay keeps the list, an extension names one of them.
	struct OverlaySetting {
		std::string name;
		// "Read the game's traffic"
		std::string turnOnLabel;
		std::string reason;
		std::function<bool()> isOn;
		std::function<void()> turnOn;
	};
	void addSetting(const OverlaySetting& setting);
	const OverlaySetting* findSetting(const std::string& name);
	// PermissionState for an overlay setting: "allowed" when it's on
	std::string settingState(const Extensions::Extension& ext, const std::string& name);

	// Nothing is queued for what it has, what was refused this start, Never
	// ask again, or what's switched off on its Permissions tab.
	void askPermission(Extensions::Extension& ext, Extensions::ExtensionPermissionTypes permission);
	void askKeys(Extensions::Extension& ext, const std::string& key);
	void askChanged(Extensions::Extension& ext);
	void askSetting(Extensions::Extension& ext, const std::string& name);
	// its questions leave the queue, when it stops or unloads
	void forget(const Extensions::Extension& ext);
	void forget(const Extensions::Extension& ext, Kind kind);
	// a key it deleted leaves its question, and a question with nothing left goes
	void forgetKey(const Extensions::Extension& ext, const std::string& key);
	const std::deque<Ask>& queue();
	void clear();

	// the question on screen now: the first in line while the map or menu is open
	const Ask* showing(bool mapOpen, bool menuOpen);

	enum class Answer { Allow, DontAllow, Never, NotNow };
	// answers the question showing, saves it, and the next one comes up
	void answer(Answer answer);

	// the window, drawn over the menu
	void draw(bool mapOpen, bool menuOpen);
}
