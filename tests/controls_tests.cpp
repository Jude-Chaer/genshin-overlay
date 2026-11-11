#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include "controls.hpp"
#include "extensions.hpp"
#include <climits>
#include <cmath>

using Catch::Matchers::WithinAbs;
using Controls::NumberOptions;

namespace {
    // frames with no screen, one number piece in a window at the corner
    struct Frames {
        ImGuiContext* context = ImGui::CreateContext();
        // where the pieces of the row are, read while drawing
        float minusX = 0, boxX = 0, plusX = 0, rowY = 0;

        Frames() {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }
        ~Frames() { ImGui::DestroyContext(context); }

        Controls::Result number(double& value, const NumberOptions& options) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(700, 200));
            ImGui::Begin("page", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
            // the label first, the - box + at the row's end, before Reset,
            // inside the row's card
            ImVec2 pad = Controls::RowPadding();
            ImVec2 start(ImGui::GetCursorScreenPos().x + pad.x, ImGui::GetCursorScreenPos().y + pad.y);
            const ImGuiStyle& style = ImGui::GetStyle();
            float reset = options.defaultValue ? style.ItemSpacing.x + ImGui::CalcTextSize("Reset").x + style.FramePadding.x * 2 : 0;
            float right = start.x + ImGui::GetContentRegionAvail().x - pad.x * 2 - reset;
            float side = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
            plusX = right - side / 2;
            boxX = right - side - gap - ImGui::GetFontSize() * 2;
            minusX = right - side - gap - ImGui::GetFontSize() * 4 - gap - side / 2;
            rowY = start.y + side / 2;
            auto result = Controls::Number("Opacity", value, options);
            ImGui::End();
            ImGui::EndFrame();
            return result;
        }

        // press and let go at x, two frames each, so ImGui sees both
        Controls::Result click(float x, double& value, const NumberOptions& options) {
            Controls::Result all;
            auto add = [&](Controls::Result r) { all.changed |= r.changed; all.done |= r.done; };
            ImGui::GetIO().AddMousePosEvent(x, rowY);
            add(number(value, options));
            ImGui::GetIO().AddMouseButtonEvent(0, true);
            add(number(value, options));
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            add(number(value, options));
            add(number(value, options));
            return all;
        }

        // click the number, then frames until its box has the keyboard
        bool startTyping(double& value, const NumberOptions& options) {
            click(boxX, value, options);
            for (int i = 0; i < 5 && ImGui::GetActiveID() == 0; i++) number(value, options);
            return ImGui::GetActiveID() != 0;
        }
    };

    NumberOptions opacity() {
        NumberOptions options;
        options.min = 0;
        options.max = 1;
        options.field = true;
        options.defaultValue = 0.5;
        return options;
    }
}

TEST_CASE("a number's step is the author's, else 1 for whole numbers, else a hundredth of the range") {
    NumberOptions options;
    CHECK(Controls::StepFor(options) == 0.1);
    options.whole = true;
    CHECK(Controls::StepFor(options) == 1);
    options.whole = false;
    options.min = 0;
    options.max = 1;
    CHECK_THAT(Controls::StepFor(options), WithinAbs(0.01, 1e-12));
    CHECK(Controls::DecimalsFor(options) == 2);
    options.step = 0.25;
    CHECK(Controls::StepFor(options) == 0.25);
    options.min = 0;
    options.max = 500;
    options.step.reset();
    CHECK(Controls::StepFor(options) == 5);
    CHECK(Controls::DecimalsFor(options) == 0);
}

TEST_CASE("a whole number's step is whole, and the decimals write the step exactly") {
    NumberOptions options;
    options.whole = true;
    options.step = 0.5;
    CHECK(Controls::StepFor(options) == 1);
    options.step = 2.4;
    CHECK(Controls::StepFor(options) == 2);
    options.whole = false;
    options.step = 0.25;
    CHECK(Controls::DecimalsFor(options) == 2);
    options.step = 2.5;
    CHECK(Controls::DecimalsFor(options) == 1);
    options.step = 0.1;
    CHECK(Controls::DecimalsFor(options) == 1);
    options.step = 3;
    CHECK(Controls::DecimalsFor(options) == 0);
}

TEST_CASE("a whole number stays inside what an int holds") {
    NumberOptions options;
    options.whole = true;
    CHECK(Controls::Fit(1e12, options) == INT_MAX);
    CHECK(Controls::Fit(-1e12, options) == INT_MIN);
}

TEST_CASE("dragging a whole number slowly still moves it") {
    Frames frames;
    NumberOptions options;
    options.min = 0;
    options.max = 50;
    options.whole = true;
    options.field = true;
    double value = 3;
    frames.number(value, options);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(frames.boxX, frames.rowY);
    frames.number(value, options);
    io.AddMouseButtonEvent(0, true);
    frames.number(value, options);
    // a pixel a frame, a quarter of a step each
    for (int i = 1; i <= 30; i++) {
        io.AddMousePosEvent(frames.boxX + i, frames.rowY);
        frames.number(value, options);
    }
    io.AddMouseButtonEvent(0, false);
    frames.number(value, options);
    CHECK(value >= 8);
    CHECK(value == std::round(value));
}

TEST_CASE("a number is kept inside its range and whole when it should be") {
    NumberOptions options;
    options.min = 0;
    options.max = 10;
    CHECK(Controls::Fit(12, options) == 10);
    CHECK(Controls::Fit(-3, options) == 0);
    options.whole = true;
    CHECK(Controls::Fit(4.6, options) == 5);
    options.defaultValue = 3;
    CHECK(Controls::Fit(std::nan(""), options) == 3);
}

TEST_CASE("+ and - move a number one step and stop at the range's ends") {
    Frames frames;
    auto options = opacity();
    double value = 0.98;
    frames.number(value, options);
    auto result = frames.click(frames.plusX, value, options);
    CHECK(result.changed);
    CHECK(result.done);
    CHECK_THAT(value, WithinAbs(0.99, 1e-9));
    frames.click(frames.plusX, value, options);
    frames.click(frames.plusX, value, options);
    CHECK(value == 1);
    frames.click(frames.minusX, value, options);
    CHECK_THAT(value, WithinAbs(0.99, 1e-9));
}

TEST_CASE("a click on the number types it, and the typed number is kept inside the range") {
    Frames frames;
    auto options = opacity();
    double value = 0.5;
    frames.number(value, options);
    REQUIRE(frames.startTyping(value, options));
    ImGui::GetIO().AddInputCharactersUTF8("0.73");
    frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    auto result = frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    frames.number(value, options);
    CHECK(result.done);
    CHECK_THAT(value, WithinAbs(0.73, 1e-9));

    REQUIRE(frames.startTyping(value, options));
    ImGui::GetIO().AddInputCharactersUTF8("7");
    frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    frames.number(value, options);
    CHECK(value == 1);
}

TEST_CASE("Esc while typing a number keeps the typed number, like every text box") {
    Frames frames;
    auto options = opacity();
    double value = 0.5;
    frames.number(value, options);
    REQUIRE(frames.startTyping(value, options));
    ImGui::GetIO().AddInputCharactersUTF8("0.2");
    frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frames.number(value, options);
    frames.number(value, options);
    CHECK(ImGui::GetActiveID() == 0);
    CHECK_THAT(value, WithinAbs(0.2, 1e-9));
}

TEST_CASE("text that isn't a number leaves the number as it was") {
    Frames frames;
    auto options = opacity();
    double value = 0.4;
    frames.number(value, options);
    REQUIRE(frames.startTyping(value, options));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
    frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    frames.number(value, options);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    frames.number(value, options);
    CHECK(value == 0.4);
}

TEST_CASE("a saved number outside its setting's range comes back inside it") {
    Extensions::ExtensionSetting setting;
    setting.type = Extensions::ExtensionSettingTypes::Float;
    setting.defaultValue = 0.5f;
    setting.value = 3.0f;
    setting.min = 0.0f;
    setting.max = 1.0f;
    Extensions::fitToRange(setting);
    CHECK(std::get<float>(setting.value) == 1.0f);

    setting.type = Extensions::ExtensionSettingTypes::Int;
    setting.defaultValue = 5;
    setting.value = -4;
    setting.min = 1.0f;
    setting.max = 10.0f;
    Extensions::fitToRange(setting);
    CHECK(std::get<int>(setting.value) == 1);
}

namespace {
    // a frame with a window of the given width, draw() runs inside it
    template <typename F>
    void frame(float width, F draw) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, 400));
        ImGui::Begin("page", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        draw();
        ImGui::End();
        ImGui::EndFrame();
    }
}

TEST_CASE("a look's name gives the look back, an unknown name none") {
    for (Controls::Look look : { Controls::Look::Windows, Controls::Look::Material, Controls::Look::Game, Controls::Look::Apple }) {
        CHECK(Controls::LookNamed(Controls::LookName(look)) == look);
    }
    CHECK_FALSE(Controls::LookNamed("Fluent").has_value());
    CHECK_FALSE(Controls::LookNamed("").has_value());
}

TEST_CASE("a named look keeps its own colours whatever the player picked") {
    Controls::SetLook(Controls::Look::Windows, false);
    Controls::Theme game = Controls::GetTheme(Controls::Look::Game, true);
    CHECK(game.look == Controls::Look::Game);
    CHECK(game.dark);
    CHECK(game.gold == 0xD3BC8EFF);
    CHECK(Controls::GetTheme().look == Controls::Look::Windows);
    CHECK_FALSE(Controls::GetTheme().dark);
    CHECK(Controls::GetTheme().text != Controls::GetTheme(Controls::Look::Windows, true).text);
    Controls::SetLook(Controls::Look::Windows, true);
}

TEST_CASE("Apple keeps its own blue and green whatever the Windows accent") {
    Controls::SetAccent(std::array<unsigned int, 7>{ 0xFFB3E0FF, 0xF589C8FF, 0xE35BA9FF, 0xC2185BFF, 0x9C0F49FF, 0x7A0A39FF, 0x55062AFF });
    Controls::Theme dark = Controls::GetTheme(Controls::Look::Apple, true);
    Controls::Theme light = Controls::GetTheme(Controls::Look::Apple, false);
    CHECK(dark.picked == 0x0A84FFFF);
    CHECK(light.picked == 0x007AFFFF);
    CHECK(dark.switchOn == 0x30D158FF);
    CHECK(light.switchOn == 0x34C759FF);
    CHECK_FALSE(dark.switchText);
    Controls::SetAccent(std::nullopt);
}

TEST_CASE("Material You from a purple accent gives the colours Material's own tests give") {
    // the answers of SchemeTonalSpotTests.swift in material-color-utilities,
    // at the commit third_party holds (test3rdPartyLightScheme, ...DarkScheme)
    std::array<unsigned int, 7> shades{};
    shades[3] = 0x6750A4FF;
    Controls::SetAccent(shades);
    Controls::Theme dark = Controls::GetTheme(Controls::Look::Material, true);
    CHECK(dark.accent == 0xCFBDFEFF);
    CHECK(dark.panel == 0x141218F0);
    CHECK(dark.text == 0xE6E0E9FF);
    Controls::Theme light = Controls::GetTheme(Controls::Look::Material, false);
    CHECK(light.accent == 0x65558FFF);
    CHECK(light.panel == 0xFDF7FFF0);
    CHECK(light.text == 0x1D1B20FF);
    // testLightSchemeFromHighChromaColor, the roles the switch and the list use
    shades[3] = 0xFA2BECFF;
    Controls::SetAccent(shades);
    light = Controls::GetTheme(Controls::Look::Material, false);
    CHECK(light.accent == 0x814C77FF);
    CHECK(light.onAccent == 0xFFFFFFFF);
    CHECK(light.accentSoft == 0xFFD7F3FF);
    CHECK(light.picked == 0xF8DAEEFF);
    CHECK(light.pickedText == 0x554050FF);
    CHECK(light.risky == 0xBA1A1AFF);
    CHECK(light.muted == 0x4E444BFF);
    CHECK(light.switchOff == 0x80747BFF);
    CHECK(light.line == 0xD2C2CBFF);
    Controls::SetAccent(std::nullopt);
}

TEST_CASE("Windows 11 takes Windows' accent, a lighter shade in dark and a darker one in light") {
    Controls::SetAccent(std::nullopt);
    CHECK(Controls::GetTheme(Controls::Look::Windows, true).accent == 0x60CDFFFF);
    CHECK(Controls::GetTheme(Controls::Look::Windows, false).accent == 0x005FB8FF);
    // Windows' orange accent, its shades as the registry held them on a PC set to it
    std::array<unsigned int, 7> orange = { 0xFEBD68FF, 0xFB9A44FF, 0xF8741DFF, 0xF7630CFF, 0xE05307FF, 0xA13105FF, 0x691202FF };
    Controls::SetAccent(orange);
    CHECK(Controls::GetTheme(Controls::Look::Windows, true).accent == 0xFB9A44FF);
    CHECK(Controls::GetTheme(Controls::Look::Windows, false).accent == 0xE05307FF);
    // the game look keeps its own
    CHECK(Controls::GetTheme(Controls::Look::Game, true).gold == 0xD3BC8EFF);
    CHECK(Controls::GetTheme(Controls::Look::Game, false).accent == 0x2F78C4FF);
    // Material You moves with it
    unsigned int before = Controls::GetTheme(Controls::Look::Material, true).accent;
    Controls::SetAccent(std::nullopt);
    CHECK(Controls::GetTheme(Controls::Look::Material, true).accent != before);
}

TEST_CASE("a click on a toggle turns it on and off") {
    Frames frames;
    bool on = false;
    ImVec2 middle;
    bool clicked = false;
    auto draw = [&] {
        clicked |= Controls::Toggle("Hide on map close", &on);
        middle = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
    };
    frame(700, draw);
    for (int i = 0; i < 2; i++) {
        clicked = false;
        ImGui::GetIO().AddMousePosEvent(middle.x, middle.y);
        frame(700, draw);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frame(700, draw);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        frame(700, draw);
        CHECK(clicked);
        CHECK(on == (i == 0));
    }
}

TEST_CASE("compact toggles share a row and wrap when it's full") {
    Frames frames;
    bool on[6] = {};
    const char* names[6] = { "Chests", "Oculi", "Domains", "Bosses", "Puzzles", "All" };
    float y[6] = {};
    Controls::ToggleOptions compact;
    compact.compact = true;
    frame(260, [&] {
        for (int i = 0; i < 6; i++) {
            Controls::Toggle(names[i], &on[i], compact);
            y[i] = ImGui::GetItemRectMin().y;
            CHECK(ImGui::GetItemRectMax().x <= 260);
        }
    });
    CHECK(y[1] == y[0]);
    CHECK(y[5] > y[0]);

    // anything else between them starts a new row
    float after = 0, before = 0;
    frame(700, [&] {
        Controls::Toggle("Chests", &on[0], compact);
        before = ImGui::GetItemRectMin().y;
        ImGui::TextUnformatted("Regions");
        Controls::Toggle("Mondstadt", &on[1], compact);
        after = ImGui::GetItemRectMin().y;
    });
    CHECK(after > before);
}

TEST_CASE("a toggle sized by the font is twice as tall with twice the font") {
    Frames frames;
    float normal = 0, big = 0;
    bool on = false;
    frame(700, [&] { Controls::Toggle("Chests", &on); normal = ImGui::GetItemRectSize().y; });
    frame(700, [&] {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 2);
        Controls::Toggle("Chests", &on);
        big = ImGui::GetItemRectSize().y;
        ImGui::PopFont();
    });
    CHECK_THAT(big, WithinAbs(normal * 2, 0.5));
}

TEST_CASE("switches line up at the list's end whatever the name's length") {
    Frames frames;
    bool on[3] = {};
    const char* names[3] = { "Notices", "Look again when a still map changes (floors)",
        "Clicks on parts of what it draws go to it instead of the game, while the map or the menu is open" };
    float left[3] = {}, right[3] = {}, height[3] = {};
    frame(400, [&] {
        for (int i = 0; i < 3; i++) {
            float top = ImGui::GetCursorScreenPos().y;
            Controls::Toggle(names[i], &on[i]);
            left[i] = ImGui::GetItemRectMin().x;
            right[i] = ImGui::GetItemRectMax().x;
            height[i] = ImGui::GetCursorScreenPos().y - top;
        }
    });
    CHECK(left[1] == left[0]);
    CHECK(left[2] == left[0]);
    // the long name wraps onto more lines instead of pushing the switch out of the window
    CHECK(right[2] <= 400);
    CHECK(height[2] > height[0] + ImGui::GetFontSize());
}

TEST_CASE("only the switch takes the click, not its name or the lines under it") {
    Frames frames;
    bool on = false;
    ImVec2 name, under;
    Controls::ToggleOptions options;
    options.lines.push_back({ "It can show you short messages." });
    auto draw = [&] {
        ImVec2 start = ImGui::GetCursorScreenPos();
        Controls::Toggle("Notices", &on, options);
        name = ImVec2(start.x + 4, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
        under = ImVec2(start.x + 4, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y - 4);
    };
    frame(700, draw);
    for (ImVec2 at : { name, under }) {
        ImGui::GetIO().AddMousePosEvent(at.x, at.y);
        frame(700, draw);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frame(700, draw);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        frame(700, draw);
        CHECK_FALSE(on);
    }
}

TEST_CASE("in every look a click on the switch turns it on and off, one on its On or Off text doesn't") {
    for (Controls::Look look : { Controls::Look::Windows, Controls::Look::Material, Controls::Look::Game, Controls::Look::Apple }) {
        for (bool dark : { true, false }) {
            Controls::SetLook(look, dark);
            Frames frames;
            bool on = false;
            ImVec2 track, before;
            auto draw = [&] {
                Controls::Toggle("Hide on map close", &on);
                ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                track = ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2);
                before = ImVec2(min.x - ImGui::GetFontSize(), track.y);
            };
            auto click = [&](ImVec2 at) {
                ImGui::GetIO().AddMousePosEvent(at.x, at.y);
                frame(700, draw);
                ImGui::GetIO().AddMouseButtonEvent(0, true);
                frame(700, draw);
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                frame(700, draw);
            };
            frame(700, draw);
            click(track);
            CHECK(on);
            click(before);
            CHECK(on);
            click(track);
            CHECK_FALSE(on);
        }
    }
    Controls::SetLook(Controls::Look::Windows, true);
}

TEST_CASE("a dotted switch keeps the same place and size as one without the dot") {
    Frames frames;
    bool on = false;
    ImVec2 plainMin, plainMax, dottedMin, dottedMax;
    Controls::ToggleOptions dotted;
    dotted.dotted = true;
    frame(700, [&] { Controls::Toggle("Notices", &on); plainMin = ImGui::GetItemRectMin(); plainMax = ImGui::GetItemRectMax(); });
    frame(700, [&] { Controls::Toggle("Notices", &on, dotted); dottedMin = ImGui::GetItemRectMin(); dottedMax = ImGui::GetItemRectMax(); });
    CHECK(plainMin.x == dottedMin.x);
    CHECK(plainMin.y == dottedMin.y);
    CHECK(plainMax.x == dottedMax.x);
    CHECK(plainMax.y == dottedMax.y);
}

namespace {
    // one text box in a window, and where its parts are
    struct TextBox {
        Frames frames;
        Controls::TextOptions options;
        std::string value;
        bool changed = false;
        ImVec2 box, clear, tick;

        void draw() {
            frame(700, [&] {
                // the label first, the box at the row's end with the X
                // then the tick inside its end, a quarter font apart, inside
                // the row's card
                ImVec2 pad = Controls::RowPadding();
                ImVec2 start(ImGui::GetCursorScreenPos().x + pad.x, ImGui::GetCursorScreenPos().y + pad.y);
                float right = start.x + ImGui::GetContentRegionAvail().x - pad.x * 2;
                float side = ImGui::GetFrameHeight();
                changed |= Controls::Text("Server", value, options);
                tick = ImVec2(right - side / 2, start.y + side / 2);
                clear = ImVec2(tick.x - side - ImGui::GetFontSize() * 0.25f, tick.y);
                box = ImVec2(right - side * 4, start.y + side / 2);
            });
        }
        void clickAt(ImVec2 at) {
            ImGui::GetIO().AddMousePosEvent(at.x, at.y);
            draw();
            ImGui::GetIO().AddMouseButtonEvent(0, true);
            draw();
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            draw();
            draw();
        }
        void key(ImGuiKey k) {
            ImGui::GetIO().AddKeyEvent(k, true);
            draw();
            ImGui::GetIO().AddKeyEvent(k, false);
            draw();
        }
        void type(const char* text) {
            ImGui::GetIO().AddInputCharactersUTF8(text);
            draw();
        }
    };
}

TEST_CASE("with confirm the text counts only once Enter sends it") {
    TextBox t;
    t.options.confirm = true;
    t.value = "old";
    t.draw();
    t.clickAt(t.box);
    REQUIRE(ImGui::GetActiveID() != 0);
    t.key(ImGuiKey_End);
    t.type("er");
    CHECK(t.value == "old");
    CHECK_FALSE(t.changed);
    t.key(ImGuiKey_Enter);
    CHECK(t.value == "older");
    CHECK(t.changed);
}

TEST_CASE("with confirm Esc drops the edit and clicking away keeps it unsent until the tick") {
    TextBox t;
    t.options.confirm = true;
    t.value = "old";
    t.draw();
    t.clickAt(t.box);
    t.key(ImGuiKey_End);
    t.type("x");
    t.key(ImGuiKey_Escape);
    t.draw();
    CHECK(t.value == "old");

    t.clickAt(t.box);
    t.key(ImGuiKey_End);
    t.type("y");
    t.clickAt(ImVec2(600, 300));
    CHECK(ImGui::GetActiveID() == 0);
    CHECK(t.value == "old");
    t.clickAt(t.tick);
    CHECK(t.value == "oldy");
    CHECK(t.changed);
}

TEST_CASE("the x empties the box") {
    TextBox t;
    t.options.clear = true;
    t.value = "chests";
    t.draw();
    // no confirm: the x sits where the tick would be
    t.clickAt(t.tick);
    CHECK(t.value.empty());
    CHECK(t.changed);
}

TEST_CASE("a text setting is saved once the box is left, not on each letter") {
    Frames frames;
    Extensions::ExtensionSetting setting;
    setting.label = "Server";
    setting.type = Extensions::ExtensionSettingTypes::String;
    setting.defaultValue = std::string("");
    setting.value = std::string("");
    bool done = false;
    ImVec2 box;
    auto draw = [&] {
        frame(700, [&] {
            // the box at the row's end, after the label
            ImVec2 start = ImGui::GetCursorScreenPos();
            float right = start.x + ImGui::GetContentRegionAvail().x;
            box = ImVec2(right - ImGui::GetFrameHeight() * 4, start.y + ImGui::GetFrameHeight() / 2);
            done |= Extensions::drawSetting(setting);
        });
    };
    auto clickAt = [&](ImVec2 at) {
        ImGui::GetIO().AddMousePosEvent(at.x, at.y);
        draw();
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        draw();
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        draw();
        draw();
    };
    draw();
    clickAt(box);
    REQUIRE(ImGui::GetActiveID() != 0);
    ImGui::GetIO().AddInputCharactersUTF8("eu");
    draw();
    draw();
    CHECK(std::get<std::string>(setting.value) == "eu");
    CHECK_FALSE(done);
    clickAt(ImVec2(600, 300));
    CHECK(done);
}

namespace {
    // a click at the middle of the item last drawn by draw(), over the next frames
    template <typename F>
    void clickLast(F draw) {
        ImVec2 at;
        auto drawn = [&] { draw(); at = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2,
            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2); };
        frame(700, drawn);
        ImGui::GetIO().AddMousePosEvent(at.x, at.y);
        frame(700, drawn);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frame(700, drawn);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        frame(700, drawn);
    }
}

TEST_CASE("a risky button asks Sure? in its place and counts only on Yes") {
    Frames frames;
    Controls::ButtonOptions risky;
    risky.look = Controls::ButtonOptions::Look::Risky;
    int pressed = 0;
    // the button, and after a click the question; its last item is No
    clickLast([&] { pressed += Controls::Button("Put back backup", risky); });
    CHECK(pressed == 0);
    clickLast([&] { pressed += Controls::Button("Put back backup", risky); });
    CHECK(pressed == 0);

    // asked again, Yes this time: Yes is the item before No
    clickLast([&] { pressed += Controls::Button("Put back backup", risky); });
    ImVec2 yes;
    frame(700, [&] {
        Controls::Button("Put back backup", risky);
        ImGuiContext& g = *ImGui::GetCurrentContext();
        yes = ImVec2(ImGui::GetItemRectMin().x - g.Style.ItemSpacing.x - 4, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
    });
    ImGui::GetIO().AddMousePosEvent(yes.x, yes.y);
    frame(700, [&] { pressed += Controls::Button("Put back backup", risky); });
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame(700, [&] { pressed += Controls::Button("Put back backup", risky); });
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame(700, [&] { pressed += Controls::Button("Put back backup", risky); });
    CHECK(pressed == 1);
}

TEST_CASE("a busy button can't be pressed") {
    Frames frames;
    Controls::ButtonOptions busy;
    busy.busy = true;
    int pressed = 0;
    clickLast([&] { pressed += Controls::Button("Sync now", busy); });
    CHECK(pressed == 0);
    busy.busy = false;
    clickLast([&] { pressed += Controls::Button("Sync now", busy); });
    CHECK(pressed == 1);
}

TEST_CASE("an extension's tab row gives the tab clicked") {
    Frames frames;
    std::vector<std::string> names = { "Markers", "Sync" };
    int open = -1;
    ImVec2 second;
    // the last item the row draws is its last tab
    auto draw = [&] {
        open = Controls::Tabs("##tabs", names);
        second = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
    };
    frame(700, draw);
    frame(700, draw);
    CHECK(open == 0);
    ImGui::GetIO().AddMousePosEvent(second.x, second.y);
    frame(700, draw);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame(700, draw);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame(700, draw);
    frame(700, draw);
    CHECK(open == 1);
}

TEST_CASE("an extension's tab names are smaller than the page's text on a scaled screen, a dot doesn't move them") {
    Frames frames;
    ImGui::GetStyle().FontScaleDpi = 2.0f;
    float tab = 0, dotted = 0, page = 0, text = 0;
    frame(700, [&] {
        page = ImGui::CalcTextSize("Sync").x;
        Controls::Tabs("##plain", { "Sync" });
        tab = ImGui::GetItemRectSize().x;
        Controls::Tabs("##dotted", { "Sync" }, { true });
        dotted = ImGui::GetItemRectSize().x;
        ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.9f);
        text = ImGui::CalcTextSize("Sync").x;
        ImGui::PopFont();
    });
    CHECK(text > 0);
    CHECK(text < page);
    // each tab keeps the room, so a dot coming or going moves nothing
    CHECK(tab > text);
    CHECK(dotted == tab);
}

TEST_CASE("FitLines breaks at spaces into at most the lines asked, then ends in ...") {
    Frames frames;
    frame(700, [&] {
        float width = ImGui::CalcTextSize("Markers and").x + 1;
        bool cut = true;
        auto lines = Controls::FitLines("Check pieces", ImGui::CalcTextSize("Check pieces").x + 1, 2, cut);
        CHECK(lines == std::vector<std::string>{ "Check pieces" });
        CHECK_FALSE(cut);
        lines = Controls::FitLines("Markers and Waypoints", width, 2, cut);
        CHECK(lines == std::vector<std::string>{ "Markers and", "Waypoints" });
        CHECK_FALSE(cut);
        lines = Controls::FitLines("Markers and Waypoints of Teyvat", width, 2, cut);
        REQUIRE(lines.size() == 2);
        CHECK(cut);
        CHECK(lines[1].size() > 3);
        CHECK(lines[1].substr(lines[1].size() - 3) == "...");
        CHECK(ImGui::CalcTextSize(lines[1].c_str()).x <= width);
        // one word wider than the line is broken inside it
        lines = Controls::FitLines("Superlongextensionname", ImGui::CalcTextSize("Superlong").x + 1, 2, cut);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "Superlong");
        CHECK(cut);
    });
}

TEST_CASE("FitLines never cuts a character in two") {
    // every line is whole UTF-8: no line starts or ends inside a character
    auto whole = [](const std::string& line) {
        if (line.empty()) return true;
        if ((line[0] & 0xC0) == 0x80) return false;
        size_t last = Controls::lastChar(line);
        unsigned char lead = line[last];
        size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        return last + length == line.size();
    };
    Frames frames;
    frame(700, [&] {
        std::string word;
        for (int i = 0; i < 30; i++) word += "\xC3\x84";
        float width = ImGui::CalcTextSize("ABCD").x;
        bool cut = false;
        auto lines = Controls::FitLines(word, width, 2, cut);
        REQUIRE(lines.size() == 2);
        CHECK(cut);
        for (const std::string& line : lines) CHECK(whole(line));
        CHECK(whole(lines[1].substr(0, lines[1].size() - 3)));
    });
    CHECK(Controls::lastChar("ab\xC3\x84") == 2);
    CHECK(Controls::lastChar("abc") == 2);
}

TEST_CASE("a header opens and closes on a click and says it changed") {
    Frames frames;
    bool open = true;
    bool changed = false;
    clickLast([&] { changed |= Controls::Header("Regions", open).changed; });
    CHECK(changed);
    CHECK_FALSE(open);
}

TEST_CASE("a section shows its part only once a click on its header opens it") {
    Frames frames;
    int drawn = 0;
    auto section = [&] {
        if (Controls::BeginSection("Advanced")) {
            drawn++;
            ImGui::TextUnformatted("Follow on its own thread");
            Controls::EndSection();
        }
    };
    frame(700, section);
    CHECK(drawn == 0);
    // the header is the last item while closed
    clickLast(section);
    CHECK(drawn > 0);
}

TEST_CASE("a tab added before the open one keeps it open") {
    Frames frames;
    std::vector<std::string> names = { "General", "Permissions" };
    int open = -1;
    ImVec2 second;
    auto draw = [&] {
        open = Controls::Tabs("##tabs", names);
        second = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
    };
    frame(700, draw);
    frame(700, draw);
    ImGui::GetIO().AddMousePosEvent(second.x, second.y);
    frame(700, draw);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame(700, draw);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame(700, draw);
    REQUIRE(open == 1);
    names = { "General", "Notices", "Permissions" };
    frame(700, draw);
    CHECK(open == 2);
}

TEST_CASE("the open tab going away opens the first, not the one now in its place") {
    Frames frames;
    std::vector<std::string> names = { "General", "Notices", "Permissions" };
    int open = -1;
    ImVec2 last;
    auto draw = [&] {
        open = Controls::Tabs("##tabs", names);
        last = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
    };
    frame(700, draw);
    frame(700, draw);
    ImGui::GetIO().AddMousePosEvent(last.x, last.y);
    frame(700, draw);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame(700, draw);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame(700, draw);
    REQUIRE(open == 2);
    names = { "General", "Notices" };
    frame(700, draw);
    CHECK(open == 0);
}

TEST_CASE("a tooltip shows once the mouse rests on its item for 0.4 s") {
    Frames frames;
    ImVec2 at;
    bool shown = false;
    auto draw = [&] {
        ImGui::Button("Sync now");
        at = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) / 2, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2);
        shown = Controls::Tooltip("Gets the markers from HoYoLAB");
    };
    frame(700, draw);
    ImGui::GetIO().AddMousePosEvent(at.x, at.y);
    // 1/60 s a frame: 0.2 s, then past 0.4 s
    for (int i = 0; i < 12; i++) frame(700, draw);
    CHECK(!shown);
    for (int i = 0; i < 16; i++) frame(700, draw);
    CHECK(shown);
    ImGui::GetIO().AddMousePosEvent(at.x, 390);
    frame(700, draw);
    CHECK(!shown);
}

TEST_CASE("the theme's sizes go by the font, Faded lowers only the opacity") {
    Frames frames;
    Controls::Theme normal, big;
    frame(700, [&] { normal = Controls::GetTheme(); });
    ImGui::GetStyle().FontScaleDpi = 2.0f;
    frame(700, [&] { big = Controls::GetTheme(); });
    ImGui::GetStyle().FontScaleDpi = 1.0f;
    CHECK(big.font == normal.font * 2);
    CHECK(big.badge == normal.badge * 2);
    CHECK(big.text == normal.text);
    CHECK(Controls::Faded(0xECE5D8FF, 0.4f) == 0xECE5D866);
    CHECK(Controls::Faded(0x00000050, 2.0f) == 0x00000050);
    CHECK(Controls::Color(0xFF000080) == IM_COL32(255, 0, 0, 128));
}

TEST_CASE("waiting, empty and progress lines take a line each, the bar its own") {
    Frames frames;
    float font = 0, waitingWidth = 0, emptyWidth = 0, barHeight = 0, unknownHeight = 0, barWidth = 0;
    frame(700, [&] {
        font = ImGui::GetFontSize();
        float y = ImGui::GetCursorPosY();
        Controls::Waiting("Waiting for the game");
        // the spinner sits before the text on the same line
        CHECK(ImGui::GetItemRectMin().x > ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x + font * 0.9f);
        waitingWidth = ImGui::GetItemRectSize().x;
        CHECK(ImGui::GetCursorPosY() > y);
        Controls::Empty("No markers yet");
        emptyWidth = ImGui::GetItemRectSize().x;
        Controls::Progress("Downloading 40%", 0.4f);
        barHeight = ImGui::GetItemRectSize().y;
        barWidth = ImGui::GetItemRectSize().x;
        Controls::Progress("Downloading", -1.0f);
        unknownHeight = ImGui::GetItemRectSize().y;
    });
    CHECK(waitingWidth > 0);
    CHECK(emptyWidth > 0);
    CHECK(barHeight == Catch::Approx(font * 0.6f));
    CHECK(unknownHeight == barHeight);
    CHECK(barWidth > 600);
}

TEST_CASE("a field's label comes first, the field fills the row to Reset at its end, the label and Reset above it when the row is too narrow") {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // the label's top and the field's, and the row's right end, at a page width
    struct Row { float labelY, fieldY, right, pageRight; };
    auto draw = [&](float width, bool text) {
        Row row{};
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, 300));
        ImGui::Begin("page", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        // the row's card is padded in from the page
        ImVec2 pad = Controls::RowPadding();
        row.pageRight = window->WorkRect.Max.x - pad.x;
        float top = ImGui::GetCursorScreenPos().y + pad.y;
        double value = 5;
        std::string name = "Mondstadt";
        if (text) {
            Controls::TextOptions options;
            options.confirm = options.taken = true;
            options.defaultValue = "Mondstadt";
            Controls::Text("Region", name, options);
        }
        else {
            NumberOptions options;
            options.min = 0; options.max = 10; options.defaultValue = 5;
            Controls::Number("Opacity", value, options);
        }
        row.right = window->DC.CursorMaxPos.x;
        row.labelY = ImGui::GetItemRectMin().y;
        row.fieldY = top;
        ImGui::End();
        ImGui::EndFrame();
        return row;
    };
    float side = 0;
    for (bool text : { false, true }) {
        draw(700, text);
        Row wide = draw(700, text);
        side = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2;
        // the last thing drawn is Reset, on the field's row
        CHECK(wide.labelY < wide.fieldY + side / 2);
        // the label first, the field filling the row, Reset at the page's edge
        CHECK(wide.right <= wide.pageRight + 1);
        CHECK(wide.right >= wide.pageRight - 1);
        draw(200, text);
        Row narrow = draw(200, text);
        // the last thing drawn is the field's row, a text row under the label and Reset
        CHECK(narrow.labelY >= narrow.fieldY + ImGui::GetFontSize());
        CHECK(narrow.right <= narrow.pageRight + 1);
    }
    ImGui::DestroyContext(context);
}

TEST_CASE("every setting row ends at the same edge, the rows without Reset keeping its room") {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // each row's last piece's right end
    float reset = 0, wholePlus = 0, toggle = 0, box = 0, pageRight = 0;
    auto draw = [&](float width) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, 500));
        ImGui::Begin("page", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        pageRight = ImGui::GetCurrentWindow()->WorkRect.Max.x - Controls::RowPadding().x;
        double value = 5, count = 3;
        NumberOptions slider;
        slider.min = 0; slider.max = 10; slider.defaultValue = 5;
        Controls::Number("Opacity", value, slider);
        reset = ImGui::GetItemRectMax().x;
        NumberOptions whole;
        whole.min = 1; whole.max = 9; whole.whole = whole.field = true;
        Controls::Number("Markers per row", count, whole);
        wholePlus = ImGui::GetItemRectMax().x;
        bool on = true, ticked = true;
        Controls::Toggle("Show markers", &on);
        toggle = ImGui::GetItemRectMax().x;
        Controls::ToggleOptions checkbox;
        checkbox.checkbox = true;
        Controls::Toggle("As a checkbox", &ticked, checkbox);
        box = ImGui::GetItemRectMax().x;
        ImGui::End();
        ImGui::EndFrame();
    };
    // the second frame, once the window knows a row has Reset
    draw(700);
    draw(700);
    float room = ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    CHECK_THAT(reset, WithinAbs(pageRight, 1));
    CHECK_THAT(wholePlus, WithinAbs(pageRight - room, 1));
    CHECK_THAT(toggle, WithinAbs(pageRight - room, 1));
    CHECK_THAT(box, WithinAbs(pageRight - room, 1));
    // too narrow, Reset goes above with the label and the rows end at the card's edge
    draw(260);
    draw(260);
    CHECK_THAT(wholePlus, WithinAbs(pageRight, 1));
    CHECK_THAT(toggle, WithinAbs(pageRight, 1));
    CHECK_THAT(box, WithinAbs(pageRight, 1));
    ImGui::DestroyContext(context);
}

TEST_CASE("a row's card is as tall as the row, whatever the window drew lower down before it") {
    Frames frames;
    bool on = true;
    float alone = 0, besideTall = 0;
    frame(700, [&] {
        ImVec2 top = ImGui::GetCursorPos();
        Controls::Toggle("Show markers", &on);
        alone = ImGui::GetCursorPosY() - top.y;
        // a tall picture, then the cursor back up beside it
        ImGui::Dummy(ImVec2(40, 400));
        ImVec2 under = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(under.x, under.y - 400));
        float rowTop = ImGui::GetCursorPosY();
        Controls::Toggle("Show markers again", &on);
        besideTall = ImGui::GetCursorPosY() - rowTop;
    });
    CHECK_THAT(besideTall, WithinAbs(alone, 1));
}

TEST_CASE("a small switch comes after its name, like every piece") {
    Frames frames;
    bool on = false;
    float start = 0, track = 0;
    Controls::ToggleOptions compact;
    compact.compact = true;
    frame(700, [&] {
        start = ImGui::GetCursorScreenPos().x;
        Controls::Toggle("Chests", &on, compact);
        track = ImGui::GetItemRectMin().x;
    });
    CHECK(track >= start + ImGui::CalcTextSize("Chests").x);
}
