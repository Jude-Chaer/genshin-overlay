#include "typing.hpp"
#include <algorithm>
#include <cstring>

namespace Typing {
    Focus focus;

    void Focus::stayAfter(ImGuiID box) {
        staying.push_back(box);
    }

    void Focus::stop() {
        state = State::Off;
        focused = false;
        from = nullptr;
    }

    Change Focus::update(const Frame& frame, Windows& windows) {
        bool stays = std::find(staying.begin(), staying.end(), frame.lastBox) != staying.end();
        staying.clear();
        // text boxes are only in the menu
        bool typing = frame.menuOpen && frame.typing;

        if (state == State::Off) {
            if (!typing) return Change::None;
            state = State::Typing;
            from = windows.foreground();
            focused = windows.takeFocus();
            // the box stays usable, the focus may still come
            return focused ? Change::Took : Change::CouldntTake;
        }

        HWND inFront = windows.foreground();
        if (!focused && inFront == windows.overlay) focused = true;
        // no window in front for a moment happens while the game minimizes
        if (focused && inFront && inFront != windows.overlay) {
            windows.giveFocus(nullptr);
            stop();
            return Change::Lost;
        }

        if (typing) {
            state = State::Typing;
            return Change::None;
        }
        if (frame.menuOpen && (state == State::Holding || stays)) {
            state = State::Holding;
            return Change::None;
        }
        if (frame.menuOpen && frame.mouseHeld) return Change::None;

        // done, or the menu closed
        HWND to = nullptr;
        if (focused) to = from && windows.exists(from) ? from : windows.game();
        windows.giveFocus(to);
        stop();
        return Change::GaveBack;
    }

    bool InputText(const char* label, std::string& text, size_t maxBytes, bool backToGame, ImGuiInputTextFlags flags, const char* hint) {
        // one buffer for every box, they're drawn one after the other on the draw thread
        static std::vector<char> buffer;
        buffer.assign(maxBytes, '\0');
        strncpy_s(buffer.data(), buffer.size(), text.c_str(), _TRUNCATE);
        bool changed = hint ? ImGui::InputTextWithHint(label, hint, buffer.data(), buffer.size(), flags)
            : ImGui::InputText(label, buffer.data(), buffer.size(), flags);
        if (!backToGame) focus.stayAfter(ImGui::GetItemID());
        // Esc: ImGui gives back the text from before, keep the typed one
        if (changed && ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return false;
        if (changed) text = buffer.data();
        return changed;
    }
}
