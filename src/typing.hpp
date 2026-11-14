#pragma once
#include <windows.h>
#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

// The overlay window never takes the focus, so keys go to the game. While a
// text box is typed in it takes the focus after all, and gives it back once
// the box is done (Enter, Esc, a click elsewhere): to the window it came from,
// or the game. A fullscreen game minimizes meanwhile, without admin we can't
// stop that.
namespace Typing {
    // What typing does to Windows. The overlay passes the real calls, the
    // tests their own.
    struct Windows {
        HWND overlay = nullptr;
        std::function<HWND()> foreground;
        std::function<HWND()> game;
        std::function<bool(HWND)> exists;
        // let the overlay be focused and bring it to the front, false when Windows said no
        std::function<bool()> takeFocus;
        // the overlay can't be focused again, then `to` comes to the front (none: nothing does)
        std::function<void(HWND to)> giveFocus;
    };

    struct Frame {
        bool menuOpen = false;
        // a text box is typed in this frame
        bool typing = false;
        // the text box typed in last, done or not, 0 for none yet
        ImGuiID lastBox = 0;
        // a mouse button is down: the focus goes back only once it's let go,
        // else the click that ended the typing (on the box's X) is lost
        bool mouseHeld = false;
    };

    enum class Change {
        None,
        Took,
        // Windows didn't give us the focus, the keys still go where they went
        CouldntTake,
        GaveBack,
        // the player clicked another window, the focus went there with the click
        Lost,
    };

    class Focus {
    public:
        // A text box that keeps the focus after Enter or Esc, for the next box.
        // Called every frame it's drawn. The others give it back.
        void stayAfter(ImGuiID box);
        // once a frame, after every text box is drawn
        Change update(const Frame& frame, Windows& windows);
        // the overlay has the focus for typing, so it reads the menu key itself
        bool hasFocus() const { return focused; }

    private:
        enum class State { Off, Typing, Holding };
        State state = State::Off;
        bool focused = false;
        HWND from = nullptr;
        std::vector<ImGuiID> staying;
        void stop();
    };

    // the overlay's own
    extern Focus focus;

    // An ImGui text box for every text box in the overlay. Esc keeps what was
    // typed, where ImGui would put the old text back. backToGame false keeps
    // the focus after Enter or Esc, for the next box. Holds up to maxBytes - 1
    // bytes. True when the text changed.
    bool InputText(const char* label, std::string& text, size_t maxBytes, bool backToGame = true,
        ImGuiInputTextFlags flags = 0, const char* hint = nullptr);
}
