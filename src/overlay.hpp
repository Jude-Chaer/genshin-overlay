#pragma once
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

namespace Overlay {
    extern GLFWwindow* Window;
    extern ImGuiIO* IO;
    extern GLFWmonitor* mainMonitor;
    extern const GLFWvidmode* monitorVideoMode;
    extern bool menuOpen;

    bool Init();
    void Shutdown();
    void MainLoop();
    void SetMenuOpen(bool open);
    // the overlay takes the mouse over this area next frame, so clicks there don't reach the game
    void ClaimMouse(float x0, float y0, float x1, float y1);
}
