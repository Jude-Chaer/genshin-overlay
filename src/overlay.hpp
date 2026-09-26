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
}
