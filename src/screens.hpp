#pragma once
#include <array>
#include <vector>

// The screens the overlay knows and can tell are up. Extensions get these by
// name (GetScreens, the Screens trigger), and what counts "while a screen is
// up" (clicks on what an extension draws) goes by this list. A screen the
// overlay learns to tell is added here.
namespace Screens {
    enum class Screen { Map, Menu, Count };

    struct Known {
        Screen screen;
        // the name extensions get
        const char* name;
    };

    // in the order GetScreens and the docs list them
    inline const std::vector<Known> list = {
        { Screen::Map, "map" },
        { Screen::Menu, "menu" },
    };

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
