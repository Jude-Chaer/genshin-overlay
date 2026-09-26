#include <windows.h>
#include "overlay.hpp"
#include "keybindings.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "map_tracking.hpp"
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <iostream>
#include <string>

namespace Overlay {
    GLFWwindow* Window = nullptr;
    ImGuiIO* IO = nullptr;
    GLFWmonitor* mainMonitor = nullptr;
    const GLFWvidmode* monitorVideoMode = nullptr;
    bool menuOpen = false;

    static HWND previousForeground = nullptr;
    static std::string iniPath;

    bool Init() {
        Overlay::mainMonitor = glfwGetPrimaryMonitor();
        Overlay::monitorVideoMode = glfwGetVideoMode(Overlay::mainMonitor);

        // borderless, transparent, always on top, and click-through until the menu opens
        const char* glsl_version = "#version 130";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
        glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
        glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        // 1px shorter than the monitor, otherwise Windows treats it as a fullscreen app
        Overlay::Window = glfwCreateWindow(Overlay::monitorVideoMode->width, Overlay::monitorVideoMode->height - 1, "Genshin Overlay", NULL, NULL);
        if (!Overlay::Window) {
            std::cerr << "Failed to create the overlay window" << std::endl;
            return false;
        }

        int monitorX = 0, monitorY = 0;
        glfwGetMonitorPos(Overlay::mainMonitor, &monitorX, &monitorY);
        glfwSetWindowPos(Overlay::Window, monitorX, monitorY);

        // no taskbar button or alt-tab entry
        HWND hwnd = glfwGetWin32Window(Overlay::Window);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
        glfwShowWindow(Overlay::Window);

        glfwMakeContextCurrent(Overlay::Window);
        glfwSwapInterval(1);

        if (glewInit() != GLEW_OK) {
            std::cerr << "Failed to initialize GLEW" << std::endl;
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        Overlay::IO = &ImGui::GetIO();
        // keep imgui.ini next to the exe, not wherever it was launched from
        iniPath = (Helpers::exeDirectory() / "imgui.ini").string();
        Overlay::IO->IniFilename = iniPath.c_str();

        ImGui::StyleColorsDark();

        // scale the UI for high DPI screens
        float scaleX = 1.0f, scaleY = 1.0f;
        glfwGetMonitorContentScale(Overlay::mainMonitor, &scaleX, &scaleY);
        ImGui::GetStyle().ScaleAllSizes(scaleX);
        ImGui::GetStyle().FontScaleDpi = scaleX;

        ImGui_ImplGlfw_InitForOpenGL(Overlay::Window, true);
        ImGui_ImplOpenGL3_Init(glsl_version);

        // ` toggles the menu, so it has to work while the menu is closed too
        Keybindings::Init(Overlay::Window);
        Keybindings::CreateKeybind(GLFW_KEY_GRAVE_ACCENT, []() {
            Overlay::SetMenuOpen(!Overlay::menuOpen);
        }, Keybindings::KeybindFlags_ProcessWhileHidden);

        return true;
    }

    void Shutdown() {
        Keybindings::Shutdown();

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        glfwDestroyWindow(Overlay::Window);
        glfwTerminate();
    }

    void SetMenuOpen(bool open) {
        if (Overlay::menuOpen == open) return;
        Overlay::menuOpen = open;

        // with the menu open the overlay takes the mouse, closed it goes to the game
        HWND hwnd = glfwGetWin32Window(Overlay::Window);
        glfwSetWindowAttrib(Overlay::Window, GLFW_MOUSE_PASSTHROUGH, open ? GLFW_FALSE : GLFW_TRUE);

        // take focus while the menu is open, then give it back to the game
        if (open) {
            previousForeground = GetForegroundWindow();
            SetForegroundWindow(hwnd);
        }
        else if (previousForeground && IsWindow(previousForeground)) {
            SetForegroundWindow(previousForeground);
            previousForeground = nullptr;
        }

        // extension keybinds only hold their keys while the menu is open
        Keybindings::RefreshRegistrations();
    }

    // what the tracker sees right now, mainly for testing until markers are drawn
    static void DrawMapStatus() {
        switch (MapTracking::GetStatus()) {
        case MapTracking::Status::NoData:
            ImGui::TextWrapped("Couldn't get the map data. Check your internet connection and restart.");
            return;
        case MapTracking::Status::Downloading: {
            int percent = 0;
            std::string stage = MapTracking::GetDownloadStage(percent);
            ImGui::Text("%s", stage.c_str());
            ImGui::ProgressBar(percent / 100.0f);
            return;
        }
        case MapTracking::Status::Loading:
            ImGui::Text("Loading map data...");
            return;
        case MapTracking::Status::Failed:
            ImGui::Text("Map data failed to load, check the console");
            return;
        case MapTracking::Status::Ready:
            break;
        }

        MapTracking::MapView view = MapTracking::GetView();
        if (!view.visible) {
            ImGui::Text("Waiting for the in-game map (M)");
            return;
        }

        const MapLocator::Result& r = view.result;
        ImGui::Text("%s", MapTracking::GetMapName(r.mapId));
        if (r.groupId != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("underground %d / floor %d", r.groupId, r.floorId);
        }
        ImGui::Text("Center: %.0f, %.0f", r.lng, r.lat);
        ImGui::Text("Zoom: %.2f units/px  (%d inliers)", r.unitsPerPixel, r.inliers);
    }

    static void DrawMenu() {
        ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);

        bool open = true;
        ImGui::Begin("Genshin Overlay", &open);
        ImGui::TextDisabled("Press ` to close this menu");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Map", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawMapStatus();
        }

        if (ImGui::CollapsingHeader("Extensions", ImGuiTreeNodeFlags_DefaultOpen)) {
            Extensions::drawExtensionMenus();
        }

        ImGui::Separator();
        if (ImGui::Button("Quit")) {
            glfwSetWindowShouldClose(Overlay::Window, true);
        }
        ImGui::End();

        if (!open) {
            Overlay::SetMenuOpen(false);
        }
    }

    void MainLoop() {
        while (!glfwWindowShouldClose(Overlay::Window)) {
            glfwPollEvents();
            Keybindings::ProcessKeybindings();
            // sends the tracker a new screenshot when it's ready for one
            MapTracking::Tick(glfwGetWin32Window(Overlay::Window));

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            Extensions::frameUpdateExtensions();
            if (Overlay::menuOpen) {
                DrawMenu();
            }

            ImGui::Render();

            int width = 0, height = 0;
            glfwGetFramebufferSize(Overlay::Window, &width, &height);
            glViewport(0, 0, width, height);
            // clear to fully transparent so only the UI is visible
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(Overlay::Window);
        }
    }
}
