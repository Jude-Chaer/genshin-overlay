#pragma once
#include <array>

// The screens the overlay knows and can tell are up. What counts "while a
// screen is up" goes by this list. A screen the overlay learns to tell is
// added here.
namespace Screens {
    enum class Screen { Map, Menu, Count };

    // which of them are up, more than one at a time is possible
    struct Up {
        std::array<bool, (size_t)Screen::Count> on = {};
        bool operator[](Screen screen) const { return on[(size_t)screen]; }
        void set(Screen screen, bool up) { on[(size_t)screen] = up; }
        bool any() const;
    };

    // the big map is open (its zoom bar is on screen), the menu is open
    Up now();
}
