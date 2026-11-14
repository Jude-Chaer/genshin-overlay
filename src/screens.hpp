#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <opencv2/core.hpp>

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
        // whether GetScreenPicture gives it. Not the menu: the overlay's own
        // menu isn't in the picture, only whatever game screen is behind it.
        bool picture;
    };

    // in the order GetScreens and the docs list them
    inline const std::vector<Known> list = {
        { Screen::Map, "map", true },
        { Screen::Menu, "menu", false },
    };

    // Parts of a screen no extension sees: black in the pictures
    // GetScreenPicture gives. In 2000x1125 units, kept to the bottom right
    // corner like the game's UI.
    struct Hidden {
        Screen screen;
        double x0, y0, x1, y1;
    };

    inline const std::vector<Hidden> hidden = {
        { Screen::Map, 1745, 1095, 1945, 1122 }, // UID
    };

    // whether GetScreenPicture gives this screen
    bool pictured(Screen screen);

    // whether the overlay saw the screen up on this game frame, so a picture
    // of it shows that screen and nothing else
    bool shownOn(Screen screen, uint64_t frame);

    // blacks out the screen's hidden parts in a picture of the whole game window, BGR or BGRA
    void blackOut(cv::Mat& picture, Screen screen);

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
