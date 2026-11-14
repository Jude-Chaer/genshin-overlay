#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include "typing.hpp"

using Typing::Change;

namespace {
    // Windows as the typing sees it: who's in front, and what it was told to do
    struct FakeWindows {
        HWND overlay = (HWND)1, game = (HWND)2, discord = (HWND)3;
        HWND inFront = game;
        bool allowFocus = true;
        bool discordOpen = true;
        int gaveBack = 0;
        HWND gaveTo = nullptr;
        Typing::Windows windows;

        FakeWindows() {
            windows.overlay = overlay;
            windows.foreground = [this]() { return inFront; };
            windows.game = [this]() { return game; };
            windows.exists = [this](HWND window) { return window != discord || discordOpen; };
            windows.takeFocus = [this]() {
                if (allowFocus) inFront = overlay;
                return allowFocus;
            };
            windows.giveFocus = [this](HWND to) {
                gaveBack++;
                gaveTo = to;
                if (to) inFront = to;
            };
        }
    };

    Typing::Frame typing(ImGuiID box) {
        Typing::Frame frame;
        frame.menuOpen = true;
        frame.typing = true;
        frame.lastBox = box;
        return frame;
    }

    Typing::Frame done(ImGuiID box, bool menuOpen = true) {
        Typing::Frame frame;
        frame.menuOpen = menuOpen;
        frame.lastBox = box;
        return frame;
    }
}

TEST_CASE("a text box takes the focus and gives it back to the game when done") {
    Typing::Focus focus;
    FakeWindows w;
    CHECK(focus.update(done(0), w.windows) == Change::None);
    CHECK(focus.update(typing(7), w.windows) == Change::Took);
    CHECK(w.inFront == w.overlay);
    CHECK(focus.hasFocus());
    CHECK(focus.update(typing(7), w.windows) == Change::None);
    CHECK(focus.update(done(7), w.windows) == Change::GaveBack);
    CHECK(w.inFront == w.game);
    CHECK_FALSE(focus.hasFocus());
    CHECK(w.gaveBack == 1);
}

TEST_CASE("the focus goes back only once the mouse is let go, so the click that ended the typing counts") {
    Typing::Focus focus;
    FakeWindows w;
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    // pressed on the box's X: the box isn't typed in any more, the button is still down
    Typing::Frame pressing = done(7);
    pressing.mouseHeld = true;
    CHECK(focus.update(pressing, w.windows) == Change::None);
    CHECK(w.inFront == w.overlay);
    CHECK(focus.update(done(7), w.windows) == Change::GaveBack);
    CHECK(w.inFront == w.game);
    // the menu closing with the button down still gives it back
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    Typing::Frame closed = done(7, false);
    closed.mouseHeld = true;
    CHECK(focus.update(closed, w.windows) == Change::GaveBack);
}

TEST_CASE("the focus goes back to the window it came from, or the game once that's gone") {
    Typing::Focus focus;
    FakeWindows w;
    w.inFront = w.discord;
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    REQUIRE(focus.update(done(7), w.windows) == Change::GaveBack);
    CHECK(w.gaveTo == w.discord);

    w.inFront = w.discord;
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    w.discordOpen = false;
    REQUIRE(focus.update(done(7), w.windows) == Change::GaveBack);
    CHECK(w.gaveTo == w.game);
}

TEST_CASE("a box with backToGame false keeps the focus until the menu closes") {
    Typing::Focus focus;
    FakeWindows w;
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    focus.stayAfter(7);
    CHECK(focus.update(done(7), w.windows) == Change::None);
    CHECK(w.inFront == w.overlay);
    // the next box, the overlay's own
    CHECK(focus.update(typing(8), w.windows) == Change::None);
    CHECK(focus.update(done(8), w.windows) == Change::GaveBack);
    CHECK(w.inFront == w.game);

    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    focus.stayAfter(7);
    REQUIRE(focus.update(done(7), w.windows) == Change::None);
    // the box is still there and says so every frame
    focus.stayAfter(7);
    CHECK(focus.update(done(7), w.windows) == Change::None);
    CHECK(focus.update(done(7, false), w.windows) == Change::GaveBack);
    CHECK(w.inFront == w.game);
}

TEST_CASE("a box that stays only counts for itself") {
    Typing::Focus focus;
    FakeWindows w;
    REQUIRE(focus.update(typing(8), w.windows) == Change::Took);
    // another box on the page keeps the focus, the one just done doesn't
    focus.stayAfter(7);
    CHECK(focus.update(done(8), w.windows) == Change::GaveBack);
}

TEST_CASE("a click on another window ends the typing, the focus stays there") {
    Typing::Focus focus;
    FakeWindows w;
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    w.inFront = w.discord;
    CHECK(focus.update(typing(7), w.windows) == Change::Lost);
    CHECK(w.gaveTo == nullptr);
    CHECK(w.inFront == w.discord);
    CHECK_FALSE(focus.hasFocus());
    // typing in a box again takes it again
    CHECK(focus.update(typing(7), w.windows) == Change::Took);
}

TEST_CASE("a moment with no window in front doesn't end the typing") {
    Typing::Focus focus;
    FakeWindows w;
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    // Windows has nothing in front for a moment while the game minimizes
    w.inFront = nullptr;
    CHECK(focus.update(typing(7), w.windows) == Change::None);
    w.inFront = w.overlay;
    CHECK(focus.update(typing(7), w.windows) == Change::None);
    CHECK(focus.hasFocus());
}

TEST_CASE("closing the menu while typing gives the focus back") {
    Typing::Focus focus;
    FakeWindows w;
    REQUIRE(focus.update(typing(7), w.windows) == Change::Took);
    Typing::Frame closed = typing(7);
    closed.menuOpen = false;
    CHECK(focus.update(closed, w.windows) == Change::GaveBack);
    CHECK(w.inFront == w.game);
}

TEST_CASE("when Windows won't give the focus the box stays usable and nothing is moved") {
    Typing::Focus focus;
    FakeWindows w;
    w.allowFocus = false;
    CHECK(focus.update(typing(7), w.windows) == Change::CouldntTake);
    CHECK_FALSE(focus.hasFocus());
    // asked once, not every frame
    CHECK(focus.update(typing(7), w.windows) == Change::None);
    CHECK(focus.update(done(7), w.windows) == Change::GaveBack);
    // the game kept the focus all along, nothing is brought to the front
    CHECK(w.gaveTo == nullptr);
    CHECK(w.inFront == w.game);
}

namespace {
    // frames the way the overlay makes them, with no screen to show them on
    struct Frames {
        ImGuiContext* context = ImGui::CreateContext();
        Frames() {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }
        ~Frames() { ImGui::DestroyContext(context); }

        // one frame with the box in a window, the first one has the keyboard on it
        bool box(std::string& text, bool focusFirst = false) {
            ImGui::NewFrame();
            ImGui::Begin("page");
            if (focusFirst) ImGui::SetKeyboardFocusHere();
            bool changed = Typing::InputText("box", text, 64);
            ImGui::End();
            ImGui::EndFrame();
            return changed;
        }
    };
}

TEST_CASE("Esc keeps what was typed in a box") {
    Frames frames;
    std::string text = "old";
    frames.box(text, true);
    frames.box(text);
    REQUIRE(ImGui::GetActiveID() != 0);
    // focused from the keyboard, so the old text is selected and typing replaces it
    ImGui::GetIO().AddInputCharactersUTF8("new");
    CHECK(frames.box(text));
    CHECK(text == "new");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    CHECK_FALSE(frames.box(text));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frames.box(text);
    CHECK(ImGui::GetActiveID() == 0);
    CHECK(text == "new");
}

TEST_CASE("Enter keeps what was typed too") {
    Frames frames;
    std::string text = "";
    frames.box(text, true);
    frames.box(text);
    ImGui::GetIO().AddInputCharactersUTF8("abc");
    frames.box(text);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    frames.box(text);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    frames.box(text);
    CHECK(ImGui::GetActiveID() == 0);
    CHECK(text == "abc");
}
