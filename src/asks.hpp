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
		Setting,
		// a change of its that unmarks more than one point: Allow unmarks,
		// DontAllow keeps them marked, NotNow cancels the whole change
		Unmark
	};

	enum class Answer { Allow, DontAllow, Never, NotNow };

	// one site's part of an Unmark question
	struct UnmarkLine {
		std::string site;
		int count = 0;
		// ("Liyue, Common Chest", 20), most first
		std::vector<std::pair<std::string, int>> which;
	};

	struct Ask {
		Extensions::Extension* ext = nullptr;
		Kind kind = Kind::Permissions;
		std::vector<Extensions::ExtensionPermissionTypes> permissions;
		// keys by their shown name, when it asks for Keybinds or more keys
		std::vector<std::string> keys;
		std::string setting;
		std::vector<UnmarkLine> unmarks;
		int unmarkPoints = 0;
		std::function<void(Answer)> answered;
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
	// the names of every setting extensions may ask for
	std::vector<std::string> settingNames();
	// PermissionState for an overlay setting: "allowed" when it's on
	std::string settingState(const Extensions::Extension& ext, const std::string& name);

	// Nothing is queued for what it has, what was refused this start, Never
	// ask again, or what's switched off on its Permissions tab.
	void askPermission(Extensions::Extension& ext, Extensions::ExtensionPermissionTypes permission);
	void askKeys(Extensions::Extension& ext, const std::string& key);
	void askChanged(Extensions::Extension& ext);
	void askSetting(Extensions::Extension& ext, const std::string& name);
	// one question per change, answered gets the player's answer once;
	// forgotten with the extension, it isn't called
	// points: how many in all, one unmarked on both sites is one
	void askUnmark(Extensions::Extension& ext, const std::vector<UnmarkLine>& lines, int points, std::function<void(Answer)> answered);
	// its questions leave the queue, when it stops or unloads
	void forget(const Extensions::Extension& ext);
	void forget(const Extensions::Extension& ext, Kind kind);
	// a key it deleted leaves its question, and a question with nothing left goes
	void forgetKey(const Extensions::Extension& ext, const std::string& key);
	const std::deque<Ask>& queue();
	void clear();

	// the question on screen now: the first in line while the map or menu is open
	const Ask* showing(bool mapOpen, bool menuOpen);

	// answers the question showing, saves it, and the next one comes up
	void answer(Answer answer);

	// the window, drawn over the menu
	void draw(bool mapOpen, bool menuOpen);
}
