#include <windows.h>
#include <dwmapi.h>
#include "overlay.hpp"
#include "keybindings.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "map_tiles.hpp"
#include "map_tracking.hpp"
#include "stats.hpp"
#include "settings.hpp"
#include "capture.hpp"
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

    static std::string iniPath;
    static double startTime = 0.0;

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

        // No taskbar button, and never take focus. Taking focus from the game made
        // the two fight over it (the cursor kept flashing), and clicks on the menu
        // still work without focus.
        HWND hwnd = glfwGetWin32Window(Overlay::Window);
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        exStyle = (exStyle & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
        glfwShowWindow(Overlay::Window);

        glfwMakeContextCurrent(Overlay::Window);
        // No vsync from the driver, its wait for the screen spun a whole core
        // the entire time we draw. DwmFlush after each frame waits instead.
        glfwSwapInterval(0);

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

        // Alt+` toggles the menu, so it has to work while the menu is closed too.
        // A plain ` didn't reach us while the game had focus (without admin),
        // Alt+` does. Holding Alt also frees the cursor in game, so the menu can
        // be clicked right away.
        Keybindings::Init(Overlay::Window);
        Keybindings::CreateKeybind(GLFW_KEY_GRAVE_ACCENT, []() {
            Overlay::SetMenuOpen(!Overlay::menuOpen);
        }, Keybindings::KeybindFlags_ProcessWhileHidden, Keybindings::KeybindModifiers_Alt);

        startTime = glfwGetTime();
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

    static void keepOnTop(bool force);

    void SetMenuOpen(bool open) {
        Overlay::menuOpen = open;
        keepOnTop(true);
        // extension keybinds only hold their keys while the menu is open
        Keybindings::RefreshRegistrations();
    }

    // The window never has focus and is usually click-through, so it can't see
    // the mouse by itself. We give ImGui the cursor position every frame, that's
    // also how we know when the cursor is over the menu.
    static void feedMousePosition() {
        POINT cursor;
        GetCursorPos(&cursor);
        int windowX = 0, windowY = 0;
        glfwGetWindowPos(Overlay::Window, &windowX, &windowY);
        Overlay::IO->AddMousePosEvent((float)(cursor.x - windowX), (float)(cursor.y - windowY));
    }

    // Being "topmost" isn't enough on its own: when the fullscreen game comes to
    // the front, Windows can put it above windows that were made topmost
    // earlier. Without admin that's what happens, so we put ourselves back on
    // top when the game comes to the front and every half second after that.
    static void keepOnTop(bool force) {
        static double lastRaise = 0.0;
        static bool gameWasInFront = false;

        HWND game = Capture::findGameWindow();
        bool gameInFront = game && GetForegroundWindow() == game;
        bool justCameToFront = gameInFront && !gameWasInFront;
        gameWasInFront = gameInFront;

        double now = glfwGetTime();
        if (!force && !justCameToFront && !(gameInFront && now - lastRaise > 0.5)) return;
        lastRaise = now;

        SetWindowPos(glfwGetWin32Window(Overlay::Window), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Only take the mouse while it's over the menu, everywhere else clicks go to the game.
    // The window covers the whole screen, so without this an open menu would swallow
    // every click. WantCaptureMouse is ImGui saying the cursor is over one of its windows.
    static bool mouseCaptured = false;
    static void updateClickThrough() {
        bool capture = Overlay::menuOpen && Overlay::IO->WantCaptureMouse;
        if (capture == mouseCaptured) return;
        mouseCaptured = capture;
        glfwSetWindowAttrib(Overlay::Window, GLFW_MOUSE_PASSTHROUGH, capture ? GLFW_FALSE : GLFW_TRUE);
        // changing the window style can drop it below the game
        keepOnTop(true);
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

        ImGui::TextDisabled(Capture::isCapturing() ? "Capture: WGC" : "Capture: not working");

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
        ImGui::TextDisabled("updated %.1f s ago", view.secondsSinceUpdate);
    }

    // a short note at the top of the screen after starting, so you know it's running
    static void DrawStartupHint() {
        double elapsed = glfwGetTime() - startTime;
        if (elapsed > 6.0) return;

        float alpha = elapsed > 5.0 ? (float)(6.0 - elapsed) : 1.0f;
        ImGui::SetNextWindowPos(ImVec2(Overlay::IO->DisplaySize.x * 0.5f, 40.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.8f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::Begin("##startup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("Genshin Overlay is running. Press Alt+` for the menu.");
        ImGui::End();
        ImGui::PopStyleVar();
    }

    static void DrawMenu() {
        ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);

        bool open = true;

        ImGui::Begin(
            "Genshin Overlay",
            &open,
            ImGuiWindowFlags_NoCollapse
        );



        // 0 = Home
        // 1 = Settings
        // 2 = Usage
        // 3+ = extensions
        static int activeTab = 0;

        static float sidebarWidth = 150.0f;

        constexpr float sidebarMinWidth = 100.0f;
        constexpr float sidebarMaxWidth = 300.0f;
        constexpr float splitterWidth = 5.0f;
        constexpr float quitButtonWidth = 80.0f;


        ImVec2 contentSize = ImGui::GetContentRegionAvail();

        ImGui::BeginChild(
            "Sidebar",
            ImVec2(sidebarWidth, contentSize.y),
            true,
            ImGuiWindowFlags_NoScrollbar
        );

        auto DrawTab = [&](const char* label, int tabIndex) {
            bool selected = activeTab == tabIndex;

            ImVec2 size(
                ImGui::GetContentRegionAvail().x,
                65.0f
            );

            if (selected) {
                ImGui::PushStyleColor(
                    ImGuiCol_Button,
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered)
                );
            }

            if (ImGui::Button(label, size)) {
                activeTab = tabIndex;
            }

            if (selected) {
                ImGui::PopStyleColor();
            }
            };

        DrawTab("Home", 0);
        DrawTab("Settings", 1);
        DrawTab("Usage", 2);

        if (!Extensions::registeredExtensions.empty()) {
            ImGui::Separator();
        }

        for (size_t i = 0; i < Extensions::registeredExtensions.size(); ++i) {
            auto& ext = Extensions::registeredExtensions[i];

            DrawTab(
                ext->name.c_str(),
                static_cast<int>(i) + 3
            );
        }

        ImGui::EndChild();


        ImGui::SameLine(0.0f, 0.0f);

        ImGui::InvisibleButton(
            "##SidebarSplitter",
            ImVec2(splitterWidth, contentSize.y)
        );

        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        if (ImGui::IsItemActive()) {
            sidebarWidth += ImGui::GetIO().MouseDelta.x;

            sidebarWidth = std::clamp(
                sidebarWidth,
                sidebarMinWidth,
                sidebarMaxWidth
            );
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImVec2 splitterMin = ImGui::GetItemRectMin();
        ImVec2 splitterMax = ImGui::GetItemRectMax();

        drawList->AddRectFilled(
            splitterMin,
            splitterMax,
            ImGui::GetColorU32(ImGuiCol_Border)
        );

        ImGui::SameLine(0.0f, 0.0f);

        ImGui::BeginChild(
            "MainContent",
            ImVec2(0, contentSize.y),
            true,
            ImGuiWindowFlags_AlwaysVerticalScrollbar
        );
        
        switch (activeTab) {
        case 0:

            ImGui::TextDisabled("Press Alt+` to hide");


            ImGui::SameLine();

            ImGui::SetCursorPosX(
                ImGui::GetWindowContentRegionMax().x - quitButtonWidth
            );

            if (ImGui::Button("Shutdown", ImVec2(quitButtonWidth, 0))) {
                glfwSetWindowShouldClose(Overlay::Window, true);
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader(
                "Map",
                ImGuiTreeNodeFlags_DefaultOpen
            )) {
                DrawMapStatus();

                bool instantClose = Settings::runAsAdmin;

                if (ImGui::Checkbox(
                    "Close instantly on M and Esc (needs admin)",
                    &instantClose
                )) {
                    Settings::runAsAdmin = instantClose;
                    Settings::Save();

                    if (instantClose && !Helpers::isElevated()) {
                        Keybindings::SetEnabled(false);

                        if (Helpers::relaunchAsAdmin()) {
                            glfwHideWindow(Overlay::Window);
                            glfwSetWindowShouldClose(
                                Overlay::Window,
                                true
                            );
                        }
                        else {
                            Keybindings::SetEnabled(true);
                        }
                    }
                }

                if (!instantClose && Helpers::isElevated()) {
                    ImGui::TextDisabled(
                        "Starts without admin next time"
                    );
                }
            }

            if (ImGui::CollapsingHeader("Advanced")) {
                ImGui::TextDisabled("How the map is followed while it moves");

                if (ImGui::Checkbox(
                    "Ignore open water",
                    &Settings::ignoreSea
                )) {
                    Settings::Save();
                }

                if (ImGui::Checkbox(
                    "Draw ahead of the map while it moves",
                    &Settings::guessAhead
                )) {
                    Settings::Save();
                }

                if (ImGui::Checkbox(
                    "Don't draw ahead while zooming",
                    &Settings::steadyZoom
                )) {
                    Settings::Save();
                }

                if (ImGui::Checkbox(
                    "Look again when a still map changes (floors)",
                    &Settings::lookAgainStill
                )) {
                    Settings::Save();
                }

                if (ImGui::Checkbox(
                    "Follow on its own thread",
                    &Settings::followOnThread
                )) {
                    Settings::Save();
                }

                if (ImGui::Checkbox(
                    "Shrink the patches on the graphics card",
                    &Settings::shrinkOnGpu
                )) {
                    Settings::Save();
                }
            }

            break;

        case 1:
            ImGui::Text("Settings");
            ImGui::Separator();

            break;

        case 2:
            ImGui::Text("Usage");
            ImGui::Separator();

            Stats::Draw();

            break;

        default:
            Extensions::drawExtensionMenus(
                activeTab - 3
            );

            break;
        }

        ImGui::EndChild();

        ImGui::End();

        if (!open) {
            Overlay::SetMenuOpen(false);
        }
    }

    void MainLoop() {
        bool drewLastFrame = true;
        while (!glfwWindowShouldClose(Overlay::Window)) {
            glfwPollEvents();
            Keybindings::ProcessKeybindings();
            keepOnTop(false);
            // sends the tracker a new screenshot when it's ready for one
            MapTracking::Tick(glfwGetWin32Window(Overlay::Window));
            Stats::Update();

            // map and menu closed: nothing to show, so don't draw at all
            bool showing = Overlay::menuOpen || MapTracking::GetView().visible || glfwGetTime() - startTime < 6.0;
            if (!showing) {
                updateClickThrough();
                if (drewLastFrame) {
                    // an empty frame so the last one doesn't stay on screen
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glfwSwapBuffers(Overlay::Window);
                    drewLastFrame = false;
                }
                // The map is open but not found yet: nothing to draw, but the
                // tracker still has to keep up with the game's frames.
                if (MapTracking::IsMapOpen()) DwmFlush();
                else glfwWaitEventsTimeout(0.05);
                continue;
            }
            drewLastFrame = true;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            feedMousePosition();
            ImGui::NewFrame();
            updateClickThrough();

            Extensions::frameUpdateExtensions();
            DrawStartupHint();
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
            // sleeps until Windows put the frame on screen, once per refresh,
            // or until the map's next answer is there
            MapTracking::WaitForDraw();
            MapTiles::EndFrame();
        }
    }
}
