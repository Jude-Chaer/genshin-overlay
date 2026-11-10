#include <windows.h>
#include <dwmapi.h>
#include "overlay.hpp"
#include "keybindings.hpp"
#include "extensions.hpp"
#include "helpers.hpp"
#include "item_files.hpp"
#include "map_tiles.hpp"
#include "map_tracking.hpp"
#include "game_ui.hpp"
#include "minimap.hpp"
#include "stats.hpp"
#include "settings.hpp"
#include "capture.hpp"
#include <imgui_internal.h>
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
    // where the floor picker is drawn this frame, overlay pixels, empty with none
    static RECT pickerBox = {};
    static bool cursorOnPicker() {
        ImVec2 at = Overlay::IO->MousePos;
        return at.x >= pickerBox.left && at.x < pickerBox.right && at.y >= pickerBox.top && at.y < pickerBox.bottom;
    }

    bool PickerBox(RECT& out) {
        if (IsRectEmpty(&pickerBox)) return false;
        out = pickerBox;
        return true;
    }

    // The picker takes the mouse with the menu closed too, only while the
    // cursor is over it. Windows only sees the change once the frame is
    // done, so a click right as the cursor gets there can still go to the game.
    static void updateClickThrough() {
        bool capture = (Overlay::menuOpen && Overlay::IO->WantCaptureMouse) || cursorOnPicker();
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
            if (MapTracking::IsMapOpen())
                ImGui::Text("Map is open, looking for where it is...");
            else
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

    // the map items' line when something's off, and the player's controls under it
    static void DrawItemsStatus() {
        static_assert(ItemFiles::RETRY == std::chrono::seconds(60), "the line says every minute");
        ItemFiles::Status items = ItemFiles::GetStatus();
        // the first check of the release takes a moment, the line waits for it
        if (items.started) {
            using ItemFiles::Reason;
            using ItemFiles::State;
            bool tryAgain = false;
            std::string line;
            if (items.state == State::Downloading) {
                ImGui::Text("Map items: downloading %d%%", items.percent);
                ImGui::ProgressBar(items.percent / 100.0f);
            }
            else if (items.reason == Reason::NoNetwork) {
                line = items.state == State::Old ? "Map items: couldn't update, using the list from " + items.built + ", trying again every minute"
                    : "Map items: not downloaded yet, trying again every minute (no internet?)";
            }
            else if (items.reason == Reason::NewerFormat) {
                line = "Map items: update the overlay to get the new list";
                if (items.state == State::Old) line += ", using the list from " + items.built;
            }
            else if (items.reason == Reason::BrokenPack) {
                line = "Map items: the download was broken";
                if (items.state == State::Old) line += ", using the list from " + items.built;
                tryAgain = true;
            }
            else if (items.reason == Reason::CantSave) {
                line = "Map items: can't save to the overlay folder, check it isn't read-only";
                tryAgain = true;
            }
            else if (items.reason == Reason::DownloadOff && items.state == State::None) {
                line = "Map items: none, the download is off";
            }
            if (!line.empty()) ImGui::TextWrapped("%s", line.c_str());
            if (tryAgain) {
                ImGui::BeginDisabled(items.checking);
                if (ImGui::Button("Try again")) ItemFiles::CheckNow();
                ImGui::EndDisabled();
            }
        }

        if (ImGui::Checkbox("Download map items", &Settings::downloadItems)) {
            Settings::Save();
            ItemFiles::SetDownload(Settings::downloadItems);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(items.checking || !Settings::downloadItems);
        if (ImGui::Button(items.checking ? "Checking..." : "Check for new map items")) ItemFiles::CheckNow();
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
        ImU32 cream = IM_COL32(236, 229, 216, 255);
        ImU32 gold = IM_COL32(211, 188, 142, 255);

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
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(28 / 255.0f, 32 / 255.0f, 48 / 255.0f, 0.82f));
        ImGui::Begin("##floorPicker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float left = ImGui::GetCursorPosX();
        auto centred = [&](float itemWidth) { ImGui::SetCursorPosX(left + (width - itemWidth) / 2); };

        // the chip: who chose the floor, an arrow
        centred(chipWidth);
        if (ImGui::InvisibleButton("##chip", ImVec2(chipWidth, chipHeight))) pickerOpen = !pickerOpen;
        // with the floors open their own hover text names them
        if (!pickerOpen && ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", who, view.floorName.c_str());
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
                if (i < (int)view.pickerNames.size() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", view.pickerNames[i].c_str());
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
                DrawItemsStatus();

                if (ImGui::Checkbox("Hide on map close", &Settings::hideOnMapClose)) {
                    Settings::Save();
                }
            }

            if (ImGui::CollapsingHeader("Advanced")) {
                ImGui::TextDisabled("How the map is followed while it moves");

                if (ImGui::Checkbox(
                    "Line up with the map tiles",
                    &Settings::lineUpWithMap
                )) {
                    Settings::Save();
                }

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

                if (ImGui::Checkbox(
                    "Find the map on a smaller picture first",
                    &Settings::findSmallFirst
                )) {
                    Settings::Save();
                }
            }

            break;

        case 1:
            ImGui::Text("Settings");
            ImGui::Separator();
            Extensions::drawExtensionPermissionMenu();
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
            // an extension that follows the player has things to draw with the map closed too
            bool forPlayer = !showing && MiniMap::Wanted();
            if (!showing && !forPlayer) {
                pickerBox = {};
                updateClickThrough();
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
            updateClickThrough();

            Extensions::frameUpdateExtensions();
            DrawStartupHint();

            if (Settings::hideOnMapClose) {
                Overlay::menuOpen = MapTracking::IsMapOpen();
            }

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
            // the player's spot changes 20 times a second
            if (forPlayer) glfwWaitEventsTimeout(0.05);
            else MapTracking::WaitForDraw();
            MapTiles::EndFrame();
        }
    }
}
