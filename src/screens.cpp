#include "screens.hpp"
#include "map_tracking.hpp"
#include "overlay.hpp"

namespace Screens {
    bool Up::any() const {
        for (bool up : on) if (up) return true;
        return false;
    }

    Up now() {
        Up up;
        up.set(Screen::Map, MapTracking::IsMapOpen());
        up.set(Screen::Menu, Overlay::MenuOpen());
        return up;
    }
}
