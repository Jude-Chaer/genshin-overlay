#pragma once
#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

// Short messages from the overlay and its extensions. They show in the
// menu and fade, and each extension's page keeps its last ones. Errors also
// show over the game for a moment while the menu is closed, never taking a
// click. A tip shows once: closed, it never comes back.
namespace Notices {
    enum class Kind { Plain, Error, Tip, Done };

    struct Notice {
        int id = 0;
        // the extension's folder, "" for the overlay's own
        std::string owner;
        // its name, shown above it in the menu
        std::string from;
        Kind kind = Kind::Plain;
        std::string text;
        // what tells one tip from another, kept once it's closed
        std::string tip;
        double addedAt = 0;
        // when the menu first showed it, below 0 until then
        double shownAt = -1;
        bool closed = false;
        // a button on it, e.g. to open a page
        std::string actionLabel;
        std::function<void()> action;
    };

    inline constexpr double FadeAfter = 8.0;
    inline constexpr double OverGameFor = 5.0;
    inline constexpr size_t KeptPerOwner = 10;
    inline constexpr size_t MaxText = 300;
    // at most this many fade in the menu at once, the newest
    inline constexpr size_t MaxShown = 3;

    // The new notice's id, 0 when it isn't added: a tip closed before or
    // still there, or the same text as the owner's newest notice while that
    // one still shows or waits for the menu. An error waiting past its time
    // over the game shows there again.
    int Add(const std::string& owner, const std::string& from, Kind kind, const std::string& text, double now, const std::string& tip = "",
        const std::string& actionLabel = "", std::function<void()> action = {});
    void Close(int id);
    // the owner's notices, newest first
    std::vector<const Notice*> Of(const std::string& owner);
    // an error still shows over the game, so the overlay draws
    bool OverGameShowing(double now);

    // Each frame with the menu open: the notices fading, in a corner of the
    // menu's box. Gives how many it drew.
    int DrawInMenu(double now, ImVec2 menuPos, ImVec2 menuSize);
    // Each frame with the menu closed: the errors of the last seconds. Gives
    // how many it drew.
    int DrawOverGame(double now);
    // The owner's notices on its page, newest first.
    void DrawList(const std::string& owner);

    // drops every notice, for the tests
    void Clear();
}
