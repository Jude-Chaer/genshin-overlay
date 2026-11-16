#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include "overlay.hpp"
#include "accounts.hpp"
#include "notices.hpp"
#include "controls.hpp"
#include "keybindings.hpp"
#include "extensions.hpp"
#include "lua_functions.hpp"
#include "asks.hpp"
#include "file_dialogs.hpp"
#include "helpers.hpp"
#include "item_files.hpp"
#include "map_tiles.hpp"
#include "map_tracking.hpp"
#include "game_ui.hpp"
#include "minimap.hpp"
#include "stats.hpp"
#include "settings.hpp"
#include "capture.hpp"
#include "triggers.hpp"
#include "screen_pictures.hpp"
#include "key_choices.hpp"
#include "typing.hpp"
#include <imgui_internal.h>
#include "game_traffic_text.hpp"
#include "menu_summary.hpp"
#include "page_order.hpp"
#include "npcap_download.hpp"
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

namespace Overlay {
    GLFWwindow* Window = nullptr;
    ImGuiIO* IO = nullptr;
    GLFWmonitor* mainMonitor = nullptr;
    const GLFWvidmode* monitorVideoMode = nullptr;

    static bool menuOpen = false;
    // the menu's page: 0 = Home, 1 = Usage, 2+ = extensions
    static int activeTab = 0;
    // Home, Usage and Accounts come before the extensions' pages
    static const int OwnPages = 3;
    // the player's order and folders of the extensions' pages
    static PageOrder::List pageList;
    static bool pageListLoaded = false;
    // the row the page list's menu is open for
    static PageOrder::Place pageMenuAt;
    static int renamedFolder = -1;
    static bool renameAsked = false;
    static std::string folderName;
    // why the typed folder name wasn't taken, shown under the field
    static std::string renameProblem;
    static std::string iniPath;
    static double startTime = 0.0;

    static const std::vector<std::string> lookNames = { Controls::LookName(Controls::Look::Windows),
        Controls::LookName(Controls::Look::Material), Controls::LookName(Controls::Look::Game),
        Controls::LookName(Controls::Look::Apple) };
    static const std::vector<std::string> lightDarkNames = { "Follow Windows", "Dark", "Light" };
    static double lookCheckedAt = -1.0;
    // the look or size picked in the menu, put on before the next frame
    static bool lookPicked = false;
    // ImGui's sizes before any scaling, and Windows' scale for the screen
    static ImGuiStyle unscaled;
    static float screenScale = 1.0f;

    // The look and light or dark from the settings. With Follow Windows,
    // Windows is asked again once a second. The accent is read again when
    // Windows says it changed.
    static void updateLook(bool now) {
        double time = glfwGetTime();
        bool accentChanged = Helpers::accentChanged();
        if (accentChanged) Controls::SetAccent(Helpers::accentShades());
        bool follow = Settings::lightDark != "Dark" && Settings::lightDark != "Light";
        if (!now && !accentChanged && (!follow || time - lookCheckedAt < 1.0)) return;
        lookCheckedAt = time;
        Controls::Look look = Controls::LookNamed(Settings::look).value_or(Controls::Look::Windows);
        bool dark = follow ? Helpers::systemDark() : Settings::lightDark == "Dark";
        Controls::Theme was = Controls::GetTheme();
        if (!now && !accentChanged && was.look == look && was.dark == dark) return;
        Controls::SetLook(look, dark);
        // Windows' scale times the player's size, from the unscaled sizes so
        // picking sizes in turn doesn't round them off
        float scale = screenScale * Settings::size / 100.0f;
        ImGuiStyle& style = ImGui::GetStyle();
        if (style.FontScaleDpi != scale) {
            style = unscaled;
            style.ScaleAllSizes(scale);
            style.FontScaleDpi = scale;
        }
        Controls::ApplyStyle(scale);
    }

    // Segoe UI Variable (ImGui's own font without it), Material You's Roboto
    // and Apple's Inter, each loaded the first time its look is picked.
    // Apple's text is macOS's 13 px, its page names bold; Material's labels
    // and heads are Roboto's medium.
    static ImFont* menuFont = nullptr;
    struct LookFont {
        Controls::Look look;
        const char* folder;
        const char* file;
        const char* name;
        float size;
        const char* boldFile = nullptr;
        ImFont* font = nullptr;
        ImFont* bold = nullptr;
        bool tried = false;
    };
    static LookFont lookFonts[] = {
        { Controls::Look::Material, "roboto", "Roboto[wdth,wght].ttf", "Roboto", 14.0f, "Roboto-Medium.ttf" },
        { Controls::Look::Apple, "inter", "Inter[opsz,wght].ttf", "Inter", 13.0f, "Inter-Bold.ttf" },
    };

    // the look's font, between frames so the frame drawn has one font
    static void updateFont() {
        Controls::Look look = Controls::GetTheme().look;
        ImFont* font = menuFont;
        ImFont* bold = nullptr;
        float size = unscaled.FontSizeBase;
        for (LookFont& entry : lookFonts) {
            if (entry.look != look) continue;
            if (!entry.tried) {
                entry.tried = true;
                std::filesystem::path folder = Helpers::exeDirectory() / "fonts" / entry.folder;
                std::error_code ec;
                if (std::filesystem::exists(folder / entry.file, ec))
                    entry.font = Overlay::IO->Fonts->AddFontFromFileTTF(Helpers::utf8(folder / entry.file).c_str(), entry.size);
                if (!entry.font) std::cerr << entry.name << " is missing from fonts/" << entry.folder << ", " << Controls::LookName(look) << " uses the menu's font" << std::endl;
                else if (entry.boldFile && std::filesystem::exists(folder / entry.boldFile, ec))
                    entry.bold = Overlay::IO->Fonts->AddFontFromFileTTF(Helpers::utf8(folder / entry.boldFile).c_str(), entry.size);
            }
            if (entry.font) {
                font = entry.font;
                bold = entry.bold;
                size = entry.size;
            }
        }
        Overlay::IO->FontDefault = font;
        ImGui::GetStyle().FontSizeBase = size;
        Controls::SetTitleFont(bold);
    }

    static int indexOf(const std::vector<std::string>& names, const std::string& name) {
        auto it = std::find(names.begin(), names.end(), name);
        return it == names.end() ? 0 : (int)(it - names.begin());
    }

    // the menu's switch and an extension's ask both go through here
    static void setGameTraffic(bool on) {
        Settings::readGameTraffic = on;
        Settings::Save();
        if (on) GameTraffic::Start();
        else GameTraffic::Stop(false);
    }

    // A white disc with a soft edge, mipmapped, that badges tint for their
    // round parts.
    static ImTextureID makeDisc() {
        const int side = 128;
        std::vector<unsigned char> pixels(side * side * 4, 255);
        float middle = side / 2.0f, radius = side / 2.0f - 1;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                float d = std::hypot(x + 0.5f - middle, y + 0.5f - middle);
                float alpha = std::clamp(radius - d + 0.5f, 0.0f, 1.0f);
                pixels[(y * side + x) * 4 + 3] = (unsigned char)std::lround(alpha * 255);
            }
        }
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        return (ImTextureID)(intptr_t)texture;
    }

    bool Init() {
        Asks::addSetting({ "Game traffic", "Read the game's traffic", "It only reads, nothing is ever sent to the game.",
            []() { return Settings::readGameTraffic; }, []() { setGameTraffic(true); } });

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
        Overlay::Window = glfwCreateWindow(Overlay::monitorVideoMode->width, Overlay::monitorVideoMode->height - 1, "Pocket Esoptron", NULL, NULL);
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
        FileDialogs::giveFocusBack = []() { GiveFocusToGame(); };

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
        // keep imgui.ini in the data folder, not wherever it was launched from
        iniPath = (Helpers::dataDirectory() / "imgui.ini").string();
        Overlay::IO->IniFilename = iniPath.c_str();

        // Segoe UI Variable at Windows' 14 px, ImGui's own font without it
        std::filesystem::path font = Helpers::systemFont();
        if (!font.empty()) menuFont = Overlay::IO->Fonts->AddFontFromFileTTF(Helpers::utf8(font).c_str(), 14.0f);
        if (menuFont) ImGui::GetStyle().FontSizeBase = 14.0f;
        else menuFont = Overlay::IO->Fonts->AddFontDefault();

        // scale the UI for high DPI screens
        float scaleY = 1.0f;
        glfwGetMonitorContentScale(Overlay::mainMonitor, &screenScale, &scaleY);
        unscaled = ImGui::GetStyle();
        ImGui::GetStyle().FontScaleDpi = 0.0f;
        updateLook(true);

        ImGui_ImplGlfw_InitForOpenGL(Overlay::Window, true);
        ImGui_ImplOpenGL3_Init(glsl_version);
        Controls::SetDiscTexture(makeDisc());

        // The menu key (Alt+` unless the player picked another) toggles the
        // menu, so it has to work while the menu is closed too. A plain ` didn't
        // reach us while the game had focus (without admin), Alt+` does. Holding
        // Alt also frees the cursor in game, so the menu can be clicked right away.
        Keybindings::Init(Overlay::Window);
        KeyChoices::startMenuKey([]() {
            Overlay::SetMenuOpen(!menuOpen);
        });

        // the menu key only works with the game in front, so without the game the
        // menu opens by itself, the settings can be changed without it
        if (!Capture::findRunningGame()) SetMenuOpen(true);

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

    bool MenuOpen() {
        return menuOpen;
    }

    int ShownExtension() {
        if (!menuOpen || activeTab < OwnPages) return -1;
        return activeTab - OwnPages;
    }

    void ShowExtension(int index) {
        activeTab = index + OwnPages;
        SetMenuOpen(true);
    }

    void SetMenuOpen(bool open) {
        if (open == menuOpen) return;
        menuOpen = open;
        keepOnTop(true);
        // extension keybinds only hold their keys while the menu is open
        Keybindings::RefreshRegistrations();
    }

    void MenuFollowsMap(bool mapOpen) {
        if (Settings::hideOnMapClose) SetMenuOpen(mapOpen);
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
        Accounts::RaiseWindow();
    }

    void MouseClaims::claim(float left, float top, float right, float bottom, const std::string& owner) {
        boxes.push_back({ ImVec4(left, top, right, bottom), owner });
    }

    bool MouseClaims::takesMouse(ImVec2 at, const Screens::Up& up, bool overMenu) {
        bool overBox = false;
        for (const Box& box : boxes) {
            const ImVec4& r = box.rect;
            if (at.x >= r.x && at.x < r.z && at.y >= r.y && at.y < r.w) overBox = true;
        }
        last.swap(boxes);
        boxes.clear();
        if (!up.any()) {
            last.clear();
            return false;
        }
        return (up[Screens::Screen::Menu] && overMenu) || overBox;
    }

    static MouseClaims claims;

    void ClaimMouse(float left, float top, float right, float bottom, const std::string& owner) {
        claims.claim(left, top, right, bottom, owner);
    }

    // showClickBoxes: an outline round every box that takes the mouse now,
    // over the extensions' drawing and under the menu
    static void drawClickBoxes() {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const ImU32 colour = IM_COL32(255, 0, 255, 255);
        for (const MouseClaims::Box& box : claims.counted()) {
            ImVec2 min(box.rect.x, box.rect.y), max(box.rect.z, box.rect.w);
            draw->AddRect(min, max, colour, 0.0f, 0, 2.0f);
            if (!box.owner.empty()) draw->AddText(ImVec2(min.x + 3, min.y + 2), colour, box.owner.c_str());
        }
    }

    // Only take the mouse while it's over the menu or a claimed box, everywhere
    // else clicks go to the game. The window covers the whole screen, so without
    // this an open menu would swallow every click. WantCaptureMouse is ImGui
    // saying the cursor is over one of its windows.
    static bool mouseCaptured = false;
    // where the floor picker is drawn this frame, overlay pixels, empty with none
    static RECT pickerBox = {};

    bool PickerBox(RECT& out) {
        if (IsRectEmpty(&pickerBox)) return false;
        out = pickerBox;
        return true;
    }

    // The boxes are the ones claimed since the last call: the extensions' from
    // the frame before, the picker's from this one. Windows only sees the change
    // once the frame is done, so a click right as the cursor gets there can
    // still go to the game.
    // Not drawing: nothing is on screen, so no box counts.
    static void updateClickThrough(bool drawing) {
        bool capture = claims.takesMouse(Overlay::IO->MousePos, drawing ? Screens::now() : Screens::Up{},
            Overlay::IO->WantCaptureMouse);
        if (capture == mouseCaptured) return;
        mouseCaptured = capture;
        glfwSetWindowAttrib(Overlay::Window, GLFW_MOUSE_PASSTHROUGH, capture ? GLFW_FALSE : GLFW_TRUE);
        // changing the window style can drop it below the game
        keepOnTop(true);
    }

    static void giveFocus(HWND to) {
        HWND hwnd = glfwGetWin32Window(Overlay::Window);
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
        // A fullscreen game minimizes once it loses the focus, and we can't
        // restore it without admin. Bringing it to the front is allowed
        // though, and it restores itself then. SwitchToThisWindow in case
        // it didn't come.
        if (to) {
            SetForegroundWindow(to);
            if (GetForegroundWindow() != to) SwitchToThisWindow(to, TRUE);
        }
        keepOnTop(true);
    }

    // Typing (typing.hpp) with the real Windows calls.
    static Typing::Windows typingWindows() {
        Typing::Windows windows;
        windows.overlay = glfwGetWin32Window(Overlay::Window);
        windows.foreground = []() { return GetForegroundWindow(); };
        windows.game = []() { return Capture::findRunningGame(); };
        windows.exists = [](HWND window) { return IsWindow(window) != FALSE; };
        windows.takeFocus = []() {
            HWND hwnd = glfwGetWin32Window(Overlay::Window);
            LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
            bool took = SetForegroundWindow(hwnd) != FALSE;
            keepOnTop(true);
            return took;
        };
        windows.giveFocus = giveFocus;
        return windows;
    }

    void GiveFocusToGame() {
        giveFocus(Capture::findRunningGame());
    }

    // once a frame after the menu is drawn, drawing false when nothing is
    static void updateTyping(bool drawing) {
        static Typing::Windows windows = typingWindows();
        Typing::Frame frame;
        frame.menuOpen = menuOpen;
        if (drawing) {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            frame.lastBox = g.InputTextState.ID;
            frame.mouseHeld = ImGui::IsAnyMouseDown();
            frame.typing = (g.ActiveId != 0 && g.ActiveId == g.InputTextState.ID) || KeyChoices::recording();
            // With the focus here the game isn't in front, so every hotkey is
            // off, the menu key too. ImGui gets the keys now, so it's read here.
            ImGuiKeyChord menuKey = Keybindings::ImGuiChordFor(KeyChoices::menuKey(), KeyChoices::menuModifiers());
            // in a text box a Ctrl menu key is left to the box, like Ctrl+V
            bool inTextBox = g.ActiveId != 0 && g.ActiveId == g.InputTextState.ID;
            bool forTheBox = inTextBox && !(KeyChoices::menuModifiers() & Keybindings::KeybindModifiers_Alt);
            if (Typing::focus.hasFocus() && !KeyChoices::recording() && !forTheBox && ImGui::IsKeyChordPressed(menuKey)) {
                SetMenuOpen(false);
                frame.menuOpen = false;
            }
        }
        if (!frame.menuOpen) KeyChoices::stopRecording();
        switch (Typing::focus.update(frame, windows)) {
        case Typing::Change::CouldntTake:
            std::cout << "Couldn't take the focus for typing, the keys still go to the window in front" << std::endl;
            break;
        case Typing::Change::Lost:
            if (drawing) ImGui::ClearActiveID();
            KeyChoices::stopRecording();
            break;
        default:
            break;
        }
    }

    static MenuSummary::MapState mapState() {
        using Data = MenuSummary::MapState::Data;
        MenuSummary::MapState map;
        switch (MapTracking::GetStatus()) {
        case MapTracking::Status::NoData: map.data = Data::NoData; break;
        case MapTracking::Status::Downloading:
            map.data = Data::Downloading;
            map.stage = MapTracking::GetDownloadStage(map.percent);
            break;
        case MapTracking::Status::Loading: map.data = Data::Loading; break;
        case MapTracking::Status::Failed: map.data = Data::Failed; break;
        case MapTracking::Status::Ready: map.data = Data::Ready; break;
        }
        if (map.data != Data::Ready) return map;
        MapTracking::MapView view = MapTracking::GetView();
        map.visible = view.visible;
        map.mapOpen = MapTracking::IsMapOpen();
        if (view.visible) map.mapName = MapTracking::GetMapName(view.result.mapId);
        return map;
    }

    // what the tracker sees right now under the Map section's line, mainly for
    // testing until markers are drawn
    static void DrawMapStatus(const MenuSummary::MapText& text) {
        if (!text.detail.empty()) ImGui::TextWrapped("%s", text.detail.c_str());
        MapTracking::Status status = MapTracking::GetStatus();
        if (status == MapTracking::Status::Downloading) {
            int percent = 0;
            MapTracking::GetDownloadStage(percent);
            Controls::Progress("", percent / 100.0f);
            return;
        }
        if (status != MapTracking::Status::Ready) return;

        ImGui::TextDisabled(Capture::isCapturing() ? "Capture: WGC" : "Capture: not working");

        MapTracking::MapView view = MapTracking::GetView();
        if (!view.visible) return;
        const MapLocator::Result& r = view.result;
        if (r.groupId != 0) ImGui::TextDisabled("underground %d / floor %d", r.groupId, r.floorId);
        ImGui::Text("Center: %.0f, %.0f", r.lng, r.lat);
        ImGui::Text("Zoom: %.2f units/px  (%d inliers)", r.unitsPerPixel, r.inliers);
        ImGui::TextDisabled("updated %.1f s ago", view.secondsSinceUpdate);
    }

    // what the Map section's line doesn't say about the map items, and Try again
    static void DrawItemsStatus(const ItemFiles::Status& items, const MenuSummary::ItemsText& text) {
        if (items.state == ItemFiles::State::Downloading) Controls::Progress("Map items", items.percent / 100.0f);
        if (!text.detail.empty()) ImGui::TextWrapped("%s", text.detail.c_str());
        if (text.tryAgain) {
            ImGui::BeginDisabled(items.checking);
            if (Controls::Button("Try again")) ItemFiles::CheckNow();
            ImGui::EndDisabled();
        }
        if (Controls::Toggle("Download map items", &Settings::downloadItems)) {
            Settings::Save();
            ItemFiles::SetDownload(Settings::downloadItems);
        }
        Controls::ButtonOptions check;
        check.busy = items.checking;
        check.busyLabel = "Checking...";
        ImGui::BeginDisabled(!Settings::downloadItems);
        if (Controls::Button("Check for new map items", check)) ItemFiles::CheckNow();
        ImGui::EndDisabled();
    }

    // The overlay's own floor picker, for when the floor read off the game's
    // layer buttons is wrong. A chip left of the game's buttons shows the
    // floor as a small row of boxes, lit like the buttons, and who chose it.
    // A click opens a column of boxes laid out like the game's buttons, and
    // Auto. A pick holds until the map closes, Auto, or a click on the game's
    // own buttons (LayerChoice).
    static bool pickerOpen = false;
    static double pickerLeftAt = 0.0;
    // how long the cursor can be off the open list before it closes
    static constexpr double PICKER_LEAVE = 0.4;
    // the game's map buttons in the Game look's dark colours, the same in every look
    static const ImU32 PICKER_TOP = IM_COL32(232, 232, 232, 255);
    static const ImU32 PICKER_UNDER = IM_COL32(140, 146, 158, 255);
    static const ImU32 PICKER_TOP_LIT = IM_COL32(110, 230, 250, 255);
    static const ImU32 PICKER_UNDER_LIT = IM_COL32(30, 160, 210, 255);

    // The open list closes once the cursor has been off it PICKER_LEAVE.
    // Not on screen counts as off.
    static void pickerCursor(bool onIt) {
        double now = glfwGetTime();
        if (!pickerOpen || onIt) pickerLeftAt = now;
        else if (now - pickerLeftAt > PICKER_LEAVE) pickerOpen = false;
    }

    static void pickerDiamond(ImDrawList* draw, ImVec2 center, float w, float h, ImU32 colour) {
        draw->AddQuadFilled(ImVec2(center.x, center.y - h), ImVec2(center.x + w, center.y),
            ImVec2(center.x, center.y + h), ImVec2(center.x - w, center.y), colour);
    }

    // a round button like the game's layer buttons: a thin ring around two
    // stacked diamonds; the lit one has no ring, its diamonds cyan with a glow
    // the game's above ground icon: two peaks, w and h from its middle
    static void pickerMountain(ImDrawList* draw, ImVec2 c, float w, float h, ImU32 colour) {
        draw->AddTriangleFilled(ImVec2(c.x - w, c.y + h), ImVec2(c.x - w * 0.3f, c.y - h * 0.6f), ImVec2(c.x + w * 0.4f, c.y + h), colour);
        draw->AddTriangleFilled(ImVec2(c.x - w * 0.1f, c.y + h), ImVec2(c.x + w * 0.4f, c.y - h), ImVec2(c.x + w, c.y + h), colour);
    }

    static void pickerButton(ImDrawList* draw, ImVec2 center, float radius, bool lit, bool hovered) {
        float w = radius * 0.5f, h = radius * 0.3f;
        ImVec2 top(center.x, center.y - h * 0.3f), under(center.x, center.y + h * 0.45f);
        if (lit) {
            for (int i = 4; i >= 1; i--) draw->AddCircleFilled(center, radius * (0.4f + 0.15f * i), IM_COL32(70, 216, 240, 14));
            pickerDiamond(draw, under, w, h, PICKER_UNDER_LIT);
            pickerDiamond(draw, top, w, h, PICKER_TOP_LIT);
            return;
        }
        if (hovered) draw->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, 24));
        draw->AddCircle(center, radius - 1.0f, IM_COL32(255, 255, 255, 50), 0, (std::max)(1.0f, radius * 0.06f));
        pickerDiamond(draw, under, w, h, PICKER_UNDER);
        pickerDiamond(draw, top, w, h, PICKER_TOP);
    }

    // a pill's outline in dashes, for a floor the overlay only guessed
    static void dashedPill(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 colour, float thickness, float dash) {
        draw->PathRect(min, max, (max.y - min.y) / 2);
        std::vector<ImVec2> points(draw->_Path.Data, draw->_Path.Data + draw->_Path.Size);
        draw->PathClear();
        if (points.size() < 2) return;
        points.push_back(points[0]);
        // walk the outline, drawing every other dash's length of it
        bool on = true;
        float left = dash;
        std::vector<ImVec2> piece = { points[0] };
        for (size_t i = 1; i < points.size(); i++) {
            ImVec2 from = points[i - 1], to = points[i];
            float length = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
            float done = 0;
            while (length - done > left) {
                done += left;
                ImVec2 at(from.x + (to.x - from.x) * done / length, from.y + (to.y - from.y) * done / length);
                if (on) {
                    piece.push_back(at);
                    draw->AddPolyline(piece.data(), (int)piece.size(), colour, ImDrawFlags_None, thickness);
                }
                piece.assign(1, at);
                on = !on;
                left = dash;
            }
            left -= length - done;
            if (on) piece.push_back(to);
            else piece.assign(1, to);
        }
        if (on && piece.size() > 1) draw->AddPolyline(piece.data(), (int)piece.size(), colour, ImDrawFlags_None, thickness);
    }

    static void pickerArrow(ImDrawList* draw, float x, float y, float half, bool up, ImU32 colour) {
        float tip = up ? -half / 2 : half / 2;
        draw->AddTriangleFilled(ImVec2(x - half, y - tip), ImVec2(x + half, y - tip), ImVec2(x, y + tip), colour);
    }

    static void DrawFloorPicker(const MapTracking::MapView& view) {
        pickerBox = {};
        if (!view.visible || view.pickerGroup == 0 || view.pickerFloors.empty()) {
            pickerCursor(false);
            return;
        }
        int windowX = 0, windowY = 0;
        glfwGetWindowPos(Overlay::Window, &windowX, &windowY);
        RECT game = view.gameRect;
        OffsetRect(&game, -windowX, -windowY);
        POINT beside;
        if (!GameUi::BesideSwitcher(game, view.panel, beside)) {
            pickerCursor(false);
            return;
        }

        // the buttons from the top: the area's floors with above ground at pickerGround
        int count = 1 + (int)view.pickerFloors.size();
        auto floorAt = [&](int button) { return button < view.pickerGround ? button : button - 1; };
        int lit = -1;
        if (view.noMapFloor) lit = -1;
        else if (view.result.groupId == 0) lit = view.pickerGround;
        else if (view.result.groupId == view.pickerGroup) {
            for (int i = 0; i < count; i++) {
                if (i != view.pickerGround && view.pickerFloors[floorAt(i)] == view.result.floorId) lit = i;
            }
        }
        bool picked = view.layerFrom == LayerChoice::From::Picked;
        Controls::Theme gameLook = Controls::GetTheme(Controls::Look::Game, true);
        ImU32 cream = Controls::Color(gameLook.text);
        ImU32 gold = Controls::Color(gameLook.gold);

        // A rounded dark column just left of the game's buttons, the chip
        // half way down them. The list opens downwards from it.
        float font = ImGui::GetFontSize();
        float pad = font * 0.45f;
        float chipHeight = font * 1.55f;
        float radius = font * 1.15f;
        // The floor: a mountain above ground, else its short name. Who said
        // it (the game's buttons, a pick, or the overlay's guess) and its
        // whole name on the tooltip.
        bool guessed = view.layerFrom == LayerChoice::From::Guess;
        const char* who = picked ? "Picked" : guessed ? "Guess" : "Auto";
        ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.9f);
        float small = ImGui::GetFontSize();
        float lineHeight = ImGui::GetTextLineHeight();
        float mountainHalf = small * 0.6f;
        float floorWidth = view.aboveGround ? mountainHalf * 2 : ImGui::CalcTextSize(view.floorShort.c_str()).x;
        float autoText = ImGui::CalcTextSize("Auto").x;
        ImGui::PopFont();
        float chipWidth = floorWidth + small * 2.4f;
        float width = (std::max)(chipWidth, radius * 2 + pad);

        ImGui::SetNextWindowPos(ImVec2((float)beside.x, beside.y - chipHeight / 2 - pad), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, chipHeight / 2 + pad);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, font * 0.7f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Controls::ColorVec(Controls::Faded(gameLook.panel | 0xFF, 0.82f)));
        ImGui::Begin("##floorPicker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float left = ImGui::GetCursorPosX();
        auto centred = [&](float itemWidth) { ImGui::SetCursorPosX(left + (width - itemWidth) / 2); };

        // the chip: who chose the floor, an arrow
        centred(chipWidth);
        if (ImGui::InvisibleButton("##chip", ImVec2(chipWidth, chipHeight))) pickerOpen = !pickerOpen;
        // with the floors open their own hover text names them
        if (!pickerOpen) Controls::Tooltip((std::string(who) + ": " + view.floorName).c_str(), Controls::TipAt::Under);
        ImVec2 chip = ImGui::GetItemRectMin(), chipEnd = ImGui::GetItemRectMax();
        draw->AddRectFilled(chip, chipEnd, IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 36 : 20), chipHeight / 2);
        if (guessed) {
            float line = (std::max)(1.0f, font * 0.1f);
            ImVec2 in(line / 2, line / 2);
            dashedPill(draw, ImVec2(chip.x + in.x, chip.y + in.y), ImVec2(chipEnd.x - in.x, chipEnd.y - in.y), gold, line, font * 0.3f);
        }
        float middleY = chip.y + chipHeight / 2;
        ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.9f);
        float textX = chip.x + small * 0.7f;
        if (view.aboveGround) pickerMountain(draw, ImVec2(textX + mountainHalf, middleY), mountainHalf, small * 0.38f, cream);
        else draw->AddText(ImVec2(textX, middleY - lineHeight / 2), cream, view.floorShort.c_str());
        ImGui::PopFont();
        pickerArrow(draw, chip.x + chipWidth - small * 0.75f, middleY, small * 0.22f, pickerOpen, cream);

        if (pickerOpen) {
            // Auto: back to what the game's buttons say, ticked while it's on
            ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.9f);
            float tick = small * 0.75f;
            float autoWidth = autoText + (picked ? 0 : small * 0.35f + tick);
            centred(autoWidth);
            if (ImGui::InvisibleButton("##auto", ImVec2(autoWidth, ImGui::GetTextLineHeight()))) {
                MapTracking::PickAuto(view.opening);
                pickerOpen = false;
            }
            ImU32 autoColour = ImGui::IsItemHovered() ? cream : gold;
            ImVec2 at = ImGui::GetItemRectMin();
            draw->AddText(at, autoColour, "Auto");
            if (!picked) {
                float x = at.x + autoText + small * 0.35f, y = at.y + ImGui::GetTextLineHeight() / 2;
                ImVec2 points[3] = { ImVec2(x, y), ImVec2(x + tick * 0.35f, y + tick * 0.32f), ImVec2(x + tick, y - tick * 0.38f) };
                draw->AddPolyline(points, 3, autoColour, ImDrawFlags_None, (std::max)(1.0f, small * 0.1f));
            }
            ImGui::PopFont();
            // the buttons as the game lays them out, the floor shown lit
            for (int i = 0; i < count && pickerOpen; i++) {
                ImGui::PushID(i);
                centred(radius * 2);
                bool clicked = ImGui::InvisibleButton("##floor", ImVec2(radius * 2, radius * 2));
                if (i < (int)view.pickerNames.size()) Controls::Tooltip(view.pickerNames[i].c_str(), Controls::TipAt::Left);
                ImVec2 min = ImGui::GetItemRectMin();
                pickerButton(draw, ImVec2(min.x + radius, min.y + radius), radius, i == lit, ImGui::IsItemHovered());
                if (clicked) {
                    LayerChoice::Layer layer = { view.result.mapId, 0, 0 };
                    if (i != view.pickerGround) layer = { view.result.mapId, view.pickerGroup, view.pickerFloors[floorAt(i)] };
                    MapTracking::PickLayer(view.opening, layer);
                    pickerOpen = false;
                }
                ImGui::PopID();
            }
        }
        pickerCursor(ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));

        ImVec2 min = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        pickerBox = { (LONG)min.x, (LONG)min.y, (LONG)(min.x + size.x), (LONG)(min.y + size.y) };
        ClaimMouse(min.x, min.y, min.x + size.x, min.y + size.y);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
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
        ImGui::Text("Pocket Esoptron is running. Press %s for the menu.", KeyChoices::menuKeyName().c_str());
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // An error notice, shown over the game with the map closed too, once
    // each time the status turns to a relog.
    static std::string trafficLastRelog;

    static void updateTrafficNote() {
        GameTraffic::Status now = GameTraffic::GetStatus();
        std::string relog = GameTrafficText::NeedsRelog(now) ? GameTraffic::ReasonCode(now) : "";
        if (!relog.empty() && relog != trafficLastRelog) {
            Notices::Add("", "Pocket Esoptron", Notices::Kind::Error, GameTrafficText::For(now).text, glfwGetTime());
        }
        trafficLastRelog = relog;
    }

    static void DrawNpcapButton() {
        switch (NpcapDownload::GetState()) {
        case NpcapDownload::State::Downloading: {
            Controls::ButtonOptions busy;
            busy.busy = true;
            busy.busyLabel = "Downloading Npcap...";
            Controls::Button("Download Npcap", busy);
            return;
        }
        case NpcapDownload::State::Opened:
            ImGui::TextWrapped("Npcap's installer is open. Game traffic starts by itself once it's installed.");
            // for when the installer got closed
            if (Controls::Button("Open the installer again"))
                NpcapDownload::Start();
            return;
        case NpcapDownload::State::Failed:
            ImGui::TextWrapped("Couldn't get Npcap. Check your internet connection, or get it from npcap.com.");
            break;
        case NpcapDownload::State::Idle:
            break;
        }
        if (Controls::Button("Download Npcap"))
            NpcapDownload::Start();
    }

    static void DrawMapSection() {
        MenuSummary::MapText map = MenuSummary::Map(mapState());
        ItemFiles::Status itemFiles = ItemFiles::GetStatus();
        MenuSummary::ItemsText items = MenuSummary::Items(itemFiles);
        if (!Controls::BeginSection("Map", true, false, MenuSummary::MapSection(map, items))) return;
        DrawMapStatus(map);
        DrawItemsStatus(itemFiles, items);
        if (Controls::Toggle("Hide on map close", &Settings::hideOnMapClose)) {
            Settings::Save();
        }
        Controls::EndSection();
    }

    static Controls::Summary trafficSummary(const GameTrafficText::Line& line) {
        return { line.text, line.problem ? Controls::Summary::Kind::Strong : Controls::Summary::Kind::Plain };
    }

    static Controls::Summary appearanceSummary() {
        return { MenuSummary::Join(MenuSummary::Join(Settings::look, Settings::lightDark), std::to_string(Settings::size) + "%") };
    }

    static void DrawGameTraffic(const GameTrafficText::Line& line) {
        bool on = Settings::readGameTraffic;
        if (Controls::Toggle("Read the game's traffic", &on)) setGameTraffic(on);
        if (line.downloadNpcap) DrawNpcapButton();
        if (line.orAdmin) ImGui::TextWrapped("Or run the overlay as admin.");
    }

    static void DrawGameTrafficSection() {
        GameTrafficText::Line traffic = GameTrafficText::For(GameTraffic::GetStatus());
        if (!Controls::BeginSection("Game traffic", false, false, trafficSummary(traffic))) return;
        DrawGameTraffic(traffic);
        Controls::EndSection();
    }

    // the overlay's logins, and the extensions allowed to use each
    static void DrawAccounts() {
        Controls::PageTitle("Accounts");
        for (Accounts::Site site : { Accounts::Site::HoyolabMap, Accounts::Site::Appsample }) {
            ImGui::PushID((int)site);
            Accounts::Status status = Accounts::Get(site);
            if (Controls::BeginSection(Accounts::SiteLabel(site), true, false, MenuSummary::Account(site, status))) {
                if (site == Accounts::Site::HoyolabMap && status.state == Accounts::State::LoggedIn && !status.name.empty())
                    ImGui::TextDisabled("Account %s", status.account.c_str());
                if (!status.problem.empty()) ImGui::TextWrapped("%s", status.problem.c_str());

                Controls::ButtonOptions main;
                main.look = Controls::ButtonOptions::Look::Main;
                // one window at a time
                std::optional<Accounts::Site> windowSite = Accounts::WindowSite();
                bool otherWindow = windowSite && *windowSite != site;
                if (otherWindow) {
                    std::string open = std::string("The ") + Accounts::SiteLabel(*windowSite) + " window is open.";
                    ImGui::TextDisabled("%s", open.c_str());
                }
                Controls::ButtonOptions logIn = main;
                logIn.busy = otherWindow;
                switch (status.state) {
                case Accounts::State::LoggedOut:
                    logIn.busyLabel = "Log in";
                    if (Controls::Button("Log in", logIn)) Accounts::LogIn(site);
                    break;
                case Accounts::State::WindowOpen:
                    if (Controls::Button("Cancel")) Accounts::CloseWindow(false);
                    break;
                case Accounts::State::LoggedIn:
                    if (Controls::Button("Log out")) Accounts::LogOut(site);
                    break;
                case Accounts::State::NeedsLogin:
                    logIn.busyLabel = "Log in again";
                    if (Controls::Button("Log in again", logIn)) Accounts::LogIn(site);
                    break;
                case Accounts::State::Captcha:
                    logIn.busyLabel = "Open HoYoLAB Map";
                    if (Controls::Button("Open HoYoLAB Map", logIn)) Accounts::OpenHoyolabMap();
                    ImGui::SameLine();
                    if (Controls::Button("Log out")) Accounts::LogOut(site);
                    break;
                }

                Extensions::ExtensionPermissionTypes permission = site == Accounts::Site::HoyolabMap
                    ? Extensions::ExtensionPermissionTypes::HoyolabMapAccount : Extensions::ExtensionPermissionTypes::AppsampleAccount;
                std::string users;
                for (const auto& ext : Extensions::registeredExtensions) {
                    if (!ext->extensionPermissions.count(permission)) continue;
                    users += (users.empty() ? "" : ", ") + ext->name;
                }
                if (users.empty()) ImGui::TextDisabled("No extension can use it.");
                else ImGui::TextWrapped("Used by: %s", users.c_str());
                Controls::EndSection();
            }
            ImGui::PopID();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Logging out keeps who's allowed. Logging in as another account switches them all off again.");
        ImGui::PopStyleColor();
    }

    // an extension in the page order, by its folder
    static std::string PageKey(const Extensions::Extension& ext, size_t index) {
        if (!ext.folderKey.empty()) return ext.folderKey;
        return "#" + std::to_string(index);
    }

    static ImTextureID PageImage(const Extensions::Extension& ext) {
        return ext.extensionImage ? (ImTextureID)(intptr_t)ext.extensionImage : 0;
    }

    static void OpenFolder(const std::filesystem::path& folder) {
        ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Undo and redo of the page list's moves, asked by the arrows on the
    // notice a move shows and done at the next frame's page list.
    static PageOrder::History pageHistory;
    static int pageNotice = 0;
    enum class PageStep { None, Undo, Redo };
    static PageStep pageStepAsked = PageStep::None;

    // one notice for every step, its text the last one
    static void PageNotice(const std::string& text) {
        double now = glfwGetTime();
        if (Notices::Update(pageNotice, text, now)) return;
        pageNotice = Notices::Add("", "Pocket Esoptron", Notices::Kind::Plain, text, now);
        Notices::SetIcons(pageNotice, {
            { Controls::ButtonIcon::Undo, "Undo", [] { return pageHistory.CanUndo(); }, [] { pageStepAsked = PageStep::Undo; } },
            { Controls::ButtonIcon::Redo, "Redo", [] { return pageHistory.CanRedo(); }, [] { pageStepAsked = PageStep::Redo; } },
        }, false);
    }

    static void ChangedPageList(const PageOrder::List& before, const std::string& text) {
        pageHistory.Did(before, text);
        Settings::SavePageList(PageOrder::ToJson(pageList));
        PageNotice(text);
    }

    static std::string PageName(const std::string& key) {
        for (size_t i = 0; i < Extensions::registeredExtensions.size(); ++i) {
            if (PageKey(*Extensions::registeredExtensions[i], i) == key) return Extensions::registeredExtensions[i]->name;
        }
        return key;
    }

    // A move in the page list, saved when something moved. The
    // folder an extension goes into opens.
    static void MovePage(const std::function<PageOrder::Place(PageOrder::List&)>& move) {
        PageOrder::List before = pageList;
        PageOrder::Place moved = move(pageList);
        if (moved.top < 0) return;
        const PageOrder::Item& item = pageList[moved.top];
        std::string name = moved.inside >= 0 ? PageName(item.inside[moved.inside]) : item.folder ? item.key : PageName(item.key);
        if (moved.inside >= 0) pageList[moved.top].open = true;
        ChangedPageList(before, "Moved " + name);
    }

    // a row of the page list is being dragged, its click doesn't count
    static bool pageDragging = false;

    struct PageDrop {
        bool set = false;
        bool onto = false;
        PageOrder::Place from, to;
    };

    // Drag and drop on the row drawn last. The top and bottom quarters drop
    // before or after it, the middle of a folder or of an extension outside
    // folders drops onto it. A folder dropped on a folder's extension goes
    // after that folder.
    static void PageDragDrop(PageOrder::Place at, const char* name, PageDrop& drop) {
        using PageOrder::Place;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            pageDragging = true;
            ImGui::SetDragDropPayload("page place", &at, sizeof at);
            ImGui::TextUnformatted(name);
            ImGui::EndDragDropSource();
        }
        if (!ImGui::BeginDragDropTarget()) return;
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("page place",
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (payload) {
            Place from = *static_cast<const Place*>(payload->Data);
            const PageOrder::Item& target = pageList[at.top];
            bool movingFolder = from.inside < 0 && pageList[from.top].folder;
            bool folderRow = target.folder && at.inside < 0;
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            float y = ImGui::GetMousePos().y, quarter = (max.y - min.y) / 4;
            bool canOnto = !movingFolder && at.inside < 0 && at.top != from.top;
            enum { Before, Onto, After } zone;
            if (movingFolder && at.inside >= 0) zone = After;
            else if (canOnto) zone = y < min.y + quarter ? Before : y > max.y - quarter ? After : Onto;
            else zone = y < (min.y + max.y) / 2 ? Before : After;

            Place to = at;
            if (zone == After) {
                if (movingFolder || (at.inside < 0 && !(folderRow && target.open))) to = { at.top + 1, -1 };
                else if (at.inside >= 0) to = { at.top, at.inside + 1 };
                else to = { at.top, 0 };
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImU32 accent = Controls::Color(Controls::GetTheme().accent);
            float line = std::max(2.0f, ImGui::GetFontSize() * 0.12f);
            if (zone == Onto) draw->AddRect(min, max, accent, ImGui::GetStyle().FrameRounding, 0, line);
            else {
                float edge = zone == Before ? min.y : max.y;
                draw->AddLine(ImVec2(min.x, edge), ImVec2(max.x, edge), accent, line);
            }
            if (payload->IsDelivery()) drop = { true, zone == Onto, from, to };
        }
        ImGui::EndDragDropTarget();
    }

    // a move picked in the ⋯ menu, done once the menu is drawn
    static std::function<PageOrder::Place(PageOrder::List&)> pageMoveAsked;

    static bool CanMove(const std::function<PageOrder::Place(PageOrder::List&)>& move) {
        PageOrder::List copy = pageList;
        return move(copy).top >= 0;
    }

    static void MoveMenu(PageOrder::Place at, bool folder) {
        using namespace PageOrder;
        if (!ImGui::BeginMenu("Move")) return;
        auto item = [&](const char* label, const std::function<Place(List&)>& move) {
            if (ImGui::MenuItem(label, nullptr, false, CanMove(move))) pageMoveAsked = move;
        };
        item("Up", [at](List& list) { return Up(list, at); });
        item("Down", [at](List& list) { return Down(list, at); });
        item("To the top", [at](List& list) { return ToTop(list, at); });
        item("To the bottom", [at](List& list) { return ToBottom(list, at); });
        if (!folder) {
            ImGui::Separator();
            std::vector<int> folders;
            for (int top = 0; top < (int)pageList.size(); top++) {
                if (pageList[top].folder && top != at.top) folders.push_back(top);
            }
            if (ImGui::BeginMenu("Into folder", !folders.empty())) {
                for (int top : folders) {
                    ImGui::PushID(top);
                    item(pageList[top].key.c_str(), [at, top](List& list) { return IntoFolder(list, at, top); });
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            item("New folder", [at](List& list) { return NewFolder(list, at); });
            if (at.inside >= 0) item("Out of folder", [at](List& list) { return OutOfFolder(list, at); });
        }
        ImGui::EndMenu();
    }

    // The ⋯ menu of an extension's or a folder's row in the page list
    static void PageMenu(const std::map<std::string, size_t>& byKey) {
        PageOrder::Place at = pageMenuAt;
        if (at.top < 0 || at.top >= (int)pageList.size()) {
            ImGui::CloseCurrentPopup();
            return;
        }
        const PageOrder::Item& item = pageList[at.top];
        if (item.folder && at.inside < 0) {
            MoveMenu(at, true);
            if (ImGui::MenuItem("Rename")) {
                renamedFolder = at.top;
                folderName = item.key;
                renameProblem.clear();
                renameAsked = true;
            }
            if (ImGui::MenuItem("Ungroup")) {
                PageOrder::List before = pageList;
                std::string name = item.key;
                if (PageOrder::Ungroup(pageList, at.top)) ChangedPageList(before, "Ungrouped " + name);
            }
            return;
        }
        const std::string& key = at.inside >= 0 ? item.inside[at.inside] : item.key;
        auto found = byKey.find(key);
        if (found == byKey.end()) {
            ImGui::CloseCurrentPopup();
            return;
        }
        Extensions::Extension& ext = *Extensions::registeredExtensions[found->second];
        MoveMenu(at, false);
        if (ImGui::MenuItem(ext.enabled ? "Turn off" : "Turn on")) Extensions::setRunning(ext, !ext.enabled);
        if (ImGui::MenuItem("Permissions")) Extensions::openPermissionsTab(ext);
        if (ImGui::MenuItem("Show in folder", nullptr, false, !ext.folder.empty())) OpenFolder(ext.folder);
        if (!ext.enabled || ext.stopped || !ext.stopReason.empty() || ext.menuItems.empty()) return;
        ImGui::Separator();
        // run once the menu is drawn, the function can add or remove items
        std::function<bool()> picked;
        for (const auto& menuItem : ext.menuItems) {
            ImGui::PushID(menuItem.id.c_str());
            if (ImGui::MenuItem(menuItem.label.c_str())) picked = menuItem.callback;
            ImGui::PopID();
        }
        if (picked && !picked()) {
            Notices::Add(ext.folderKey, ext.name, Notices::Kind::Error, ext.name + " couldn't do that.", glfwGetTime());
        }
    }

    // the official Map, Minimap and Markers get the overlay's own icons, any other
    // extension its icon.png or its first letter
    static Controls::PageIcon OfficialIcon(const Extensions::Extension& ext) {
        if (!ext.official) return Controls::PageIcon::None;
        if (ext.folderKey == "map") return Controls::PageIcon::Map;
        if (ext.folderKey == "minimap") return Controls::PageIcon::Minimap;
        if (ext.folderKey == "markers") return Controls::PageIcon::Markers;
        return Controls::PageIcon::None;
    }

    static void DrawMenu() {
        float font = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImVec2(font * 4, font * 4), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(font * 55, font * 40), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(font * 30, font * 20), ImVec2(FLT_MAX, FLT_MAX));

        bool open = true;

        // the list and the page reach the window's edges
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(
            "Pocket Esoptron",
            &open,
            ImGuiWindowFlags_NoCollapse
        );
        ImGui::PopStyleVar();

        Controls::Theme theme = Controls::GetTheme();
        // the player's width in font sizes once the splitter is dragged, below 0 to fit the names
        static float sidebarFonts = -1;
        const float sidebarMin = font * 9, sidebarMax = font * 18;
        float splitterWidth = std::max(2.0f, font * 0.35f);
        float listPad = font * 0.35f;

        float cpu = 0, ram = 0;
        Stats::Latest(cpu, ram);
        std::vector<std::pair<std::string, Controls::Summary>> pages = {
            { "Home", {} },
            { "Usage", { Settings::usageInList ? MenuSummary::Usage(cpu, ram) : "" } },
            { "Accounts", MenuSummary::AccountsPage(Accounts::Get(Accounts::Site::HoyolabMap), Accounts::Get(Accounts::Site::Appsample)) },
        };

        if (!pageListLoaded) {
            pageList = PageOrder::FromJson(Settings::LoadPageList());
            pageListLoaded = true;
        }
        std::vector<std::string> present;
        std::map<std::string, size_t> byKey;
        for (size_t i = 0; i < Extensions::registeredExtensions.size(); ++i) {
            std::string key = PageKey(*Extensions::registeredExtensions[i], i);
            present.push_back(key);
            byKey[key] = i;
        }
        if (pageStepAsked != PageStep::None) {
            bool undo = pageStepAsked == PageStep::Undo;
            pageStepAsked = PageStep::None;
            std::string what = undo ? pageHistory.NextUndo() : pageHistory.NextRedo();
            // folders keep being open or closed as they are now
            std::map<std::string, bool> open;
            for (const auto& item : pageList) if (item.folder) open[item.key] = item.open;
            if (undo ? pageHistory.Undo(pageList) : pageHistory.Redo(pageList)) {
                for (auto& item : pageList) {
                    if (item.folder && open.count(item.key)) item.open = open[item.key];
                }
                pageList = PageOrder::Arrange(pageList, present);
                Settings::SavePageList(PageOrder::ToJson(pageList));
                PageNotice(undo ? "Undone: " + what : what);
            }
        }
        pageList = PageOrder::Arrange(pageList, present);

        // shown only once the list is longer than the window
        static bool listScrolls = false;
        float sidebarWidth = sidebarMin;
        if (sidebarFonts > 0) sidebarWidth = sidebarFonts * font;
        else {
            for (const auto& [name, summary] : pages) {
                sidebarWidth = std::max(sidebarWidth, Controls::PageButtonWidth(name.c_str(), summary));
            }
            for (const auto& item : pageList) {
                if (!item.folder) {
                    sidebarWidth = std::max(sidebarWidth, Controls::PageButtonWidth(Extensions::registeredExtensions[byKey[item.key]]->name.c_str(), {}, true));
                    continue;
                }
                sidebarWidth = std::max(sidebarWidth, Controls::FolderButtonWidth(item.key.c_str()));
                for (const auto& key : item.inside) {
                    float width = Controls::PageButtonWidth(Extensions::registeredExtensions[byKey[key]]->name.c_str(), {}, true);
                    sidebarWidth = std::max(sidebarWidth, width + Controls::PageIndent());
                }
            }
            if (listScrolls) sidebarWidth += ImGui::GetStyle().ScrollbarSize;
            // windows are whole pixels
            sidebarWidth = std::ceil(sidebarWidth + listPad * 2);
        }
        sidebarWidth = std::clamp(sidebarWidth, sidebarMin, sidebarMax);

        ImVec2 contentSize = ImGui::GetContentRegionAvail();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(listPad, listPad));
        ImGui::BeginChild(
            "Sidebar",
            ImVec2(sidebarWidth, contentSize.y),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        );
        ImGui::PopStyleVar();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, font * 0.2f));

        // by number, so an extension named "Settings" or two with one name are their own buttons
        auto DrawPage = [&](const char* name, int index, ImTextureID icon, bool dotted, unsigned int tile,
            const Controls::Summary& summary, Controls::PageIcon glyph, bool menu = false) {
            ImGui::PushID(index);
            if (Controls::PageButton(name, activeTab == index, icon, dotted, tile, summary, glyph, menu) && !pageDragging) activeTab = index;
            ImGui::PopID();
        };

        // Apple's grey, orange and blue tiles for the overlay's own pages
        DrawPage("Home", 0, 0, false, 0x8E8E93FF, {}, Controls::PageIcon::Home);
        DrawPage("Usage", 1, 0, false, 0xFF9F0AFF, pages[1].second, Controls::PageIcon::Usage);
        DrawPage("Accounts", 2, 0, false, 0x0A84FFFF, pages[2].second, Controls::PageIcon::Accounts);

        if (!Extensions::registeredExtensions.empty()) {
            ImGui::Separator();
        }

        // only the extensions scroll, the overlay's own pages stay put
        ImGui::BeginChild("Extensions");
        listScrolls = ImGui::GetScrollMaxY() > 0;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) pageDragging = false;
        PageDrop drop;
        auto DrawExtension = [&](size_t i, PageOrder::Place at) {
            auto& ext = Extensions::registeredExtensions[i];
            DrawPage(ext->name.c_str(), static_cast<int>(i) + OwnPages, PageImage(*ext), !ext->dots.empty(), 0, {}, OfficialIcon(*ext), true);
            PageDragDrop(at, ext->name.c_str(), drop);
            if (Controls::PageMenuAsked() && !pageDragging) {
                pageMenuAt = at;
                ImGui::OpenPopup("page menu");
            }
        };
        bool listChanged = false;
        for (int top = 0; top < (int)pageList.size(); ++top) {
            PageOrder::Item& item = pageList[top];
            if (!item.folder) {
                DrawExtension(byKey[item.key], { top, -1 });
                continue;
            }
            std::vector<Controls::PageMark> marks;
            bool dotted = false, holdsOpenPage = false;
            for (const auto& key : item.inside) {
                size_t i = byKey[key];
                auto& ext = Extensions::registeredExtensions[i];
                if (marks.size() < 4) marks.push_back({ ext->name, PageImage(*ext), OfficialIcon(*ext) });
                if (!ext->dots.empty()) dotted = true;
                if (activeTab == static_cast<int>(i) + OwnPages) holdsOpenPage = true;
            }
            ImGui::PushID(("folder " + std::to_string(top)).c_str());
            if (Controls::FolderButton(item.key.c_str(), item.open, marks, dotted && !item.open, holdsOpenPage && !item.open) && !pageDragging) {
                item.open = !item.open;
                listChanged = true;
            }
            ImGui::PopID();
            PageDragDrop({ top, -1 }, item.key.c_str(), drop);
            if (Controls::PageMenuAsked() && !pageDragging) {
                pageMenuAt = { top, -1 };
                ImGui::OpenPopup("page menu");
            }
            if (!item.open) continue;
            ImGui::Indent(Controls::PageIndent());
            for (int inside = 0; inside < (int)item.inside.size(); ++inside) DrawExtension(byKey[item.inside[inside]], { top, inside });
            ImGui::Unindent(Controls::PageIndent());
        }
        // under the last row, the end of the list outside folders
        if (pageDragging && !pageList.empty()) {
            float end = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
            // the rest of the window, or a line's height once the list scrolls
            float room = ImGui::GetContentRegionAvail().y;
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, room > 0 ? room : font));
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("page place",
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                if (payload) {
                    float line = std::max(2.0f, ImGui::GetFontSize() * 0.12f);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetItemRectMin().x, end), ImVec2(ImGui::GetItemRectMax().x, end),
                        Controls::Color(Controls::GetTheme().accent), line);
                    PageOrder::Place from = *static_cast<const PageOrder::Place*>(payload->Data);
                    if (payload->IsDelivery()) drop = { true, false, from, { (int)pageList.size(), -1 } };
                }
                ImGui::EndDragDropTarget();
            }
        }
        if (listChanged) Settings::SavePageList(PageOrder::ToJson(pageList));
        if (drop.set) {
            MovePage([&](PageOrder::List& list) {
                return drop.onto ? PageOrder::Onto(list, drop.from, drop.to) : PageOrder::Move(list, drop.from, drop.to);
            });
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) pageDragging = false;

        if (ImGui::BeginPopup("page menu")) {
            PageMenu(byKey);
            ImGui::EndPopup();
        }
        if (pageMoveAsked) {
            auto move = std::move(pageMoveAsked);
            pageMoveAsked = nullptr;
            MovePage(move);
        }
        if (renameAsked) {
            ImGui::OpenPopup("rename folder");
            renameAsked = false;
        }
        // wide enough for the 40 bytes a name can have
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24, 0));
        // the box keeps the keyboard while the popup is open, and gives it back as it closes
        static bool renaming = false;
        if (ImGui::BeginPopup("rename folder")) {
            renaming = true;
            // in the box when it opens and after a refused name
            static bool typeAgain = false;
            bool appearing = ImGui::IsWindowAppearing();
            if (appearing || typeAgain) ImGui::SetKeyboardFocusHere(0);
            typeAgain = false;
            bool sent = false, escaped = false;
            Controls::TextOptions options;
            options.confirm = true;
            options.maxBytes = 40;
            options.backToGame = false;
            options.fresh = appearing;
            options.sent = &sent;
            options.escaped = &escaped;
            std::string name = folderName;
            Controls::Text("Name", name, options);
            bool close = escaped || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            if (sent) {
                using PageOrder::NameCheck;
                PageOrder::List before = pageList;
                NameCheck check = PageOrder::CheckName(pageList, renamedFolder, name);
                if (PageOrder::Rename(pageList, renamedFolder, name)) {
                    ChangedPageList(before, "Renamed " + before[renamedFolder].key + " to " + pageList[renamedFolder].key);
                    close = true;
                }
                // Enter on the name it has closes it like Esc
                else if (check == NameCheck::Same) close = true;
                else {
                    folderName = name;
                    typeAgain = true;
                    switch (check) {
                    case NameCheck::Empty: renameProblem = "The name can't be empty"; break;
                    case NameCheck::Taken: renameProblem = "A folder already has this name"; break;
                    case NameCheck::HasHashes: renameProblem = "The name can't have ##"; break;
                    case NameCheck::Fine: case NameCheck::Same: break;
                    }
                }
            }
            if (close) ImGui::CloseCurrentPopup();
            if (!renameProblem.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Controls::ColorVec(Controls::GetTheme().risky));
                ImGui::TextWrapped("%s", renameProblem.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndPopup();
        }
        else if (renaming) {
            renaming = false;
            Typing::focus.letGo();
        }
        ImGui::EndChild();

        ImGui::PopStyleVar();
        ImGui::EndChild();


        ImGui::SameLine(0.0f, 0.0f);

        ImGui::InvisibleButton(
            "##SidebarSplitter",
            ImVec2(splitterWidth, contentSize.y)
        );

        bool splitterHovered = ImGui::IsItemHovered();
        if (splitterHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0) {
            sidebarFonts = std::clamp(sidebarWidth + ImGui::GetIO().MouseDelta.x, sidebarMin, sidebarMax) / font;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // shown only while it's under the mouse or held
        if (splitterHovered || ImGui::IsItemActive()) {
            ImVec2 splitterMin = ImGui::GetItemRectMin();
            ImVec2 splitterMax = ImGui::GetItemRectMax();
            float middle = (splitterMin.x + splitterMax.x) / 2;
            drawList->AddLine(ImVec2(middle, splitterMin.y), ImVec2(middle, splitterMax.y), Controls::Color(theme.accent), std::max(1.0f, font * 0.12f));
        }

        ImGui::SameLine(0.0f, 0.0f);

        // the page on its own layer, its top left corner rounded
        ImVec2 pageMin = ImGui::GetCursorScreenPos();
        ImVec2 windowMax(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
        float rounding = ImGui::GetStyle().WindowRounding;
        drawList->AddRectFilled(pageMin, windowMax, Controls::Color(theme.page), rounding, ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomRight);
        drawList->AddRect(pageMin, ImVec2(windowMax.x + rounding, windowMax.y + rounding), Controls::Color(theme.cardLine), rounding, 1.0f, ImDrawFlags_RoundCornersTopLeft);

        // the scrollbar keeps its room, so nothing moves when the page grows,
        // and shows only once there's something to scroll
        static bool pageScrolls = false;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 1.6f, font * 1.1f));
        if (!pageScrolls) ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild(
            "MainContent",
            ImVec2(0, contentSize.y),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_AlwaysVerticalScrollbar
        );
        if (!pageScrolls) ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        pageScrolls = ImGui::GetScrollMaxY() > 0;

        switch (activeTab) {
        case 0:

            {
                // Shutdown at the end of the title, or beside the title and the
                // line under it when it's taller than the title
                ImVec2 top = ImGui::GetCursorPos();
                Controls::PageTitle("Home");
                float titleHeight = ImGui::GetCursorPosY() - top.y;
                ImGui::TextDisabled("Press %s to hide", KeyChoices::menuKeyName().c_str());
                ImVec2 below = ImGui::GetCursorPos();
                ImVec2 quit = Controls::ButtonSize("Shutdown");
                float quitHeight = quit.y;
                float block = quitHeight > titleHeight - ImGui::GetStyle().ItemSpacing.y ? below.y - top.y - ImGui::GetStyle().ItemSpacing.y : titleHeight - ImGui::GetStyle().ItemSpacing.y;
                ImGui::SetCursorPos(ImVec2(std::max(top.x, ImGui::GetWindowContentRegionMax().x - quit.x), top.y + (block - quitHeight) / 2));
                if (Controls::Button("Shutdown")) glfwSetWindowShouldClose(Overlay::Window, true);
                ImGui::SetCursorPos(below);
            }
            ImGui::Dummy(ImVec2(0, font * 0.4f));

            DrawMapSection();

            DrawGameTrafficSection();

            if (Controls::BeginSection("Appearance", false, false, appearanceSummary())) {
                int look = indexOf(lookNames, Settings::look);
                if (Controls::Choice("Theme", look, lookNames)) {
                    Settings::look = lookNames[look];
                    Settings::Save();
                    lookPicked = true;
                }
                int lightDark = indexOf(lightDarkNames, Settings::lightDark);
                if (Controls::Choice("Light or dark", lightDark, lightDarkNames)) {
                    Settings::lightDark = lightDarkNames[lightDark];
                    Settings::Save();
                    lookPicked = true;
                }
                static std::vector<std::string> sizeNames;
                if (sizeNames.empty())
                    for (int size : Settings::Sizes) sizeNames.push_back(std::to_string(size) + "%");
                int size = (int)(std::find(Settings::Sizes.begin(), Settings::Sizes.end(), Settings::size) - Settings::Sizes.begin());
                if (Controls::Choice("Size", size, sizeNames)) {
                    Settings::size = Settings::Sizes[size];
                    Settings::Save();
                    lookPicked = true;
                }
                Controls::EndSection();
            }

            if (Controls::BeginSection("Keys", false, false, { "Menu: " + KeyChoices::menuKeyName() })) {
                KeyChoices::drawMenuKeyRow();
                Controls::EndSection();
            }

            if (Controls::BeginSection("Advanced")) {
                Controls::ToggleOptions takenKeys;
                takenKeys.lines = { { "Lets Record catch a key another program has taken. Some antivirus programs dislike this." } };
                if (Controls::Toggle("Record taken keys", &Settings::recordTakenKeys, takenKeys)) {
                    Settings::Save();
                }

                ImGui::TextDisabled("How the map is followed while it moves");

                if (Controls::Toggle(
                    "Line up with the map tiles",
                    &Settings::lineUpWithMap
                )) {
                    Settings::Save();
                }

                if (Controls::Toggle(
                    "Ignore open water",
                    &Settings::ignoreSea
                )) {
                    Settings::Save();
                }

                if (Controls::Toggle(
                    "Draw ahead of the map while it moves",
                    &Settings::guessAhead
                )) {
                    Settings::Save();
                }

                if (Controls::Toggle(
                    "Don't draw ahead while zooming",
                    &Settings::steadyZoom
                )) {
                    Settings::Save();
                }

                if (Controls::Toggle(
                    "Look again when a still map changes (floors)",
                    &Settings::lookAgainStill
                )) {
                    Settings::Save();
                }

                if (Controls::Toggle(
                    "Follow on its own thread",
                    &Settings::followOnThread
                )) {
                    Settings::Save();
                }

                if (Controls::Toggle(
                    "Shrink the patches on the graphics card",
                    &Settings::shrinkOnGpu
                )) {
                    Settings::Save();
                }

                if (Controls::Toggle(
                    "Find the map on a smaller picture first",
                    &Settings::findSmallFirst
                )) {
                    Settings::Save();
                }
                Controls::EndSection();
            }

            ImGui::TextDisabled("Version %s", Helpers::appVersion().c_str());

            break;

        case 1:
            Controls::PageTitle("Usage");
            if (Controls::Toggle("Show CPU and RAM in the page list", &Settings::usageInList)) Settings::Save();
            Stats::Draw();

            break;

        case 2:
            DrawAccounts();

            break;

        default:
            Extensions::drawExtensionMenus(
                activeTab - OwnPages
            );

            break;
        }

        ImGui::EndChild();

        ImVec2 menuPos = ImGui::GetWindowPos();
        ImVec2 menuSize = ImGui::GetWindowSize();
        ImGui::End();
        Notices::DrawInMenu(glfwGetTime(), menuPos, menuSize);

        if (!open) {
            Overlay::SetMenuOpen(false);
        }
    }

    void MainLoop() {
        bool drewLastFrame = true;
        while (!glfwWindowShouldClose(Overlay::Window)) {
            Extensions::countTurn();
            glfwPollEvents();
            Keybindings::ProcessKeybindings();
            keepOnTop(false);
            // sends the tracker a new screenshot when it's ready for one
            MapTracking::Tick(glfwGetWin32Window(Overlay::Window));
            Stats::Update();
            // before the frame is skipped, so the map and menu closing reach the Screens trigger
            updateLook(lookPicked);
            lookPicked = false;
            updateFont();
            ScreenPictures::nextTurn();
            Accounts::Frame();
            Triggers::checkNow();
            LuaFunctions::UpdateAnswers();
            updateTrafficNote();

            // map and menu closed: nothing to show, so don't draw at all
            bool showing = menuOpen || MapTracking::GetView().visible || glfwGetTime() - startTime < 6.0 ||
                Notices::OverGameShowing(glfwGetTime());
            // an extension that follows the player has things to draw with the map closed too
            bool forPlayer = !showing && MiniMap::Wanted();
            if (!showing && !forPlayer) {
                pickerBox = {};
                updateClickThrough(false);
                updateTyping(false);
                if (drewLastFrame) {
                    // an empty frame so the last one doesn't stay on screen
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glfwSwapBuffers(Overlay::Window);
                    drewLastFrame = false;
                }
                // The map is open but hidden or not found yet: nothing to draw,
                // but the tracker still has to keep up with the game's frames.
                if (MapTracking::IsMapOpen()) DwmFlush();
                else glfwWaitEventsTimeout(0.05);
                continue;
            }
            drewLastFrame = true;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            feedMousePosition();
            ImGui::NewFrame();
            // the picker first: extensions keep off this frame's box, and the
            // click-through goes by where it is now
            DrawFloorPicker(MapTracking::GetView());
            updateClickThrough(true);

            // extension folders added while it runs
            static double lookedForExtensions = glfwGetTime();
            if (glfwGetTime() - lookedForExtensions >= 2.0) {
                lookedForExtensions = glfwGetTime();
                Extensions::findNewExtensions(Helpers::exeDirectory() / "extensions", std::chrono::seconds(2));
            }
            Extensions::frameUpdateExtensions();
            if (Settings::showClickBoxes) drawClickBoxes();
            DrawStartupHint();

            // without the game there's no map to follow, the menu stays as it is
            if (Capture::findRunningGame()) MenuFollowsMap(MapTracking::IsMapOpen());

            if (menuOpen) {
                DrawMenu();
            }
            else Notices::DrawOverGame(glfwGetTime());
            Asks::draw(MapTracking::IsMapOpen(), menuOpen);
            updateTyping(true);

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
            // the player's spot changes 20 times a second
            if (forPlayer) glfwWaitEventsTimeout(0.05);
            else MapTracking::WaitForDraw();
            MapTiles::EndFrame();
        }
    }
}
