#pragma once
#include <windows.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <string>
#include "screens.hpp"
#include <vector>

namespace Overlay {
    extern GLFWwindow* Window;
    extern ImGuiIO* IO;
    extern GLFWmonitor* mainMonitor;
    extern const GLFWvidmode* monitorVideoMode;

    bool Init();
    void Shutdown();
    void MainLoop();
    // the menu only opens and closes through SetMenuOpen, it also takes or
    // gives back the extensions' keys
    bool MenuOpen();
    void SetMenuOpen(bool open);
    // the game in front again, restored if it minimized, e.g. after a file window
    void GiveFocusToGame();
    // opens the menu on that extension's page
    void ShowExtension(int index);
    // "Hide on map close": the menu opens and closes with the map
    void MenuFollowsMap(bool mapOpen);
    // Boxes in overlay pixels where the overlay takes the mouse from the game,
    // so a click there lands on what's drawn. Claimed again every frame.
    class MouseClaims {
    public:
        struct Box {
            ImVec4 rect;
            // the extension's name, empty for the overlay's own
            std::string owner;
        };
        void claim(float left, float top, float right, float bottom, const std::string& owner = {});
        // Whether the overlay takes the mouse at `at`: over the menu while it's
        // open, or over a box claimed since the last call. Boxes only count
        // while one of the screens the overlay knows is up: with none up the
        // cursor is locked in the game, and a box would eat its clicks. Empties
        // the boxes for the next frame.
        bool takesMouse(ImVec2 at, const Screens::Up& up, bool overMenu);
        // the boxes the last takesMouse went by
        const std::vector<Box>& counted() const { return last; }
    private:
        std::vector<Box> boxes;
        std::vector<Box> last;
    };
    // Takes the mouse over this box from the next frame on, while one of the
    // known screens is up. Call it every frame the box is there.
    void ClaimMouse(float left, float top, float right, float bottom, const std::string& owner = {});
    // Where the floor picker was last drawn, in overlay pixels. False while
    // it isn't on screen.
    bool PickerBox(RECT& out);
}
