#include "controls.hpp"
#include "typing.hpp"
#include "cpp/dynamiccolor/material_dynamic_colors.h"
#include "cpp/scheme/scheme_tonal_spot.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace Controls {
    static Look playerLook = Look::Windows;
    static bool playerDark = true;
    static std::optional<std::array<unsigned int, 7>> accent;
    // the look's heavier font: Apple's bold, Material's medium
    static ImFont* titleFont = nullptr;

    // Material 3 gives its sizes in dp, its body text 14 of them, so one dp is
    // the screen's scale whatever font size is pushed
    static float dp() {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        return g.FontSizeBase > 0 ? g.FontSize / g.FontSizeBase : 1.0f;
    }
    // a size of Material's type scale: 24 page titles, 16 names and section
    // heads, 14 the rest; medium for labels, tabs and heads
    static void pushType(float size, bool medium) {
        ImGui::PushFont(medium && titleFont ? titleFont : nullptr, size);
    }
    static bool isMaterial() {
        return playerLook == Look::Material;
    }
    // a setting's name, Material's 16 (Body Large) while it's on
    struct NameType {
        bool pushed = isMaterial();
        NameType() { if (pushed) pushType(16, false); }
        ~NameType() { if (pushed) ImGui::PopFont(); }
    };
    // a button's or tab's label, Material's 14 medium (Label Large)
    struct LabelType {
        bool pushed = isMaterial();
        LabelType() { if (pushed) pushType(14, true); }
        ~LabelType() { if (pushed) ImGui::PopFont(); }
    };
    // Material's colour roles the Theme has no field for
    struct MaterialRoles { unsigned int onPrimaryContainer = 0, secondaryContainer = 0, onSurfaceVariant = 0; };
    static MaterialRoles materialRoles[2];

    const char* LookName(Look look) {
        switch (look) {
        case Look::Material: return "Material You";
        case Look::Game: return "Game";
        case Look::Apple: return "Apple";
        default: return "Windows 11";
        }
    }

    std::optional<Look> LookNamed(const std::string& name) {
        for (Look look : { Look::Windows, Look::Material, Look::Game, Look::Apple }) {
            if (name == LookName(look)) return look;
        }
        return std::nullopt;
    }

    // a over b at amount, both 0xRRGGBBAA
    static unsigned int blend(unsigned int a, unsigned int b, float amount) {
        unsigned int out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            float x = (float)((a >> shift) & 0xFF), y = (float)((b >> shift) & 0xFF);
            out |= (unsigned int)std::lround(y + (x - y) * amount) << shift;
        }
        return out;
    }

    // Material You's colours, made by Material's own code from the accent the
    // way Android makes them from the wallpaper (the tonal spot scheme)
    static Theme materialColours(bool dark) {
        namespace mcu = material_color_utilities;
        using Roles = mcu::MaterialDynamicColors;
        // Windows' default blue without an accent
        unsigned int seed = accent ? (*accent)[3] : 0x0078D4FF;
        mcu::SchemeTonalSpot scheme(mcu::Hct((mcu::Argb)(0xFF000000u | (seed >> 8))), dark, 0.0);
        auto role = [&](mcu::DynamicColor color) { return ((color.GetArgb(scheme) & 0xFFFFFFu) << 8) | 0xFF; };
        Theme theme;
        theme.look = Look::Material;
        theme.dark = dark;
        theme.switchText = false;
        theme.text = role(Roles::OnSurface());
        theme.muted = role(Roles::OnSurfaceVariant());
        theme.faint = role(Roles::Outline());
        theme.panel = role(Roles::Surface()) & 0xFFFFFFF0;
        theme.page = role(Roles::SurfaceContainerLow());
        theme.inside = role(Roles::SurfaceContainer());
        theme.cardLine = 0x00000000;
        theme.line = role(Roles::OutlineVariant());
        theme.picked = role(Roles::SecondaryContainer());
        theme.pickedText = role(Roles::OnSecondaryContainer());
        theme.button = role(Roles::SurfaceContainerHighest());
        theme.buttonLine = role(Roles::Outline());
        theme.gold = theme.accent = role(Roles::Primary());
        theme.onAccent = role(Roles::OnPrimary());
        theme.accentSoft = role(Roles::PrimaryContainer());
        theme.badgeFill = role(Roles::SecondaryContainer());
        theme.risky = role(Roles::Error());
        // Material has no warning or success colours, Windows 11's are used
        theme.warning = dark ? 0xFCE100FF : 0x9D5D00FF;
        theme.success = dark ? 0x6CCB5FFF : 0x0F7B0FFF;
        theme.done = theme.faint;
        theme.shadow = (role(Roles::Shadow()) & 0xFFFFFF00) | (dark ? 0x50 : 0x24);
        theme.switchOff = role(Roles::Outline());
        theme.switchTrack = role(Roles::SurfaceContainerHighest());
        materialRoles[dark ? 1 : 0] = { role(Roles::OnPrimaryContainer()), role(Roles::SecondaryContainer()), role(Roles::OnSurfaceVariant()) };
        return theme;
    }

    // made again only when the accent changes, GetTheme is asked for every piece
    static Theme materialThemes[2];
    static bool materialMade = false;

    void SetAccent(const std::optional<std::array<unsigned int, 7>>& shades) {
        if (materialMade && shades == accent) return;
        accent = shades;
        materialThemes[0] = materialColours(false);
        materialThemes[1] = materialColours(true);
        materialMade = true;
    }

    // the colours of a look; the sizes are set by GetTheme
    static Theme colours(Look look, bool dark) {
        if (look == Look::Material) {
            if (!materialMade) SetAccent(accent);
            return materialThemes[dark ? 1 : 0];
        }
        Theme theme;
        theme.look = look;
        theme.dark = dark;
        if (look == Look::Windows && !dark) {
            theme.text = 0x1B1B1BFF;
            theme.muted = 0x5D5D5DFF;
            theme.faint = 0x616161FF;
            theme.panel = 0xF3F3F3F0;
            theme.page = 0xF9F9F9FF;
            theme.inside = 0xFFFFFFFF;
            theme.cardLine = 0xE5E5E5FF;
            theme.line = 0x0000000F;
            theme.picked = 0xEAEAEAFF;
            theme.button = 0xFBFBFBFF;
            theme.buttonLine = 0xD6D6D6FF;
            theme.gold = theme.accent = 0x005FB8FF;
            theme.onAccent = 0xFFFFFFFF;
            theme.badgeFill = 0xDBE9F6FF;
            theme.warning = 0x9D5D00FF;
            theme.risky = 0xC42B1CFF;
            theme.success = 0x0F7B0FFF;
            theme.done = 0x8A8A8AFF;
            theme.shadow = 0x00000024;
            theme.switchOff = 0x616161FF;
        }
        else if (look == Look::Game) {
            theme.switchText = false;
            if (dark) {
                theme.text = 0xECE5D8FF;
                theme.muted = 0xA8A29AFF;
                theme.faint = 0x8C877FFF;
                theme.panel = 0x1C2030D0;
                theme.page = 0x1C2030FF;
                theme.inside = 0x3B4255FF;
                theme.cardLine = 0xD3BC8E40;
                theme.line = 0xD3BC8E60;
                theme.picked = 0x3B4255FF;
                theme.button = 0x3B4255FF;
                theme.buttonLine = 0xD3BC8E60;
                theme.gold = 0xD3BC8EFF;
                theme.accent = 0x4FB8FFFF;
                theme.onAccent = 0x10131CFF;
                theme.badgeFill = 0xD3BC8E33;
                theme.warning = 0xF2A33AFF;
                theme.risky = 0xED6661FF;
                theme.success = 0x7EC27AFF;
                theme.done = 0x969696FF;
                theme.switchOff = 0xA8A29AFF;
            }
            else {
                // the game's cream menus
                theme.text = 0x3B4255FF;
                theme.muted = 0x6E6A62FF;
                theme.faint = 0x8A8378FF;
                theme.panel = 0xE4DDD0F0;
                theme.page = 0xECE5D8FF;
                theme.inside = 0xF5F0E6FF;
                theme.cardLine = 0xD3BC8E99;
                theme.line = 0xD3BC8EFF;
                theme.picked = 0xDCD2BFFF;
                theme.button = 0xDDD4C3FF;
                theme.buttonLine = 0xC4AE84FF;
                theme.gold = 0x9A7D45FF;
                theme.accent = 0x2F78C4FF;
                theme.onAccent = 0xECE5D8FF;
                theme.badgeFill = 0x9A7D4533;
                theme.warning = 0xB86E0BFF;
                theme.risky = 0xC4443EFF;
                theme.success = 0x4C8A48FF;
                theme.done = 0x8A8378FF;
                theme.shadow = 0x00000030;
                theme.switchOff = 0x8A8378FF;
            }
        }
        else if (look == Look::Apple) {
            // macOS and iOS system colours, Apple's blue and green
            theme.switchText = false;
            theme.pickedText = theme.onAccent = 0xFFFFFFFF;
            if (dark) {
                theme.text = 0xFFFFFFFF;
                theme.muted = 0x98989FFF;
                theme.faint = 0x8E8E93FF;
                theme.panel = 0x2A2A2CF0;
                theme.page = 0x1C1C1EFF;
                theme.inside = 0x2C2C2EFF;
                theme.cardLine = 0x3A3A3CFF;
                theme.line = 0x3A3A3CFF;
                theme.button = 0x3A3A3CFF;
                theme.buttonLine = 0x3A3A3CFF;
                theme.picked = theme.accent = 0x0A84FFFF;
                theme.gold = 0x64A8FFFF;
                theme.badgeFill = 0x0A84FF33;
                theme.warning = 0xFF9F0AFF;
                theme.risky = 0xFF453AFF;
                theme.success = theme.switchOn = 0x30D158FF;
                theme.done = 0x8E8E93FF;
                theme.switchOff = theme.switchTrack = 0x39393DFF;
            }
            else {
                theme.text = 0x000000FF;
                theme.muted = 0x6C6C70FF;
                theme.faint = 0x8A8A8EFF;
                theme.panel = 0xE8E8EAF0;
                theme.page = 0xF2F2F7FF;
                theme.inside = 0xFFFFFFFF;
                theme.cardLine = 0xE5E5EAFF;
                theme.line = 0xC6C6C8FF;
                theme.button = 0xFFFFFFFF;
                theme.buttonLine = 0xD1D1D6FF;
                theme.picked = theme.gold = theme.accent = 0x007AFFFF;
                theme.badgeFill = 0x007AFF1F;
                // Apple's darker orange, red and green, for text on white
                theme.warning = 0xC93400FF;
                theme.risky = 0xD70015FF;
                theme.success = 0x248A3DFF;
                theme.switchOn = 0x34C759FF;
                theme.done = 0x8A8A8EFF;
                theme.shadow = 0x00000026;
                theme.switchOff = theme.switchTrack = 0xE9E9EAFF;
            }
            theme.accentSoft = theme.accent;
            return theme;
        }
        // Windows 11's accent fill: a lighter shade on dark, a darker one on light
        if (look == Look::Windows && accent) {
            theme.gold = theme.accent = dark ? (*accent)[1] : (*accent)[4];
            theme.badgeFill = blend(theme.accent, theme.page, dark ? 0.25f : 0.15f);
        }
        theme.pickedText = theme.text;
        theme.accentSoft = theme.accent;
        return theme;
    }

    void SetLook(Look look, bool dark) {
        playerLook = look;
        playerDark = dark;
    }

    Theme GetTheme() {
        return GetTheme(playerLook, playerDark);
    }

    Theme GetTheme(Look look, bool dark) {
        Theme theme = colours(look, dark);
        float font = ImGui::GetCurrentContext() ? ImGui::GetFontSize() : 13.0f;
        float scale = font / 13.0f;
        theme.font = font;
        theme.gap = 8 * scale;
        theme.rounding = (look == Look::Game || look == Look::Apple ? 6 : 4) * scale;
        theme.badge = 40 * scale;
        if (look == Look::Material) {
            // Material 3's switch, 52 x 32 dp at its 14 dp text: the knob 16
            // off, 24 on, 28 while held
            theme.switchWidth = 52.0f / 14;
            theme.switchHeight = 32.0f / 14;
            theme.knob = 0.5f;
            theme.knobOn = 0.75f;
            theme.knobHover = 0.0f;
            theme.knobHeld = 0.875f;
        }
        else if (look == Look::Apple) {
            // macOS's switch, 38 x 22 at its 13 px text, its knob 20 and a
            // little wider while held
            theme.switchWidth = 38.0f / 13;
            theme.switchHeight = 22.0f / 13;
            theme.knob = theme.knobOn = theme.knobHover = 20.0f / 22;
            theme.knobHeld = 25.0f / 22;
        }
        return theme;
    }

    ImU32 Color(unsigned int rgba) {
        return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
    }

    ImVec4 ColorVec(unsigned int rgba) {
        return ImGui::ColorConvertU32ToFloat4(Color(rgba));
    }

    // a over b at amount, as ImGui's colour
    static ImVec4 mix(unsigned int a, unsigned int b, float amount) {
        ImVec4 x = ColorVec(a), y = ColorVec(b);
        return ImVec4(y.x + (x.x - y.x) * amount, y.y + (x.y - y.y) * amount, y.z + (x.z - y.z) * amount, y.w + (x.w - y.w) * amount);
    }

    void ApplyStyle(float scale) {
        Theme theme = colours(playerLook, playerDark);
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* c = style.Colors;
        ImVec4 none(0, 0, 0, 0);
        c[ImGuiCol_Text] = ColorVec(theme.text);
        c[ImGuiCol_TextDisabled] = ColorVec(theme.muted);
        c[ImGuiCol_WindowBg] = ColorVec(theme.panel | 0xFF);
        c[ImGuiCol_ChildBg] = none;
        c[ImGuiCol_PopupBg] = ColorVec(theme.inside | 0xFF);
        // windows and frames share it, frames have an outline only in Windows 11
        c[ImGuiCol_Border] = ColorVec(playerLook == Look::Game ? theme.cardLine : theme.buttonLine);
        c[ImGuiCol_BorderShadow] = none;
        c[ImGuiCol_FrameBg] = ColorVec(theme.button);
        c[ImGuiCol_FrameBgHovered] = mix(theme.text, theme.button, 0.06f);
        c[ImGuiCol_FrameBgActive] = mix(theme.text, theme.button, 0.1f);
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = ColorVec(theme.panel | 0xFF);
        c[ImGuiCol_MenuBarBg] = ColorVec(theme.panel | 0xFF);
        c[ImGuiCol_ScrollbarBg] = none;
        c[ImGuiCol_ScrollbarGrab] = mix(theme.faint, theme.page, 0.5f);
        c[ImGuiCol_ScrollbarGrabHovered] = mix(theme.faint, theme.page, 0.75f);
        c[ImGuiCol_ScrollbarGrabActive] = ColorVec(theme.faint);
        c[ImGuiCol_CheckMark] = ColorVec(theme.accent);
        c[ImGuiCol_CheckboxSelectedBg] = ColorVec(theme.button);
        c[ImGuiCol_TreeLines] = ColorVec(theme.line);
        c[ImGuiCol_DragDropTargetBg] = none;
        c[ImGuiCol_UnsavedMarker] = ColorVec(theme.text);
        c[ImGuiCol_SliderGrab] = ColorVec(theme.accent);
        c[ImGuiCol_SliderGrabActive] = mix(theme.text, theme.accent, 0.2f);
        c[ImGuiCol_Button] = ColorVec(theme.button);
        c[ImGuiCol_ButtonHovered] = mix(theme.text, theme.button, 0.06f);
        c[ImGuiCol_ButtonActive] = mix(theme.text, theme.button, 0.1f);
        c[ImGuiCol_Header] = ColorVec(theme.picked);
        c[ImGuiCol_HeaderHovered] = mix(theme.text, theme.picked, 0.06f);
        c[ImGuiCol_HeaderActive] = mix(theme.text, theme.picked, 0.1f);
        c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = ColorVec(theme.line);
        c[ImGuiCol_SeparatorActive] = ColorVec(theme.accent);
        c[ImGuiCol_ResizeGrip] = none;
        c[ImGuiCol_ResizeGripHovered] = mix(theme.accent, theme.page, 0.5f);
        c[ImGuiCol_ResizeGripActive] = ColorVec(theme.accent);
        c[ImGuiCol_InputTextCursor] = ColorVec(theme.text);
        c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = none;
        c[ImGuiCol_TabHovered] = mix(theme.text, theme.page, 0.06f);
        c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = ColorVec(theme.picked);
        c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = ColorVec(theme.accent);
        c[ImGuiCol_PlotHistogram] = c[ImGuiCol_PlotLines] = ColorVec(theme.accent);
        c[ImGuiCol_PlotHistogramHovered] = c[ImGuiCol_PlotLinesHovered] = mix(theme.text, theme.accent, 0.2f);
        c[ImGuiCol_TableHeaderBg] = ColorVec(theme.picked);
        c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = ColorVec(theme.line);
        c[ImGuiCol_TableRowBg] = none;
        c[ImGuiCol_TableRowBgAlt] = mix(theme.text, theme.page, 0.03f);
        c[ImGuiCol_TextLink] = ColorVec(theme.accent);
        c[ImGuiCol_TextSelectedBg] = mix(theme.accent, theme.page, 0.35f);
        c[ImGuiCol_DragDropTarget] = ColorVec(theme.accent);
        c[ImGuiCol_NavCursor] = ColorVec(theme.accent);
        c[ImGuiCol_NavWindowingHighlight] = ColorVec(theme.text);
        c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.4f);

        bool game = playerLook == Look::Game;
        bool material = playerLook == Look::Material;
        bool apple = playerLook == Look::Apple;
        style.WindowRounding = (material ? 16 : apple ? 12 : 8) * scale;
        style.ChildRounding = 8 * scale;
        style.PopupRounding = (material ? 4 : 8) * scale;
        style.FrameRounding = (game || apple ? 6 : 4) * scale;
        style.GrabRounding = (game || apple ? 6 : 4) * scale;
        style.TabRounding = 4 * scale;
        style.ScrollbarRounding = 99 * scale;
        style.WindowBorderSize = 1;
        style.FrameBorderSize = game ? 0.0f : 1.0f;
        style.PopupBorderSize = 1;
        // Material's buttons and fields are M3's dense 32 dp, their text 16 in
        // from the side; the others keep ImGui's own
        style.FramePadding = material ? ImVec2(16 * scale, 9 * scale) : ImVec2(4 * scale, 3 * scale);
        if (material) {
            // outlined fields and buttons: no fill, the state layers of
            // Material, 8% under the mouse and 10% while pressed
            c[ImGuiCol_FrameBg] = none;
            c[ImGuiCol_FrameBgHovered] = ColorVec(Faded(theme.text, 0.08f));
            c[ImGuiCol_FrameBgActive] = ColorVec(Faded(theme.text, 0.1f));
            c[ImGuiCol_Button] = none;
            c[ImGuiCol_ButtonHovered] = ColorVec(Faded(theme.accent, 0.08f));
            c[ImGuiCol_ButtonActive] = ColorVec(Faded(theme.accent, 0.1f));
            c[ImGuiCol_Header] = ColorVec(theme.picked);
            c[ImGuiCol_HeaderHovered] = ColorVec(Faded(theme.text, 0.08f));
            c[ImGuiCol_HeaderActive] = ColorVec(Faded(theme.text, 0.1f));
        }
    }

    unsigned int Faded(unsigned int rgba, float amount) {
        unsigned int alpha = (unsigned int)std::lround((rgba & 0xFF) * std::clamp(amount, 0.0f, 1.0f));
        return (rgba & 0xFFFFFF00u) | alpha;
    }

    double StepFor(const NumberOptions& options) {
        if (options.whole) return options.step && *options.step > 0 ? std::max(1.0, std::round(*options.step)) : 1;
        if (options.step && *options.step > 0) return *options.step;
        if (options.min && options.max && *options.max > *options.min) return (*options.max - *options.min) / 100.0;
        return 0.1;
    }

    int DecimalsFor(const NumberOptions& options) {
        if (options.whole) return 0;
        // the fewest that write the step exactly: 0.25 two, 2.5 one
        double step = StepFor(options);
        for (int decimals = 0; decimals < 4; decimals++) {
            double scaled = step * std::pow(10.0, decimals);
            if (std::abs(scaled - std::round(scaled)) < 1e-6 * std::max(1.0, scaled)) return decimals;
        }
        return 4;
    }

    double Fit(double value, const NumberOptions& options) {
        if (!std::isfinite(value)) value = options.defaultValue.value_or(options.min.value_or(0));
        if (options.whole) value = std::clamp(std::round(value), (double)INT_MIN, (double)INT_MAX);
        if (options.min) value = std::max(value, *options.min);
        if (options.max) value = std::min(value, *options.max);
        return value;
    }

    static std::string format(const NumberOptions& options) {
        return "%." + std::to_string(DecimalsFor(options)) + "f";
    }

    static std::string written(double value, const NumberOptions& options) {
        char text[64];
        snprintf(text, sizeof(text), "%.*f", DecimalsFor(options), value);
        return text;
    }

    // the number being typed, one at a time since only one box has the keyboard
    struct Typed {
        ImGuiID id = 0;
        std::string text;
        int startFrame = 0;
        bool wasActive = false;
    };
    static Typed typed;

    // a click on the number without dragging it starts typing
    static bool numberBox(double& value, const NumberOptions& options, Result& result) {
        ImGuiID id = ImGui::GetID("##number");
        float width = ImGui::GetFontSize() * 4;
        if (typed.id == id) {
            ImGui::SetNextItemWidth(width);
            if (ImGui::GetFrameCount() == typed.startFrame) ImGui::SetKeyboardFocusHere();
            Typing::InputText("##typed", typed.text, 32, true, ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_AutoSelectAll);
            // the keyboard comes a frame or two after it's asked for
            bool active = ImGui::IsItemActive();
            bool left = typed.wasActive && !active;
            bool neverCame = !typed.wasActive && ImGui::GetFrameCount() > typed.startFrame + 4;
            typed.wasActive |= active;
            if (neverCame) typed = {};
            else if (left) {
                char* end = nullptr;
                double read = std::strtod(typed.text.c_str(), &end);
                if (end != typed.text.c_str()) {
                    double fitted = Fit(read, options);
                    result.changed |= fitted != value;
                    value = fitted;
                }
                typed = {};
                result.done = true;
            }
            return true;
        }

        double speed = options.min && options.max ? (*options.max - *options.min) / 200.0 : StepFor(options) / 4.0;
        if (options.whole) speed = std::max(speed, 0.05);
        std::string shown = written(value, options);
        ImGui::SetNextItemWidth(width);
        double min = options.min.value_or(-1e300), max = options.max.value_or(1e300);
        if (ImGui::DragScalar("##number", ImGuiDataType_Double, &value, static_cast<float>(speed), &min, &max,
                format(options).c_str(), ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp)) {
            value = Fit(value, options);
            result.changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) result.done = true;
        else if (ImGui::IsItemDeactivated()) {
            // let go where it was clicked: type it
            typed.id = id;
            typed.text = shown;
            typed.startFrame = ImGui::GetFrameCount() + 1;
        }
        return false;
    }

    static void stepButton(const char* text, double by, double& value, const NumberOptions& options, Result& result) {
        float side = ImGui::GetFrameHeight();
        // Material's outlined icon button: a circle, its minus or plus drawn
        // as its 20 dp icons are, 12 dp long and 2 dp thick
        bool round = isMaterial();
        if (round) ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, side / 2);
        ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
        std::string id = round ? std::string("##") + text : text;
        bool pressed = ImGui::Button(id.c_str(), ImVec2(side, side));
        if (round) {
            ImGui::PopStyleVar();
            float d = dp(), arm = 6 * d;
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 center(std::floor(min.x + side / 2), std::floor(min.y + side / 2));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImU32 ink = Color(materialRoles[GetTheme().dark ? 1 : 0].onSurfaceVariant);
            draw->AddRectFilled(ImVec2(center.x - arm, center.y - d), ImVec2(center.x + arm, center.y + d), ink);
            if (by > 0) draw->AddRectFilled(ImVec2(center.x - d, center.y - arm), ImVec2(center.x + d, center.y + arm), ink);
        }
        if (pressed) {
            double next = Fit(value + by, options);
            result.changed |= next != value;
            value = next;
        }
        ImGui::PopItemFlag();
        if (ImGui::IsItemDeactivated()) result.done = true;
    }

    // a section card's corners: Material's cards are 12 of its 14 px, Apple's
    // grouped lists 10, the others round as boxes do
    static float cardRounding(const Theme& theme) {
        if (theme.look == Look::Material) return theme.font * 12 / 14;
        if (theme.look == Look::Apple) return theme.font * 10 / 14;
        return ImGui::GetStyle().FrameRounding;
    }
    // a card's outline; Apple's cards have none, only lines between their rows
    static void cardOutline(ImDrawList* draw, ImVec2 min, ImVec2 max, const Theme& theme, float rounding, ImDrawFlags corners = 0) {
        if (theme.look != Look::Apple) draw->AddRect(min, max, Color(theme.cardLine), rounding, 1.0f, corners);
    }

    // the open sections' parts, innermost last; their rows share the part's card
    static std::vector<ImGuiWindow*> openParts;
    // the last frame a window drew a Reset at a row's end, by window, so
    // every row of it keeps Reset's room and they all end at the same edge
    // (kept in the window's own storage, frame + 1 so 0 is never)
    static ImGuiID resetSeenKey() {
        return ImHashStr("##resetSeen");
    }
    static bool windowHasReset() {
        int seen = ImGui::GetCurrentWindow()->StateStorage.GetInt(resetSeenKey(), 0) - 1;
        int frame = ImGui::GetFrameCount();
        return seen == frame || seen == frame - 1;
    }
    static void sawReset() {
        ImGui::GetCurrentWindow()->StateStorage.SetInt(resetSeenKey(), ImGui::GetFrameCount() + 1);
    }
    // Reset's room at a row's end, the gap before it included
    static float resetWidth() {
        if (isMaterial()) {
            LabelType type;
            return ImGui::CalcTextSize("Reset").x + 24 * dp();
        }
        return ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    }
    static float resetRoom() {
        return ImGui::GetStyle().ItemSpacing.x + resetWidth();
    }
    // Reset at a row's end: Material's text button (its label in the accent,
    // 12 dp each side, no outline), a small button in the others
    static bool resetButton() {
        if (!isMaterial()) return ImGui::SmallButton("Reset");
        LabelType type;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dp(), ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() / 2);
        ImGui::PushStyleColor(ImGuiCol_Text, ColorVec(GetTheme().accent));
        bool pressed = ImGui::Button("Reset");
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        return pressed;
    }
    // the room from the cursor to where a row's control ends, Reset's room left out
    static float rowAvail() {
        return ImGui::GetContentRegionAvail().x - (windowHasReset() ? resetRoom() : 0);
    }

    ImVec2 RowPadding() {
        // Material's list row: 56 dp tall, 16 in from each side
        if (isMaterial()) return ImVec2(std::floor(16 * dp()), std::floor((56 * dp() - ImGui::GetFrameHeight()) / 2));
        float font = ImGui::GetFontSize();
        return ImVec2(std::floor(font * 1.1f), std::floor((font * 2.9f - ImGui::GetFrameHeight()) / 2));
    }

    // One setting's row on its own card, as Windows 11's Settings has them,
    // as tall as a header with one line in it. Inside a section's part the
    // part is the card, so its rows get a line between them instead. The
    // row's pieces go between beginRow and endRow, the room they see
    // narrowed by the card's padding.
    struct RowCard {
        bool card;
        // Apple: right under the last row's card, so the two are one group
        bool joined = false;
        ImVec2 start;
        float width;
        ImVec2 pad;
        // the window's lowest point before the row, the row's own bottom measured apart from it
        float windowBottom;
        ImDrawListSplitter splitter;
    };
    // the last row card drawn, for Apple's groups
    struct LastCard { ImGuiWindow* window = nullptr; int frame = -1; float x = 0, width = 0, bottom = 0; };
    static LastCard lastCard;
    static void beginRow(RowCard& row) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        row.card = openParts.empty() || openParts.back() != window;
        row.start = ImGui::GetCursorScreenPos();
        row.width = ImGui::GetContentRegionAvail().x;
        row.pad = RowPadding();
        if (!row.card) row.pad.x = 0;
        // Apple's rows one after another make one group, a line between
        // them, as its grouped lists have
        if (row.card && GetTheme().look == Look::Apple && lastCard.window == window && lastCard.frame == ImGui::GetFrameCount()
            // (ImGui rounds the cursor, so up to a pixel off either way)
            && std::fabs(lastCard.x - row.start.x) < 1.5f && std::fabs(lastCard.width - row.width) < 1.5f
            && std::fabs(row.start.y - lastCard.bottom - ImGui::GetStyle().ItemSpacing.y) < 1.5f) {
            row.joined = true;
            // the card's edges lined up with the last one's
            row.start = ImVec2(lastCard.x, lastCard.bottom);
            row.width = lastCard.width;
        }
        if (!row.card && row.start.y > window->DC.CursorStartPos.y + 1) {
            float y = std::floor(row.start.y - ImGui::GetStyle().ItemSpacing.y / 2);
            window->DrawList->AddLine(ImVec2(row.start.x, y), ImVec2(row.start.x + row.width, y), Color(GetTheme().cardLine));
        }
        if (row.card) {
            row.splitter.Split(window->DrawList, 2);
            row.splitter.SetCurrentChannel(window->DrawList, 1);
        }
        // Indent(0) would indent by the style's own step
        if (row.pad.x > 0) ImGui::Indent(row.pad.x);
        ImGui::SetCursorScreenPos(ImVec2(window->DC.CursorPos.x, row.start.y + row.pad.y));
        row.windowBottom = window->DC.CursorMaxPos.y;
        window->DC.CursorMaxPos.y = row.start.y + row.pad.y;
        window->ContentRegionRect.Max.x -= row.pad.x;
        window->WorkRect.Max.x -= row.pad.x;
    }
    static void endRow(RowCard& row) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        window->ContentRegionRect.Max.x += row.pad.x;
        window->WorkRect.Max.x += row.pad.x;
        if (row.pad.x > 0) ImGui::Unindent(row.pad.x);
        float bottom = window->DC.CursorMaxPos.y + row.pad.y;
        if (row.card) {
            Theme theme = GetTheme();
            row.splitter.SetCurrentChannel(window->DrawList, 0);
            ImVec2 max(row.start.x + row.width, bottom);
            float rounding = cardRounding(theme);
            if (row.joined) {
                // squares off the last card's bottom corners, a line between at the text's start
                ImDrawList* draw = window->DrawList;
                draw->AddRectFilled(ImVec2(row.start.x, row.start.y - rounding), ImVec2(row.start.x + rounding, row.start.y), Color(theme.inside));
                draw->AddRectFilled(ImVec2(max.x - rounding, row.start.y - rounding), ImVec2(max.x, row.start.y), Color(theme.inside));
                draw->AddRectFilled(row.start, max, Color(theme.inside), rounding, ImDrawFlags_RoundCornersBottom);
                draw->AddLine(ImVec2(row.start.x + row.pad.x, row.start.y), ImVec2(max.x, row.start.y), Color(theme.cardLine));
            }
            else window->DrawList->AddRectFilled(row.start, max, Color(theme.inside), rounding);
            cardOutline(window->DrawList, row.start, max, theme, rounding);
            row.splitter.Merge(window->DrawList);
            lastCard = { window, ImGui::GetFrameCount(), row.start.x, row.width, bottom };
        }
        // the next piece under the card, the row's last piece kept as the last item
        window->DC.CursorPos.y = bottom + ImGui::GetStyle().ItemSpacing.y;
        window->DC.CursorMaxPos.y = std::max(row.windowBottom, bottom);
    }

    // Where a field's label goes: first, the field then filling the row up to
    // Reset at its end, or on a row above with Reset after it when the field
    // wouldn't get least beside it, the field then taking the whole row. fixed is the
    // row's parts that don't shrink; fill: the field takes the room left, else
    // it has none (a number box only) and the fixed parts end the row.
    // reset: Reset's room at the row's end, kept when another row of the
    // window has Reset; resetWidth: its width either way
    struct LabelPlace { float field; bool above; float reset; float resetWidth; };
    static LabelPlace placeLabel(const char* label, bool reset, float fixed, bool fill, float least) {
        const ImGuiStyle& style = ImGui::GetStyle();
        float name = 0;
        {
            NameType type;
            name = ImGui::CalcTextSize(label, ImGui::FindRenderedTextEnd(label)).x + style.ItemSpacing.x;
        }
        float resetWidth = reset || windowHasReset() ? resetRoom() : 0;
        float room = ImGui::GetContentRegionAvail().x - fixed - resetWidth;
        if (room - name >= least) {
            // a Reset at a row's end, so the window's other rows keep its room
            if (reset) sawReset();
            return { fill ? room - name : 0, false, resetWidth, resetWidth };
        }
        // on its own row the field takes what there is, short of spilling out of it
        return { fill ? std::max(room + resetWidth, ImGui::GetFrameHeight()) : 0, true, 0, resetWidth };
    }

    // a name beside a box: its line as tall as the box and the name in its
    // middle, whatever size the name is drawn at. frame: the box's height,
    // measured before the name's size is pushed
    static void alignName(float frame) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        window->DC.CurrLineSize.y = std::max(window->DC.CurrLineSize.y, frame);
        window->DC.CurrLineTextBaseOffset = std::max(window->DC.CurrLineTextBaseOffset, std::floor((frame - ImGui::GetTextLineHeight()) / 2));
    }

    // the label, wrapped short of room when it's on its own row
    static void labelText(const char* label, bool above, float room = 0) {
        const char* end = ImGui::FindRenderedTextEnd(label);
        float frame = ImGui::GetFrameHeight();
        NameType type;
        if (!above) alignName(frame);
        if (above) {
            float wrap = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - room;
            ImGui::PushTextWrapPos(std::max(wrap, ImGui::GetCursorPosX() + ImGui::GetFontSize() * 4));
        }
        ImGui::TextUnformatted(label, end);
        if (above) ImGui::PopTextWrapPos();
    }

    // The label first, as Choice and Toggle have it, then the field's row
    // (rowWidth wide) ending where Reset starts, on the next row when the
    // label is above, Reset then after the label. reset draws Reset.
    template <class F>
    static void labelFirst(const LabelPlace& place, float rowWidth, const char* label, bool hasReset, F reset) {
        float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - place.reset;
        labelText(label, place.above, hasReset ? place.resetWidth : 0);
        if (place.above) {
            if (hasReset) {
                ImGui::SameLine();
                reset();
            }
            return;
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - rowWidth));
    }
    // Reset at the row's end, when the label isn't above
    template <class F>
    static void resetLast(const LabelPlace& place, bool hasReset, F reset) {
        if (place.above || !hasReset) return;
        ImGui::SameLine();
        reset();
    }

    // macOS's slider: a thin track, blue up to a round white knob with a
    // shadow, the knob's middle kept a radius from each end
    static void appleSlider(ImVec2 min, ImVec2 max, float fraction, bool held) {
        Theme theme = GetTheme();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float font = ImGui::GetFontSize();
        float radius = std::min((max.y - min.y) / 2, font * 10 / 13);
        float track = std::max(2.0f, std::floor(font * 4 / 13));
        float middleY = std::floor((min.y + max.y) / 2);
        float left = min.x + radius, right = max.x - radius;
        float x = left + (right - left) * std::clamp(fraction, 0.0f, 1.0f);
        ImVec2 top(min.x, middleY - track / 2), bottom(max.x, middleY + track / 2);
        draw->AddRectFilled(top, bottom, Color(Faded(theme.text, 0.15f)), track / 2);
        draw->AddRectFilled(top, ImVec2(x, bottom.y), Color(theme.accent), track / 2);
        float px = std::max(1.0f, font / 13);
        draw->AddCircleFilled(ImVec2(x, middleY + px), radius + px * 0.5f, IM_COL32(0, 0, 0, 40));
        draw->AddCircle(ImVec2(x, middleY), radius, IM_COL32(0, 0, 0, 30), 0, px * 0.5f);
        draw->AddCircleFilled(ImVec2(x, middleY), radius, held ? IM_COL32(236, 236, 236, 255) : IM_COL32(255, 255, 255, 255));
    }

    // Material 3's slider at the dense boxes' size: an 8 dp track, the
    // accent up to a 4 x 24 dp handle with 4 dp of room each side of it, the
    // rest in its tonal colour with a dot at its end; the handle thinner
    // while held
    static void materialSlider(ImVec2 min, ImVec2 max, float fraction, bool held) {
        Theme theme = GetTheme();
        const MaterialRoles& roles = materialRoles[theme.dark ? 1 : 0];
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float d = dp();
        float track = 8 * d, handle = (held ? 2 : 4) * d, gap = 4 * d, handleHeight = 24 * d;
        float middle = std::floor((min.y + max.y) / 2);
        float top = middle - track / 2, bottom = middle + track / 2;
        // the handle's middle stays half a handle in from each end
        float left = min.x + 2 * d, right = max.x - 2 * d;
        float x = std::floor(left + (right - left) * std::clamp(fraction, 0.0f, 1.0f));
        float outer = track / 2, inner = 2 * d;
        if (x - gap - handle / 2 > min.x + d) {
            draw->AddRectFilled(ImVec2(min.x, top), ImVec2(x - gap - handle / 2, bottom), Color(theme.accent), outer, ImDrawFlags_RoundCornersLeft);
            draw->AddRectFilled(ImVec2(std::max(min.x + outer, x - gap - handle / 2 - inner), top), ImVec2(x - gap - handle / 2, bottom), Color(theme.accent), inner, ImDrawFlags_RoundCornersRight);
        }
        if (x + gap + handle / 2 < max.x - d) {
            draw->AddRectFilled(ImVec2(x + gap + handle / 2, top), ImVec2(max.x, bottom), Color(roles.secondaryContainer), outer, ImDrawFlags_RoundCornersRight);
            draw->AddRectFilled(ImVec2(x + gap + handle / 2, top), ImVec2(std::min(max.x - outer, x + gap + handle / 2 + inner), bottom), Color(roles.secondaryContainer), inner, ImDrawFlags_RoundCornersLeft);
            draw->AddCircleFilled(ImVec2(max.x - track / 2, middle), 1.5f * d, Color(theme.accent));
        }
        draw->AddRectFilled(ImVec2(x - handle / 2, middle - handleHeight / 2), ImVec2(x + handle / 2, middle + handleHeight / 2), Color(theme.accent), handle / 2);
    }

    Result Number(const char* label, double& value, const NumberOptions& options) {
        Result result;
        ImGui::PushID(label);
        RowCard row;
        beginRow(row);
        float gap = ImGui::GetStyle().ItemInnerSpacing.x;
        bool slider = !options.field && options.min && options.max && *options.max > *options.min;
        float font = ImGui::GetFontSize();
        float steppers = ImGui::GetFrameHeight() * 2 + font * 4 + gap * 2;
        // the slider's row ends where a text box's does
        float fixed = steppers + (slider ? ImGui::GetStyle().ItemSpacing.x : 0);
        LabelPlace place = placeLabel(label, options.defaultValue.has_value(), fixed, slider, slider ? font * 4 : 0);
        auto reset = [&] {
            ImGui::BeginDisabled(value == *options.defaultValue);
            if (resetButton()) {
                value = *options.defaultValue;
                result.changed = result.done = true;
            }
            ImGui::EndDisabled();
        };
        bool hasReset = options.defaultValue.has_value();
        labelFirst(place, place.field + fixed, label, hasReset, reset);
        if (slider) {
            double min = *options.min, max = *options.max;
            ImGui::SetNextItemWidth(place.field);
            bool ownSlider = GetTheme().look == Look::Apple || isMaterial();
            if (ownSlider) {
                // ImGui's slider kept for the dragging, drawn as macOS's or Material's below
                ImVec4 none(0, 0, 0, 0);
                for (ImGuiCol col : { ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive })
                    ImGui::PushStyleColor(col, none);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            }
            if (ImGui::SliderScalar("##slider", ImGuiDataType_Double, &value, &min, &max, "", ImGuiSliderFlags_NoInput)) {
                value = Fit(value, options);
                result.changed = true;
            }
            if (ownSlider) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(5);
                float fraction = (float)((value - min) / (max - min));
                if (isMaterial()) materialSlider(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), fraction, ImGui::IsItemActive());
                else appleSlider(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), fraction, ImGui::IsItemActive());
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) result.done = true;
            if (options.dotted) CornerDot(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetStyle().FrameRounding);
            ImGui::SameLine();
        }
        double step = StepFor(options);
        stepButton("-", -step, value, options, result);
        ImGui::SameLine(0, gap);
        numberBox(value, options, result);
        if (options.dotted && !slider) CornerDot(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetStyle().FrameRounding);
        ImGui::SameLine(0, gap);
        stepButton("+", step, value, options, result);
        resetLast(place, hasReset, reset);
        endRow(row);
        ImGui::PopID();
        return result;
    }
    // the last compact toggle, so the next one in the same window goes on its row
    static ImGuiID lastCompact = 0;
    static int lastCompactFrame = -1;

    bool Toggle(const char* label, bool* value, const ToggleOptions& options) {
        if (options.checkbox) {
            // the name first, the box at the row's end where a switch would be
            RowCard row;
            beginRow(row);
            ImGui::PushID(label);
            float side = ImGui::GetFrameHeight();
            float right = ImGui::GetCursorPosX() + rowAvail();
            float wrap = std::max(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 4, right - side - ImGui::GetStyle().ItemSpacing.x);
            {
                NameType type;
                alignName(side);
                ImGui::PushTextWrapPos(wrap);
                ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
                ImGui::PopTextWrapPos();
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - side));
            bool clicked = false;
            if (isMaterial()) {
                // Material 3's checkbox: an 18 dp box with 2 dp corners in the
                // middle of its 40 dp touch area, outlined while off, filled
                // with the accent and a tick while on; a round state layer
                if (ImGui::InvisibleButton("##box", ImVec2(side, side))) {
                    *value = !*value;
                    clicked = true;
                    ImGui::MarkItemEdited(ImGui::GetItemID());
                }
                Theme theme = GetTheme();
                const MaterialRoles& roles = materialRoles[theme.dark ? 1 : 0];
                ImDrawList* draw = ImGui::GetWindowDrawList();
                ImVec2 min = ImGui::GetItemRectMin();
                ImVec2 center(min.x + side / 2, min.y + side / 2);
                float d = dp(), half = 9 * d;
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    draw->AddCircleFilled(center, side / 2, Color(Faded(*value ? theme.accent : theme.text, ImGui::IsItemActive() ? 0.1f : 0.08f)));
                ImVec2 boxMin(std::floor(center.x - half), std::floor(center.y - half)), boxMax(boxMin.x + half * 2, boxMin.y + half * 2);
                if (*value) {
                    draw->AddRectFilled(boxMin, boxMax, Color(theme.accent), 2 * d);
                    ImGui::RenderCheckMark(draw, ImVec2(boxMin.x + 3 * d, boxMin.y + 3 * d), Color(theme.onAccent), 12 * d);
                }
                // the 2 dp line inside the box's 18
                else draw->AddRect(ImVec2(boxMin.x + d, boxMin.y + d), ImVec2(boxMax.x - d, boxMax.y - d), Color(roles.onSurfaceVariant), d, 2 * d);
            }
            else clicked = ImGui::Checkbox("##box", value);
            if (options.dotted) {
                ImVec2 min = ImGui::GetItemRectMin();
                CornerDot(min, ImVec2(min.x + side, min.y + side), ImGui::GetStyle().FrameRounding);
            }
            ImGui::PopID();
            endRow(row);
            return clicked;
        }

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiID id = window->GetID(label);
        float font = ImGui::GetFontSize();
        const char* labelEnd = ImGui::FindRenderedTextEnd(label);
        Theme theme = GetTheme();
        float trackHeight = font * theme.switchHeight * (options.compact ? 0.8f : 1.0f);
        float trackWidth = trackHeight * theme.switchWidth / theme.switchHeight;
        float gap = font * 0.6f;
        // On or Off before the switch, in a slot as wide as the wider of them
        bool stateText = theme.switchText && !options.compact;
        float stateWidth = stateText ? std::max(ImGui::CalcTextSize("On").x, ImGui::CalcTextSize("Off").x) + font * 0.85f : 0;
        // a full switch's name in Material's 16
        ImFont* nameFont = ImGui::GetFont();
        float nameSize = isMaterial() && !options.compact ? 16 * dp() : font;
        float iconSide = options.icon ? font * (options.compact ? 1.1f : 1.4f) : 0;
        float iconGap = options.icon ? font * 0.4f : 0;
        // a full switch is a row on its card, compact ones share a row
        RowCard row;
        if (!options.compact) beginRow(row);

        ImVec2 pos = window->DC.CursorPos;
        ImRect track;
        ImVec2 size;
        ImVec2 iconAt, textAt;
        float wrap = 0;
        if (options.compact) {
            ImVec2 text = ImGui::CalcTextSize(label, labelEnd);
            float width = trackWidth + gap + iconSide + iconGap + text.x;
            if (lastCompactFrame == ImGui::GetFrameCount() && g.LastItemData.ID == lastCompact) {
                ImGui::SameLine(0, font);
                if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
            }
            pos = window->DC.CursorPos;
            float height = std::max({ trackHeight, iconSide, text.y });
            float middle = pos.y + height / 2;
            // the name first, as every piece has it, the switch after it
            iconAt = ImVec2(pos.x, middle - iconSide / 2);
            textAt = ImVec2(std::floor(iconAt.x + iconSide + iconGap), std::floor(middle - text.y / 2));
            float trackLeft = textAt.x + text.x + gap;
            track = ImRect(ImVec2(trackLeft, middle - trackHeight / 2), ImVec2(trackLeft + trackWidth, middle + trackHeight / 2));
            size = ImVec2(width, height);
        }
        else {
            // the switch at the end of the list, so every name lines up and every switch does
            float avail = rowAvail();
            wrap = std::max(font * 4, avail - stateWidth - trackWidth - gap - iconSide - iconGap);
            ImVec2 text = nameFont->CalcTextSizeA(nameSize, FLT_MAX, wrap, label, labelEnd);
            // Material's switch sits in a box's height, so its rows are as tall as the others
            float band = isMaterial() ? std::max(trackHeight, ImGui::GetFrameHeight()) : trackHeight;
            // the name's first line centred on the switch
            float textTop = std::floor(pos.y + std::max(0.0f, (band - nameSize) / 2));
            float bottom = textTop + text.y;
            for (const ToggleLine& line : options.lines)
                bottom += std::floor(g.Style.ItemSpacing.y / 2) + ImGui::CalcTextSize(line.text.c_str(), nullptr, false, wrap).y;
            float right = pos.x + std::max(avail, iconSide + iconGap + text.x + gap + stateWidth + trackWidth);
            float trackTop = std::floor(pos.y + (band - trackHeight) / 2);
            track = ImRect(ImVec2(right - trackWidth, trackTop), ImVec2(right, trackTop + trackHeight));
            iconAt = ImVec2(pos.x, textTop + nameSize / 2 - iconSide / 2);
            textAt = ImVec2(pos.x + iconSide + iconGap, textTop);
            size = ImVec2(right - pos.x, std::max(band, bottom - pos.y));
        }

        ImGui::ItemSize(ImRect(pos, ImVec2(pos.x + size.x, pos.y + size.y)), 0);
        ImDrawList* draw = window->DrawList;
        if (options.icon) draw->AddImage(options.icon, iconAt, ImVec2(iconAt.x + iconSide, iconAt.y + iconSide));
        draw->AddText(nameFont, nameSize, textAt, ImGui::GetColorU32(ImGuiCol_Text), label, labelEnd, wrap);
        if (!options.compact) {
            float y = textAt.y + nameFont->CalcTextSizeA(nameSize, FLT_MAX, wrap, label, labelEnd).y;
            for (const ToggleLine& line : options.lines) {
                y += std::floor(g.Style.ItemSpacing.y / 2);
                draw->AddText(nullptr, 0, ImVec2(textAt.x, y),
                    ImGui::GetColorU32(line.grey ? ImGuiCol_TextDisabled : ImGuiCol_Text), line.text.c_str(), nullptr, wrap);
                y += ImGui::CalcTextSize(line.text.c_str(), nullptr, false, wrap).y;
            }
        }

        if (!ImGui::ItemAdd(track, id)) {
            if (!options.compact) endRow(row);
            return false;
        }
        bool hovered, held;
        bool pressed = ImGui::ButtonBehavior(track, id, &hovered, &held);
        if (pressed) {
            *value = !*value;
            ImGui::MarkItemEdited(id);
        }

        // Windows 11 and Game, off: an outline and a knob in the off colour; on:
        // filled with the accent; the knob grows under the mouse and stretches
        // while held
        float round = trackHeight / 2;
        float stroke = std::max(1.0f, font / 14);
        if (stateText) {
            const char* state = *value ? "On" : "Off";
            ImVec2 stateSize = ImGui::CalcTextSize(state);
            draw->AddText(ImVec2(std::floor(track.Min.x - font * 0.85f - stateSize.x), std::floor((track.Min.y + track.Max.y - stateSize.y) / 2)),
                ImGui::GetColorU32(ImGuiCol_Text), state);
        }
        float middle = (track.Min.y + track.Max.y) / 2;
        if (theme.look == Look::Material) {
            // Material 3: the track filled either way, outlined while off; the
            // knob grows from off to on and again while held, a soft circle
            // around it under the mouse
            if (*value) draw->AddRectFilled(track.Min, track.Max, Color(theme.accent), round);
            else {
                draw->AddRectFilled(track.Min, track.Max, Color(theme.switchTrack), round);
                draw->AddRect(track.Min, track.Max, Color(theme.switchOff), round, 0, std::max(1.0f, trackHeight / 16));
            }
            ImVec2 center(*value ? track.Max.x - round : track.Min.x + round, middle);
            bool touched = hovered || held;
            if (touched) draw->AddCircleFilled(center, trackHeight * 0.625f, Color(Faded(*value ? theme.accent : theme.text, held ? 0.1f : 0.08f)));
            float knobSize = trackHeight * (held ? theme.knobHeld : *value ? theme.knobOn : theme.knob);
            unsigned int knob = *value ? (touched ? theme.accentSoft : theme.onAccent) : (touched ? theme.muted : theme.switchOff);
            draw->AddCircleFilled(center, knobSize / 2, Color(knob));
        }
        else if (theme.look == Look::Apple) {
            // Apple: the track filled grey while off, green while on, a white
            // knob with a soft shadow under it, stretching while held
            unsigned int fill = *value ? (theme.switchOn ? theme.switchOn : theme.accent) : theme.switchTrack;
            draw->AddRectFilled(track.Min, track.Max, Color(fill), round);
            if (hovered) draw->AddRectFilled(track.Min, track.Max, Color(Faded(theme.dark ? 0xFFFFFFFF : 0x000000FF, 0.05f)), round);
            float knobHeight = trackHeight * theme.knob;
            float knobWidth = held ? trackHeight * theme.knobHeld : knobHeight;
            float inset = (trackHeight - knobHeight) / 2;
            float knobLeft = *value ? track.Max.x - inset - knobWidth : track.Min.x + inset;
            ImVec2 knobMin(knobLeft, middle - knobHeight / 2), knobMax(knobLeft + knobWidth, middle + knobHeight / 2);
            float knobRound = knobHeight / 2;
            // Apple's two shadows: a wide soft one 3 px down, a tight one 1 px down
            float px = trackHeight / 22;
            for (int i = 3; i >= 1; i--) {
                float grow = px * i * 1.3f;
                draw->AddRectFilled(ImVec2(knobMin.x - grow, knobMin.y - grow + px * 3), ImVec2(knobMax.x + grow, knobMax.y + grow + px * 3),
                    IM_COL32(0, 0, 0, 13), knobRound + grow);
            }
            draw->AddRectFilled(ImVec2(knobMin.x, knobMin.y + px), ImVec2(knobMax.x, knobMax.y + px), IM_COL32(0, 0, 0, 41), knobRound);
            draw->AddRectFilled(knobMin, knobMax, IM_COL32(255, 255, 255, 255), knobRound);
        }
        else {
            if (*value) {
                draw->AddRectFilled(track.Min, track.Max, ImGui::GetColorU32(hovered ? mix(theme.text, theme.accent, 0.1f) : ColorVec(theme.accent)), round);
            }
            else {
                if (hovered) draw->AddRectFilled(track.Min, track.Max, ImGui::GetColorU32(ImGuiCol_Text, 0.06f), round);
                draw->AddRect(track.Min, track.Max, Color(theme.switchOff), round, 0, stroke);
            }
            float knobHeight = trackHeight * (hovered || held ? theme.knobHover : *value ? theme.knobOn : theme.knob);
            float knobWidth = held ? trackHeight * theme.knobHeld : knobHeight;
            // its centre a half track in from the end it rests at, stretching toward the other
            float knobLeft = *value ? track.Max.x - round + knobHeight / 2 - knobWidth
                                    : track.Min.x + round - knobHeight / 2;
            ImU32 knob = Color(*value ? theme.onAccent : theme.switchOff);
            draw->AddRectFilled(ImVec2(knobLeft, middle - knobHeight / 2), ImVec2(knobLeft + knobWidth, middle + knobHeight / 2), knob, knobHeight / 2);
        }

        if (g.NavId == id) ImGui::RenderNavCursor(track, id);
        if (options.dotted) CornerDot(track.Min, track.Max, round);

        if (options.compact) {
            lastCompact = id;
            lastCompactFrame = ImGui::GetFrameCount();
        }
        else endRow(row);
        return pressed;
    }
    // edits not sent yet, by box
    static std::unordered_map<ImGuiID, std::string> unsent;

    // a square button the height of a box, with a drawn mark on it
    enum class Mark { Clear, Tick };
    // An X or a tick inside a text box's end, no frame of its own, a soft
    // fill under the mouse. accent: the tick that sends, a filled accent
    // button so it reads as one to press.
    static bool markButton(const char* id, Mark mark, bool accent, bool enabled = true) {
        float side = ImGui::GetFrameHeight();
        Theme theme = GetTheme();
        ImGui::BeginDisabled(!enabled);
        ImGui::PushStyleColor(ImGuiCol_Button, ColorVec(0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorVec(Faded(theme.text, 0.08f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorVec(Faded(theme.text, 0.14f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        bool pressed = ImGui::Button(id, ImVec2(side, side));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImU32 ink = accent ? Color(theme.onAccent) : ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        float inset = side * 0.32f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (accent) {
            float in = side * 0.12f;
            bool held = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
            ImVec4 fill = held ? mix(theme.text, theme.accent, 0.2f) : hovered ? mix(theme.text, theme.accent, 0.1f) : ColorVec(theme.accent);
            draw->AddRectFilled(ImVec2(min.x + in, min.y + in), ImVec2(max.x - in, max.y - in), ImGui::GetColorU32(fill),
                theme.look == Look::Material ? side : ImGui::GetStyle().FrameRounding);
        }
        if (mark == Mark::Clear) {
            float thick = std::max(1.0f, side / 14);
            draw->AddLine(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset), ink, thick);
            draw->AddLine(ImVec2(max.x - inset, min.y + inset), ImVec2(min.x + inset, max.y - inset), ink, thick);
        }
        else {
            ImGui::RenderCheckMark(draw, ImVec2(min.x + inset * 0.8f, min.y + inset * 0.8f), ink, side - inset * 1.6f);
        }
        return pressed;
    }

    bool Text(const char* label, std::string& value, const TextOptions& options) {
        bool changed = false;
        ImGui::PushID(label);
        RowCard row;
        beginRow(row);
        ImGuiID id = ImGui::GetID("##text");
        float side = ImGui::GetFrameHeight();
        // the X and the tick sit inside the box's end, a little apart, the
        // tick always last so it doesn't move
        float markGap = ImGui::GetFontSize() * 0.25f;
        float extras = (options.clear ? side : 0) + (options.confirm ? side : 0) + (options.clear && options.confirm ? markGap : 0);
        LabelPlace place = placeLabel(label, options.defaultValue.has_value(), extras, true, ImGui::GetFontSize() * 6);
        auto reset = [&] {
            ImGui::BeginDisabled(value == *options.defaultValue && !unsent.count(id));
            if (resetButton()) {
                unsent.erase(id);
                changed |= value != *options.defaultValue;
                value = *options.defaultValue;
            }
            ImGui::EndDisabled();
        };
        bool hasReset = options.defaultValue.has_value();
        labelFirst(place, place.field + extras, label, hasReset, reset);
        // after the label's row, so a Reset clicked there shows at once
        auto pending = unsent.find(id);
        bool hasUnsent = options.confirm && pending != unsent.end();
        std::string shown = hasUnsent ? pending->second : value;
        bool takenLook = options.confirm && options.taken && !hasUnsent && !value.empty() && ImGui::GetActiveID() != id;
        // one box behind the text and its X and tick, the text's own frame left out
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float rounding = ImGui::GetStyle().FrameRounding;
        ImVec2 boxMin = ImGui::GetCursorScreenPos();
        ImVec2 boxMax(boxMin.x + place.field + extras, boxMin.y + side);
        draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        ImGui::SetNextItemWidth(place.field);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ColorVec(0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ColorVec(0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ColorVec(0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        if (takenLook) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        bool typed = Typing::InputText("##text", shown, options.maxBytes, options.backToGame, 0,
            options.hint.empty() ? nullptr : options.hint.c_str());
        if (takenLook) ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        bool typing = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() && !typing) draw->AddRectFilled(boxMin, boxMax, Color(Faded(GetTheme().text, 0.04f)), rounding);
        bool deactivated = ImGui::IsItemDeactivated();
        bool enter = deactivated && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        bool escape = deactivated && ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        if (!options.confirm) {
            if (typed && shown != value) {
                value = shown;
                changed = true;
            }
        }
        else {
            if (typed && shown != value) unsent[id] = shown;
            else if (typed) unsent.erase(id);
            if (escape) unsent.erase(id);
        }

        auto send = [&] {
            auto it = unsent.find(id);
            if (it == unsent.end()) return;
            changed = it->second != value;
            value = it->second;
            unsent.erase(it);
        };
        if (enter) send();

        hasUnsent = options.confirm && unsent.count(id);
        std::string now = hasUnsent ? unsent[id] : value;
        if (options.clear) {
            ImGui::SameLine(0, 0);
            if (!now.empty() && !takenLook) {
                if (markButton("##clear", Mark::Clear, false)) {
                    if (options.confirm) {
                        if (value.empty()) unsent.erase(id);
                        else unsent[id] = "";
                    }
                    else {
                        changed = !value.empty();
                        value.clear();
                    }
                }
            }
            else ImGui::Dummy(ImVec2(side, side));
        }
        if (options.confirm) {
            ImGui::SameLine(0, options.clear ? markGap : 0);
            if (hasUnsent) {
                if (markButton("##send", Mark::Tick, true)) send();
            }
            else if (takenLook) markButton("##taken", Mark::Tick, false, false);
            else ImGui::Dummy(ImVec2(side, side));
        }
        // the box's line, the accent while typing
        Theme theme = GetTheme();
        // (Material's 2 dp)
        float focused = isMaterial() ? 2 * dp() : std::max(1.0f, ImGui::GetFontSize() * 0.1f);
        if (typing) draw->AddRect(boxMin, boxMax, Color(theme.accent), rounding, focused);
        else if (ImGui::GetStyle().FrameBorderSize > 0) draw->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border), rounding);
        if (options.dotted) CornerDot(boxMin, boxMax, rounding);
        resetLast(place, hasReset, reset);
        endRow(row);
        ImGui::PopID();
        return changed;
    }
    // the risky button asking "Sure?", one at a time
    static ImGuiID asking = 0;
    // a question not drawn last frame is dropped, e.g. the page changed
    static int askedFrame = -1;

    bool Button(const char* label, const ButtonOptions& options) {
        using Look = ButtonOptions::Look;
        ImGuiID id = ImGui::GetID(label);
        const char* labelEnd = ImGui::FindRenderedTextEnd(label);
        if (asking == id && askedFrame < ImGui::GetFrameCount() - 1) asking = 0;
        // Material's buttons are pills, 24 dp from the label to each end,
        // the label medium; outlined, the main one filled
        bool pill = isMaterial();
        LabelType type;
        if (pill) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() / 2);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24 * dp(), ImGui::GetStyle().FramePadding.y));
        }
        struct PopPill { bool pill; ~PopPill() { if (pill) ImGui::PopStyleVar(2); } } popPill{ pill };
        if (options.look == Look::Risky && asking == id && !options.busy) {
            askedFrame = ImGui::GetFrameCount();
            ImGui::PushID(label);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%.*s?", static_cast<int>(labelEnd - label), label);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ColorVec(GetTheme().risky));
            bool yes = ImGui::Button("Yes");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            bool no = ImGui::Button("No");
            ImGui::PopID();
            if (yes || no || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) asking = 0;
            return yes;
        }

        std::string shown;
        if (options.busy) {
            shown = options.busyLabel.empty() ? std::string(label, labelEnd) + "..." : options.busyLabel;
            shown += "###";
            shown += label;
        }
        int colors = 0;
        if (options.look == Look::Main) {
            Theme theme = GetTheme();
            ImGui::PushStyleColor(ImGuiCol_Button, ColorVec(theme.accent));
            unsigned int layer = pill ? theme.onAccent : theme.text;
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(layer, theme.accent, pill ? 0.08f : 0.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(layer, theme.accent, pill ? 0.1f : 0.2f));
            ImGui::PushStyleColor(ImGuiCol_Text, ColorVec(theme.onAccent));
            colors = 4;
            // Material's filled button has no outline
            if (pill) {
                ImGui::PushStyleColor(ImGuiCol_Border, ColorVec(0));
                colors++;
            }
        }
        else if (options.look == Look::Risky) {
            ImGui::PushStyleColor(ImGuiCol_Text, ColorVec(GetTheme().risky));
            colors = 1;
            if (pill) {
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorVec(Faded(GetTheme().risky, 0.08f)));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorVec(Faded(GetTheme().risky, 0.1f)));
                colors += 2;
            }
        }
        // Material's outlined button has its label in the accent
        else if (pill) {
            ImGui::PushStyleColor(ImGuiCol_Text, ColorVec(GetTheme().accent));
            colors = 1;
        }
        ImGui::BeginDisabled(options.busy);
        bool pressed = ImGui::Button(options.busy ? shown.c_str() : label, options.size);
        ImGui::EndDisabled();
        if (options.dotted) CornerDot(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetStyle().FrameRounding);
        ImGui::PopStyleColor(colors);
        if (pressed && options.look == Look::Risky) {
            asking = id;
            askedFrame = ImGui::GetFrameCount();
            return false;
        }
        return pressed;
    }
    ImVec2 ButtonSize(const char* label) {
        LabelType type;
        float pad = isMaterial() ? 24 * dp() : ImGui::GetStyle().FramePadding.x;
        return ImVec2(ImGui::CalcTextSize(label, ImGui::FindRenderedTextEnd(label)).x + pad * 2, ImGui::GetFrameHeight());
    }

    void RowName(const char* text) {
        NameType type;
        alignName(ImGui::GetFrameHeight());
        ImGui::TextUnformatted(text, ImGui::FindRenderedTextEnd(text));
    }

    float RowNameWidth(const char* text) {
        NameType type;
        return ImGui::CalcTextSize(text, ImGui::FindRenderedTextEnd(text)).x;
    }

    // Material's style vars pushed for the open list, popped by EndDropDown
    static bool dropDownPushed = false;

    bool BeginDropDown(const char* id, const char* preview, float width, int rows) {
        ImGui::SetNextItemWidth(width);
        // Material's exposed dropdown: an outlined box with its arrow on no
        // fill, its menu 8 dp in from top and bottom, 48 dp rows with the
        // name 12 dp in (a row's fill reaches half the spacing past its text)
        bool material = isMaterial();
        if (!material) return ImGui::BeginCombo(id, preview, rows > 8 ? ImGuiComboFlags_HeightLarge : 0);
        float d = dp();
        ImGui::PushStyleColor(ImGuiCol_Button, ColorVec(0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorVec(0));
        // a row's fill reaches half the spacing past its text, so the
        // list's padding is 8 dp past that
        float spacing = 48 * d - ImGui::GetTextLineHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * d, 8 * d + spacing / 2));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(24 * d, spacing));
        // up to 8 rows the list fits them all (2 pixels to spare, or
        // ImGui's rounding shows a scrollbar), more scroll with half a row
        // showing past the 8th
        ImGuiComboFlags flags = ImGuiComboFlags_NoArrowButton;
        float height = rows <= 8 ? rows * 48 * d + 16 * d + 2 : 8.5f * 48 * d + 16 * d;
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, height), ImVec2(FLT_MAX, height));
        // the box's corners before it's drawn: while open, the last item is the list
        ImVec2 min = ImGui::GetCursorScreenPos();
        ImVec2 max(min.x + width, min.y + ImGui::GetFrameHeight());
        ImDrawList* draw = ImGui::GetWindowDrawList();
        bool open = ImGui::BeginCombo(id, preview, flags);
        // M3's drop-down arrow: a small 10 by 5 dp triangle in a 24 dp
        // icon 12 dp from the end, pointing up while the list is open
        ImVec2 c((float)(int)(max.x - 24 * d), (float)(int)((min.y + max.y) / 2));
        float h = (open ? -1.0f : 1.0f) * 2.5f * d;
        draw->AddTriangleFilled(ImVec2(c.x - 5 * d, c.y - h), ImVec2(c.x + 5 * d, c.y - h), ImVec2(c.x, c.y + h), Color(materialRoles[GetTheme().dark ? 1 : 0].onSurfaceVariant));
        ImGui::PopStyleColor(2);
        if (!open) ImGui::PopStyleVar(2);
        dropDownPushed = open;
        return open;
    }

    void EndDropDown() {
        ImGui::EndCombo();
        if (dropDownPushed) ImGui::PopStyleVar(2);
        dropDownPushed = false;
    }

    bool Choice(const char* label, int& picked, const std::vector<std::string>& names) {
        if (names.empty()) return false;
        picked = std::clamp(picked, 0, (int)names.size() - 1);
        ImGuiStyle& style = ImGui::GetStyle();
        float widest = 0;
        for (const std::string& name : names) widest = std::max(widest, ImGui::CalcTextSize(name.c_str()).x);
        float width = widest + style.FramePadding.x * 2 + ImGui::GetFrameHeight();

        ImGui::PushID(label);
        RowCard row;
        beginRow(row);
        float right = ImGui::GetCursorPosX() + rowAvail();
        RowName(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - width));
        bool changed = false;
        if (BeginDropDown("##choice", names[picked].c_str(), width, (int)names.size())) {
            for (int i = 0; i < (int)names.size(); i++) {
                if (ImGui::Selectable(names[i].c_str(), i == picked) && i != picked) {
                    picked = i;
                    changed = true;
                }
                if (i == picked) ImGui::SetItemDefaultFocus();
            }
            EndDropDown();
        }
        endRow(row);
        ImGui::PopID();
        return changed;
    }

    static float dotRadius() {
        return ImGui::GetFontSize() * 0.22f;
    }

    float DotRoom() {
        return dotRadius() * 3.2f;
    }

    static void drawDot(ImVec2 center) {
        float radius = dotRadius();
        Theme theme = GetTheme();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddCircleFilled(center, radius * 1.3f, Color(theme.inside));
        draw->AddCircleFilled(center, radius, Color(theme.risky));
    }

    void Dot() {
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        float in = dotRadius() * 1.5f;
        drawDot(ImVec2(max.x - in, min.y + in));
    }

    void CornerDot(ImVec2 min, ImVec2 max, float rounding) {
        // centred on the corner, on a round one where its curve is
        float in = rounding * 0.29f;
        drawDot(ImVec2(max.x - in, min.y + in));
    }

    // the open one of a row of tabs by name, so tabs coming or going don't
    // move it, else by place, so a renamed one stays open
    static int openTab(const std::vector<std::string>& names) {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        int open = storage->GetInt(ImGui::GetID("count"), 0) == (int)names.size()
            ? std::clamp(storage->GetInt(ImGui::GetID("open"), 0), 0, (int)names.size() - 1) : 0;
        int openName = storage->GetInt(ImGui::GetID("name"), 0);
        for (int i = 0; i < (int)names.size(); i++) {
            if ((int)ImHashStr(names[i].c_str()) == openName) open = i;
        }
        return open;
    }

    static void keepTab(const std::vector<std::string>& names, int open) {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        storage->SetInt(ImGui::GetID("open"), open);
        storage->SetInt(ImGui::GetID("name"), (int)ImHashStr(names[open].c_str()));
        storage->SetInt(ImGui::GetID("count"), (int)names.size());
    }

    // Material 3's tabs: 48 dp tall, each name 16 dp in from its sides in
    // Title Small (14 medium), the open one in the accent with a 3 dp line
    // under the name, round on top; a state layer under the mouse
    static int materialTabs(const std::vector<std::string>& names, const std::vector<bool>& dotted, int open) {
        Theme theme = GetTheme();
        float d = dp();
        float rowX = ImGui::GetCursorScreenPos().x;
        float rowWidth = ImGui::GetContentRegionAvail().x;
        LabelType type;
        float pad = 12 * d, height = 40 * d, underline = 3 * d;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)names.size(); i++) {
            if (i > 0) ImGui::SameLine(0, 0);
            ImVec2 text = ImGui::CalcTextSize(names[i].c_str());
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(text.x + pad * 2, height))) open = i;
            ImGui::PopID();
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            bool shown = i == open;
            if (ImGui::IsItemHovered()) draw->AddRectFilled(min, max, Color(Faded(shown ? theme.accent : theme.text, ImGui::IsItemActive() ? 0.1f : 0.08f)));
            unsigned int color = shown ? theme.accent : ImGui::IsItemHovered() ? theme.text : theme.muted;
            draw->AddText(ImVec2(min.x + pad, std::floor(min.y + (height - text.y) / 2)), Color(color), names[i].c_str());
            if (shown) draw->AddRectFilled(ImVec2(min.x + pad, max.y - underline), ImVec2(min.x + pad + text.x, max.y), Color(theme.accent), underline, ImDrawFlags_RoundCornersTop);
            if (i < (int)dotted.size() && dotted[i]) CornerDot(ImVec2(min.x + pad, min.y + (height - text.y) / 2), ImVec2(min.x + pad + text.x, min.y + (height + text.y) / 2), 0);
        }
        float y = ImGui::GetItemRectMax().y;
        draw->AddLine(ImVec2(rowX, y), ImVec2(rowX + rowWidth, y), Color(theme.line));
        ImGui::ItemSize(ImVec2(0, 4 * d));
        return open;
    }

    int Tabs(const char* id, const std::vector<std::string>& names, const std::vector<bool>& dotted) {
        if (names.empty()) return 0;
        ImGui::PushID(id);
        int open = openTab(names);
        if (isMaterial()) {
            open = materialTabs(names, dotted, open);
            keepTab(names, open);
            ImGui::PopID();
            return open;
        }
        Theme theme = GetTheme();
        float rowX = ImGui::GetCursorScreenPos().x;
        float rowWidth = ImGui::GetContentRegionAvail().x;

        ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.9f);
        float font = ImGui::GetFontSize();
        bool wholeName = theme.look == Look::Game || theme.look == Look::Material;
        // Material's is 3 of its 14 px
        float underline = std::max(2.0f, font * (theme.look == Look::Material ? 0.21f : 0.14f));
        float height = ImGui::GetTextLineHeight() + font * 0.3f + underline;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < static_cast<int>(names.size()); i++) {
            if (i > 0) ImGui::SameLine(0, theme.gap * 2);
            bool dot = i < (int)dotted.size() && dotted[i];
            ImVec2 text = ImGui::CalcTextSize(names[i].c_str());
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(text.x + DotRoom(), height))) open = i;
            ImGui::PopID();
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            bool shown = i == open;
            unsigned int color = shown ? (wholeName ? theme.gold : theme.text) : ImGui::IsItemHovered() ? theme.text : theme.muted;
            draw->AddText(min, Color(color), names[i].c_str());
            // the game's and Material's line under the whole name (Material's
            // round on top only), Windows 11's short bar in its middle
            float bar = wholeName ? text.x : std::min(text.x, font * 1.2f);
            float barX = min.x + (text.x - bar) / 2;
            ImDrawFlags corners = theme.look == Look::Material ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
            if (shown) draw->AddRectFilled(ImVec2(barX, max.y - underline), ImVec2(barX + bar, max.y), Color(theme.gold), underline / 2, corners);
            if (dot) Dot();
        }
        ImGui::PopFont();
        // the thin line under the whole row
        float y = ImGui::GetItemRectMax().y;
        if (wholeName) draw->AddLine(ImVec2(rowX, y), ImVec2(rowX + rowWidth, y), Color(theme.line));
        ImGui::ItemSize(ImVec2(0, theme.gap * 0.5f));
        keepTab(names, open);
        ImGui::PopID();
        return open;
    }

    // Apple's segmented control: each row of pieces on a grey track, the
    // open piece raised on it, a thin line between the others
    static void appleSegments(ImDrawList* draw, const std::vector<std::string>& names, const std::vector<bool>& dotted,
        const std::vector<ImRect>& boxes, const std::vector<bool>& hovers, int open, float pad, float height) {
        Theme theme = GetTheme();
        float font = ImGui::GetFontSize();
        float inset = std::max(1.0f, std::floor(font * 2 / 14));
        float outer = font * 8 / 14, inner = font * 6 / 14;
        // the tracks, one per row
        for (int i = 0; i < (int)boxes.size();) {
            int end = i;
            while (end + 1 < (int)boxes.size() && boxes[end + 1].Min.y == boxes[i].Min.y) end++;
            draw->AddRectFilled(boxes[i].Min, ImVec2(boxes[end].Max.x, boxes[i].Max.y), Color(theme.switchTrack), outer);
            i = end + 1;
        }
        unsigned int raised = theme.dark ? 0x636366FF : 0xFFFFFFFF;
        for (int i = 0; i < (int)boxes.size(); i++) {
            ImRect box = boxes[i];
            ImVec2 min(box.Min.x + inset, box.Min.y + inset), max(box.Max.x - inset, box.Max.y - inset);
            bool shown = i == open;
            if (shown) {
                draw->AddRectFilled(ImVec2(min.x, min.y + inset / 2), ImVec2(max.x, max.y + inset), IM_COL32(0, 0, 0, 30), inner);
                draw->AddRectFilled(min, max, Color(raised), inner);
            }
            else if (hovers[i]) draw->AddRectFilled(min, max, Color(Faded(theme.text, 0.06f)), inner);
            // a line before a piece, except next to the raised one or a row's start
            bool rowStart = i == 0 || boxes[i - 1].Min.y != box.Min.y;
            if (!rowStart && !shown && i - 1 != open) {
                float h = (max.y - min.y) * 0.25f;
                draw->AddLine(ImVec2(box.Min.x, min.y + h), ImVec2(box.Min.x, max.y - h), Color(Faded(theme.muted, 0.4f)));
            }
            ImVec2 size = ImGui::CalcTextSize(names[i].c_str());
            draw->AddText(ImVec2(box.Min.x + pad, box.Min.y + (height - size.y) / 2), Color(theme.text), names[i].c_str());
            if (i < (int)dotted.size() && dotted[i]) {
                float in = dotRadius() * 1.5f;
                drawDot(ImVec2(box.Max.x - in, box.Min.y + in));
            }
        }
    }

    int Segments(const char* id, const std::vector<std::string>& names, const std::vector<bool>& dotted) {
        if (names.empty()) return 0;
        ImGui::PushID(id);
        int open = openTab(names);
        Theme theme = GetTheme();
        bool material = theme.look == Look::Material;
        float height = ImGui::GetFrameHeight();
        // Material's: 12 dp each side, the label medium, an 18 dp tick and 8 dp
        // before the open one's name, room for it kept in each so none moves
        LabelType type;
        float font = ImGui::GetFontSize();
        float pad = material ? 12 * dp() : font * 0.9f;
        float tick = material ? 26 * dp() : 0;
        float rounding = material ? height / 2 : ImGui::GetStyle().FrameRounding;
        float rowStart = ImGui::GetCursorScreenPos().x;
        float rowEnd = rowStart + ImGui::GetContentRegionAvail().x;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        // each piece's box, then the rows drawn once the pieces are placed
        std::vector<ImRect> boxes;
        std::vector<bool> hovers;
        for (int i = 0; i < (int)names.size(); i++) {
            float width = ImGui::CalcTextSize(names[i].c_str()).x + pad * 2 + tick + DotRoom();
            // a piece that doesn't fit starts a new row, under the last
            if (i > 0) {
                ImGui::SameLine(0, 0);
                if (ImGui::GetCursorScreenPos().x + width > rowEnd) {
                    ImGui::NewLine();
                    ImGui::SetCursorScreenPos(ImVec2(rowStart, ImGui::GetCursorScreenPos().y));
                }
            }
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##segment", ImVec2(width, height))) open = i;
            ImGui::PopID();
            boxes.push_back(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            hovers.push_back(ImGui::IsItemHovered());
        }
        if (theme.look == Look::Apple) {
            appleSegments(draw, names, dotted, boxes, hovers, open, pad, height);
            ImGui::ItemSize(ImVec2(0, theme.gap * 0.25f));
            keepTab(names, open);
            ImGui::PopID();
            return open;
        }
        for (int i = 0; i < (int)boxes.size(); i++) {
            const ImRect& box = boxes[i];
            bool first = i == 0 || boxes[i - 1].Min.y != box.Min.y;
            bool last = i + 1 == (int)boxes.size() || boxes[i + 1].Min.y != box.Min.y;
            ImDrawFlags corners = (first ? ImDrawFlags_RoundCornersLeft : 0) | (last ? ImDrawFlags_RoundCornersRight : 0);
            if (!corners) corners = ImDrawFlags_RoundCornersNone;
            bool shown = i == open;
            bool hovered = hovers[i];
            // Material's picked piece is its tonal fill, the others the accent
            unsigned int fill = shown ? (material ? theme.picked : theme.accent) : theme.button;
            draw->AddRectFilled(box.Min, box.Max, Color(fill), rounding, corners);
            if (hovered && !shown) draw->AddRectFilled(box.Min, box.Max, Color(Faded(theme.text, 0.05f)), rounding, corners);
            draw->AddRect(box.Min, box.Max, Color(theme.buttonLine), rounding, 1.0f, corners);
            unsigned int text = shown ? (material ? theme.pickedText : theme.onAccent) : theme.text;
            ImVec2 size = ImGui::CalcTextSize(names[i].c_str());
            float textX = box.Min.x + pad;
            if (material) {
                // the name, and the open one's tick before it, in the piece's middle
                float content = size.x + (shown ? tick : 0);
                float x = std::floor((box.Min.x + box.Max.x - DotRoom() - content) / 2);
                if (shown) {
                    float d = dp();
                    ImGui::RenderCheckMark(draw, ImVec2(x + 2 * d, box.Min.y + (height - 14 * d) / 2), Color(text), 14 * d);
                }
                textX = x + (shown ? tick : 0);
            }
            draw->AddText(ImVec2(textX, box.Min.y + (height - size.y) / 2), Color(text), names[i].c_str());
            if (i < (int)dotted.size() && dotted[i]) {
                float in = dotRadius() * 1.5f;
                drawDot(ImVec2(box.Max.x - in, box.Min.y + in));
            }
        }
        ImGui::ItemSize(ImVec2(0, theme.gap * 0.25f));
        keepTab(names, open);
        ImGui::PopID();
        return open;
    }

    // the label up to "##"
    static std::string shownLabel(const char* label) {
        return std::string(label, ImGui::FindRenderedTextEnd(label));
    }

    // A card row with the name and a chevron at its end, pointing down while
    // closed. joined: the part under it is a card that meets it. True when clicked.
    static bool appleHead(const char* label, bool open, bool dotted);
    static bool headerRow(const char* label, bool open, bool dotted, bool joined) {
        Theme theme = GetTheme();
        if (theme.look == Look::Apple) return appleHead(label, open, dotted);
        // Material's: 56 dp tall, 16 dp in, its name Title Medium (16 medium)
        bool material = theme.look == Look::Material;
        float rowHeight = material ? 56 * dp() : ImGui::GetFontSize() * 2.9f;
        if (material) pushType(16, true);
        struct PopType { bool on; ~PopType() { if (on) ImGui::PopFont(); } } popType{ material };
        float font = material ? 14 * dp() : ImGui::GetFontSize();
        float width = ImGui::GetContentRegionAvail().x;
        float pad = material ? 16 * dp() : font * 1.1f;
        float chevron = font * 0.32f;
        std::string shown = shownLabel(label);
        bool cut = false;
        std::vector<std::string> lines = FitLines(shown, width - pad * 3 - chevron * 2 - DotRoom(), 2, cut);
        float lineHeight = ImGui::GetTextLineHeight();
        float height = std::max(rowHeight, lineHeight * lines.size() + font * 1.2f);
        bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
        bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (cut) Tooltip(shown.c_str());
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        float rounding = cardRounding(theme);
        ImDrawFlags corners = joined && open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, Color(theme.inside), rounding, corners);
        if (hovered) draw->AddRectFilled(min, max, Color(Faded(theme.text, held ? 0.08f : 0.04f)), rounding, corners);
        cardOutline(draw, min, max, theme, rounding, corners);
        float y = min.y + (height - lineHeight * lines.size()) / 2;
        for (const std::string& line : lines) {
            draw->AddText(ImVec2(min.x + pad, y), Color(theme.text), line.c_str());
            y += lineHeight;
        }
        ImVec2 center(max.x - pad - chevron, (min.y + max.y) / 2);
        float rise = open ? chevron * 0.5f : -chevron * 0.5f;
        ImVec2 points[3] = {
            ImVec2(center.x - chevron, center.y + rise),
            ImVec2(center.x, center.y - rise),
            ImVec2(center.x + chevron, center.y + rise),
        };
        draw->AddPolyline(points, 3, Color(theme.text), ImDrawFlags_None, std::max(1.0f, font * 0.09f));
        if (dotted) CornerDot(min, max, rounding);
        return clicked;
    }

    // Apple's section head: the name small, muted and in capitals over its
    // group, no card, a small chevron after it to open and close it
    static bool appleHead(const char* label, bool open, bool dotted) {
        Theme theme = GetTheme();
        float width = ImGui::GetContentRegionAvail().x;
        std::string shown = shownLabel(label);
        for (char& c : shown) if ((unsigned char)c < 0x80) c = (char)std::toupper((unsigned char)c);
        ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 12 / 14);
        float font = ImGui::GetFontSize();
        float pad = std::floor(theme.font * 16 / 14);
        float chevron = font * 0.3f;
        bool cut = false;
        std::vector<std::string> lines = FitLines(shown, width - pad * 2 - chevron * 3 - DotRoom(), 2, cut);
        float lineHeight = ImGui::GetTextLineHeight();
        // room above, so a head reads as the start of a new group
        float height = lineHeight * lines.size() + font * 1.1f;
        bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
        bool hovered = ImGui::IsItemHovered();
        if (cut) Tooltip(shownLabel(label).c_str());
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        unsigned int ink = hovered ? theme.text : theme.muted;
        float y = max.y - lineHeight * lines.size() - font * 0.3f;
        float right = min.x + pad;
        for (const std::string& line : lines) {
            draw->AddText(ImVec2(min.x + pad, y), Color(ink), line.c_str());
            right = std::max(right, min.x + pad + ImGui::CalcTextSize(line.c_str()).x);
            y += lineHeight;
        }
        // after the last line, pointing down while open
        ImVec2 center(right + chevron * 2.2f, y - lineHeight / 2);
        float rise = open ? chevron * 0.5f : -chevron * 0.5f;
        ImVec2 points[3] = {
            ImVec2(center.x - chevron, center.y - rise),
            ImVec2(center.x, center.y + rise),
            ImVec2(center.x + chevron, center.y - rise),
        };
        if (!open) {
            // pointing to the side while closed, as Apple's disclosure arrow
            points[0] = ImVec2(center.x - chevron * 0.5f, center.y - chevron);
            points[1] = ImVec2(center.x + chevron * 0.5f, center.y);
            points[2] = ImVec2(center.x - chevron * 0.5f, center.y + chevron);
        }
        draw->AddPolyline(points, 3, Color(ink), ImDrawFlags_None, std::max(1.0f, font * 0.1f));
        ImGui::PopFont();
        if (dotted) CornerDot(ImVec2(min.x + pad, y - lineHeight * lines.size()), ImVec2(center.x + chevron, y), 0);
        return clicked;
    }

    Result Header(const char* label, bool& open, bool dotted) {
        Result result;
        if (headerRow(label, open, dotted, false)) {
            open = !open;
            result.changed = result.done = true;
        }
        return result;
    }

    // the card under an open header, ended by EndSection
    static void beginPart(ImGuiID id) {
        float font = ImGui::GetFontSize();
        // Apple's group sits under its head on its own, the others meet their header
        bool apple = GetTheme().look == Look::Apple;
        if (!apple) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 1.1f, font * 0.8f));
        ImGui::BeginChild(ImHashStr("##part", 0, id), ImVec2(0, 0),
            ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        openParts.push_back(ImGui::GetCurrentWindow());
        // the card behind the part, at its size of the last frame
        Theme theme = GetTheme();
        ImGuiWindow* card = ImGui::GetCurrentWindow();
        ImDrawList* under = card->ParentWindow->DrawList;
        ImVec2 min = card->Pos, max(card->Pos.x + card->Size.x, card->Pos.y + card->Size.y);
        float rounding = cardRounding(theme);
        if (apple) {
            under->AddRectFilled(min, max, Color(theme.inside), rounding);
            return;
        }
        under->AddRectFilled(min, max, Color(Faded(theme.inside, 0.6f)), rounding, ImDrawFlags_RoundCornersBottom);
        under->AddRect(min, max, Color(theme.cardLine), rounding, 1.0f, ImDrawFlags_RoundCornersBottom);
    }

    bool BeginSection(const char* label, bool defaultOpen, bool dotted) {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        ImGuiID id = ImGui::GetID(label);
        bool open = storage->GetBool(id, defaultOpen);
        if (headerRow(label, open, dotted, true)) {
            open = !open;
            storage->SetBool(id, open);
        }
        if (!open) return false;
        beginPart(id);
        return true;
    }

    Result BeginHeader(const char* label, bool& open, bool dotted) {
        Result result;
        ImGuiID id = ImGui::GetID(label);
        if (headerRow(label, open, dotted, true)) {
            open = !open;
            result.changed = result.done = true;
        }
        if (open) beginPart(id);
        return result;
    }

    void EndSection() {
        if (!openParts.empty()) openParts.pop_back();
        ImGui::EndChild();
    }

    void SetTitleFont(ImFont* font) {
        titleFont = font;
    }

    void PageTitle(const char* text) {
        // Material's Headline Small, 24 regular
        if (isMaterial()) ImGui::PushFont(nullptr, 24);
        else ImGui::PushFont(titleFont, ImGui::GetCurrentContext()->FontSizeBase * 1.45f);
        ImGui::TextWrapped("%s", text);
        ImGui::PopFont();
    }

    // the text's first character, a capital when it's a plain letter
    static std::string firstLetter(const char* text) {
        unsigned char lead = text[0];
        if (!lead) return "";
        size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        std::string letter(text, strnlen(text, length));
        if (letter.size() == 1) letter[0] = (char)std::toupper(lead);
        return letter;
    }

    // Apple's system colours for page tiles
    static const unsigned int appleTiles[] = { 0x0A84FFFF, 0x30D158FF, 0xFF9F0AFF, 0xBF5AF2FF, 0xFF375FFF, 0x64D2FFFF, 0x5E5CE6FF, 0xFF453AFF };

    // Apple's sidebar row: filled blue while picked, the icon on a coloured tile
    static bool applePageButton(const char* label, bool picked, ImTextureID icon, bool dotted, unsigned int tile) {
        Theme theme = GetTheme();
        float font = ImGui::GetFontSize();
        float width = ImGui::GetContentRegionAvail().x;
        float pad = font * 0.5f;
        float iconSize = std::floor(font * 22 / 14);
        float textX = pad + iconSize + font * 0.6f;
        bool cut = false;
        std::vector<std::string> lines = FitLines(label, width - textX - pad - DotRoom(), 2, cut);
        float lineHeight = ImGui::GetTextLineHeight();
        // Apple's 32 px row at 14 px text
        float height = std::max(font * 32 / 14, lineHeight * lines.size() + font * 0.6f);
        bool clicked = ImGui::InvisibleButton("##page", ImVec2(width, height));
        bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (cut) Tooltip(label);
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        float rounding = font * 6 / 14;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (picked) draw->AddRectFilled(min, max, Color(theme.picked), rounding);
        else if (hovered) draw->AddRectFilled(min, max, Color(Faded(theme.text, held ? 0.1f : 0.06f)), rounding);
        float middle = (min.y + max.y) / 2;
        ImVec2 at(min.x + pad, std::floor(middle - iconSize / 2)), to(at.x + iconSize, at.y + iconSize);
        float tileRound = font * 6 / 14;
        if (icon) draw->AddImageRounded(icon, at, to, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, tileRound);
        else {
            if (!tile) tile = appleTiles[ImHashStr(label) % (sizeof(appleTiles) / sizeof(appleTiles[0]))];
            draw->AddRectFilled(at, to, Color(tile), tileRound);
            std::string letter = firstLetter(label);
            ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.85f);
            ImVec2 size = ImGui::CalcTextSize(letter.c_str());
            draw->AddText(ImVec2(std::floor(at.x + (iconSize - size.x) / 2), std::floor(at.y + (iconSize - size.y) / 2)), IM_COL32_WHITE, letter.c_str());
            ImGui::PopFont();
        }
        float y = min.y + (height - lineHeight * lines.size()) / 2;
        for (const std::string& line : lines) {
            draw->AddText(ImVec2(min.x + textX, y), Color(picked ? theme.pickedText : theme.text), line.c_str());
            y += lineHeight;
        }
        if (dotted) CornerDot(min, max, rounding);
        return clicked;
    }

    // Material 3's navigation drawer row: 56 dp tall, a pill in the tonal
    // colour while picked, a 24 dp icon 16 dp in and the name 12 dp after it
    // in Label Large (14 medium). Without an icon, the name's first letter on
    // a round tile, as Material's avatars have it.
    static bool materialPageButton(const char* label, bool picked, ImTextureID icon, bool dotted) {
        Theme theme = GetTheme();
        const MaterialRoles& roles = materialRoles[theme.dark ? 1 : 0];
        LabelType type;
        float d = dp();
        float width = ImGui::GetContentRegionAvail().x;
        float pad = 16 * d, iconSize = 24 * d;
        float textX = pad + iconSize + 12 * d;
        bool cut = false;
        std::vector<std::string> lines = FitLines(label, width - textX - pad - DotRoom(), 2, cut);
        float lineHeight = ImGui::GetTextLineHeight();
        float height = std::max(56 * d, lineHeight * lines.size() + 16 * d);
        bool clicked = ImGui::InvisibleButton("##page", ImVec2(width, height));
        bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (cut) Tooltip(label);
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        float rounding = height / 2;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (picked) draw->AddRectFilled(min, max, Color(theme.picked), rounding);
        if (hovered) draw->AddRectFilled(min, max, Color(Faded(picked ? theme.pickedText : theme.text, held ? 0.1f : 0.08f)), rounding);
        float middle = std::floor((min.y + max.y) / 2);
        ImVec2 at(min.x + pad, middle - iconSize / 2), to(at.x + iconSize, at.y + iconSize);
        if (icon) draw->AddImage(icon, at, to);
        else {
            ImVec2 center(at.x + iconSize / 2, middle);
            draw->AddCircleFilled(center, iconSize / 2, Color(theme.accentSoft));
            std::string letter = firstLetter(label);
            ImGui::PushFont(nullptr, 12);
            ImVec2 size = ImGui::CalcTextSize(letter.c_str());
            draw->AddText(ImVec2(std::floor(center.x - size.x / 2), std::floor(center.y - size.y / 2)), Color(roles.onPrimaryContainer), letter.c_str());
            ImGui::PopFont();
        }
        float y = min.y + (height - lineHeight * lines.size()) / 2;
        for (const std::string& line : lines) {
            draw->AddText(ImVec2(min.x + textX, y), Color(picked ? theme.pickedText : roles.onSurfaceVariant), line.c_str());
            y += lineHeight;
        }
        if (dotted) CornerDot(min, max, rounding);
        return clicked;
    }

    bool PageButton(const char* label, bool picked, ImTextureID icon, bool dotted, unsigned int tile) {
        Theme theme = GetTheme();
        if (theme.look == Look::Apple) return applePageButton(label, picked, icon, dotted, tile);
        if (theme.look == Look::Material) return materialPageButton(label, picked, icon, dotted);
        float font = ImGui::GetFontSize();
        float width = ImGui::GetContentRegionAvail().x;
        float pad = font * 0.75f;
        float iconSize = font * 1.15f;
        float textX = pad + iconSize + font * 0.8f;
        bool cut = false;
        std::vector<std::string> lines = FitLines(label, width - textX - pad - DotRoom(), 2, cut);
        float lineHeight = ImGui::GetTextLineHeight();
        float height = std::max(font * 2.6f, lineHeight * lines.size() + font * 0.9f);
        bool clicked = ImGui::InvisibleButton("##page", ImVec2(width, height));
        bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (cut) Tooltip(label);
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        float rounding = ImGui::GetStyle().FrameRounding;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (picked) draw->AddRectFilled(min, max, Color(theme.picked), rounding);
        if (hovered) draw->AddRectFilled(min, max, Color(Faded(theme.text, held ? 0.08f : 0.05f)), rounding);
        float middle = (min.y + max.y) / 2;
        if (picked) {
            float bar = std::max(2.0f, font * 0.21f);
            draw->AddRectFilled(ImVec2(min.x, middle - iconSize / 2), ImVec2(min.x + bar, middle + iconSize / 2), Color(theme.gold), bar / 2);
        }
        ImVec2 at(min.x + pad, middle - iconSize / 2), to(at.x + iconSize, at.y + iconSize);
        if (icon) draw->AddImage(icon, at, to);
        else {
            std::string letter = firstLetter(label);
            draw->AddRectFilled(at, to, Color(theme.inside), font * 0.25f);
            draw->AddRect(at, to, Color(theme.cardLine), font * 0.25f);
            ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.8f);
            ImVec2 size = ImGui::CalcTextSize(letter.c_str());
            draw->AddText(ImVec2(at.x + (iconSize - size.x) / 2, at.y + (iconSize - size.y) / 2), Color(theme.text), letter.c_str());
            ImGui::PopFont();
        }
        float y = min.y + (height - lineHeight * lines.size()) / 2;
        for (const std::string& line : lines) {
            draw->AddText(ImVec2(min.x + textX, y), Color(picked ? theme.pickedText : theme.text), line.c_str());
            y += lineHeight;
        }
        if (dotted) Dot();
        return clicked;
    }

    void Tag(const char* text) {
        Theme theme = GetTheme();
        float before = ImGui::GetItemRectSize().y;
        ImGui::SameLine();
        // Material's Label Medium, 12 medium
        if (isMaterial()) pushType(12, true);
        else ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.85f);
        float font = ImGui::GetFontSize();
        ImVec2 size = ImGui::CalcTextSize(text);
        ImVec2 pad(font * 0.5f, font * 0.15f);
        ImVec2 box(size.x + pad.x * 2, size.y + pad.y * 2);
        // centred on the item before it, under it when the row has no room
        if (ImGui::GetContentRegionAvail().x < box.x) ImGui::NewLine();
        else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (before - box.y) / 2);
        ImGui::Dummy(box);
        ImVec2 min = ImGui::GetItemRectMin();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, ImVec2(min.x + box.x, min.y + box.y), Color(theme.badgeFill), box.y / 2);
        draw->AddText(ImVec2(min.x + pad.x, min.y + pad.y), Color(theme.gold), text);
        ImGui::PopFont();
    }

    bool Tooltip(const char* text, TipAt at) {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_AllowWhenDisabled)) return false;
        ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        if (at == TipAt::Under)
            ImGui::SetNextWindowPos(ImVec2(max.x, max.y + ImGui::GetStyle().ItemSpacing.y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        else if (at == TipAt::Left)
            ImGui::SetNextWindowPos(ImVec2(min.x - ImGui::GetStyle().ItemSpacing.x, (min.y + max.y) / 2), ImGuiCond_Always, ImVec2(1.0f, 0.5f));
        if (!ImGui::BeginTooltip()) return false;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        return true;
    }

    // a three quarter ring turning once a second, one font size wide
    static void spinner() {
        float size = ImGui::GetFontSize();
        ImVec2 at = ImGui::GetCursorScreenPos();
        float lineHeight = ImGui::GetTextLineHeight();
        ImGui::Dummy(ImVec2(size, lineHeight));
        ImVec2 center(at.x + size / 2, at.y + lineHeight / 2);
        float radius = size * 0.35f;
        float start = (float)ImGui::GetTime() * 2.0f * IM_PI;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PathArcTo(center, radius, start, start + IM_PI * 1.5f, 16);
        draw->PathStroke(ImGui::GetColorU32(ImGuiCol_TextDisabled), 0, std::max(1.0f, size * 0.12f));
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    }

    static void mutedLine(const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }

    void Waiting(const char* text) {
        spinner();
        mutedLine(text);
    }

    void Empty(const char* text) {
        mutedLine(text);
    }

    void Progress(const char* text, float fraction) {
        bool known = fraction >= 0;
        if (!known) spinner();
        ImGui::TextWrapped("%s", text);
        if (isMaterial() && known) {
            // Material 3's bar: 4 dp, the accent up to where it is, 4 dp of
            // room, the rest in its tonal colour with a dot at its end
            Theme theme = GetTheme();
            const MaterialRoles& roles = materialRoles[theme.dark ? 1 : 0];
            float d = dp(), bar = 4 * d;
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, bar));
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            float x = min.x + (max.x - min.x) * std::clamp(fraction, 0.0f, 1.0f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (x > min.x) draw->AddRectFilled(min, ImVec2(x, max.y), Color(theme.accent), bar / 2);
            if (x + bar < max.x) {
                draw->AddRectFilled(ImVec2(x + bar, min.y), max, Color(roles.secondaryContainer), bar / 2);
                draw->AddCircleFilled(ImVec2(max.x - bar / 2, (min.y + max.y) / 2), bar / 2, Color(theme.accent));
            }
            return;
        }
        float height = ImGui::GetFontSize() * 0.6f;
        // ImGui runs the bar back and forth for a negative fraction
        float shown = known ? std::min(fraction, 1.0f) : -(float)ImGui::GetTime();
        ImGui::ProgressBar(shown, ImVec2(-FLT_MIN, height), "");
    }

    void Badge(ImDrawList* draw, ImVec2 center, float radius, ImTextureID icon, const BadgeOptions& options) {
        if (radius <= 0) return;
        Theme theme = GetTheme(Look::Game, true);
        float amount = options.faded ? theme.faded : 1.0f;
        unsigned int ring = options.ring.value_or(options.faded ? theme.done : theme.text);
        draw->AddCircleFilled(ImVec2(center.x, center.y + radius * 0.08f), radius, Color(Faded(theme.shadow, amount)));
        draw->AddCircleFilled(center, radius * 0.94f, Color(Faded(ring, amount)));
        draw->AddCircleFilled(center, radius * 0.8f, Color(Faded(theme.inside, amount)));
        if (icon) {
            float half = radius * 0.6f;
            draw->AddImage(icon, ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
                ImVec2(0, 0), ImVec2(1, 1), Color(Faded(0xFFFFFFFF, amount)));
        }
    }

    void Bubble(ImDrawList* draw, ImVec2 center, float radius, const char* text, std::optional<unsigned int> fill) {
        if (radius <= 0) return;
        Theme theme = GetTheme(Look::Game, true);
        draw->AddCircleFilled(center, radius, Color(theme.inside));
        draw->AddCircleFilled(center, radius * 0.86f, Color(fill.value_or(theme.gold)));
        if (!text || !*text) return;
        float size = radius * 1.3f;
        ImVec2 measured = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
        // a longer count gets a smaller font, to stay inside
        if (measured.x > radius * 1.6f) {
            size *= radius * 1.6f / measured.x;
            measured = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
        }
        draw->AddText(ImGui::GetFont(), size, ImVec2(center.x - measured.x / 2, center.y - measured.y / 2), Color(0x1C2030FF), text);
    }

    // UTF-8: where the character after the one at at starts, and where the last one starts
    static size_t nextChar(const std::string& text, size_t at) {
        at++;
        while (at < text.size() && (text[at] & 0xC0) == 0x80) at++;
        return at;
    }

    size_t lastChar(const std::string& text) {
        size_t at = text.size();
        while (at > 0 && (text[--at] & 0xC0) == 0x80) {}
        return at;
    }

    std::vector<std::string> FitLines(const std::string& text, float width, int maxLines, bool& cut) {
        cut = false;
        std::vector<std::string> lines;
        auto wide = [](const std::string& line) { return ImGui::CalcTextSize(line.c_str()).x; };
        size_t at = 0;
        while (at < text.size() && (int)lines.size() < maxLines) {
            while (at < text.size() && text[at] == ' ') at++;
            if (at >= text.size()) break;
            // as many words as fit, or as much of one long word
            size_t end = at, fits = at;
            while (end < text.size()) {
                size_t next = text.find(' ', end + 1);
                if (next == std::string::npos) next = text.size();
                if (wide(text.substr(at, next - at)) > width) break;
                fits = end = next;
            }
            if (fits == at) {
                fits = nextChar(text, at);
                while (fits < text.size() && wide(text.substr(at, nextChar(text, fits) - at)) <= width) fits = nextChar(text, fits);
            }
            lines.push_back(text.substr(at, fits - at));
            at = fits;
        }
        while (at < text.size() && text[at] == ' ') at++;
        if (at < text.size() && !lines.empty()) {
            cut = true;
            std::string& last = lines.back();
            while (!last.empty() && wide(last + "...") > width) last.erase(lastChar(last));
            while (!last.empty() && last.back() == ' ') last.pop_back();
            last += "...";
        }
        return lines;
    }
}
