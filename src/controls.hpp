#pragma once
#include <imgui.h>
#include <array>
#include <optional>
#include <string>
#include <vector>

// The menu's pieces: every number, on/off, text box and button in the menu,
// the overlay's own and the extensions' alike. Sizes go by the font.
namespace Controls {
    // How the menu and every piece look, picked in the menu under Appearance.
    // Material You takes its colours from the Windows accent, as Android does
    // from the wallpaper. Game is the game's own colours: light text, dark
    // blue panels, gold lines. Apple is macOS and iOS settings: grouped
    // lists, a green switch, Apple's own blue whatever the Windows accent.
    enum class Look { Windows, Material, Game, Apple };
    // the name in settings.json and Lua, and back; an unknown name gives none
    const char* LookName(Look look);
    std::optional<Look> LookNamed(const std::string& name);

    // The colours and sizes every piece uses, and extensions get with
    // GetTheme. Colours are 0xRRGGBBAA.
    struct Theme {
        Look look = Look::Windows;
        bool dark = true;
        unsigned int text = 0xFFFFFFFF;
        // lines under a name, hints
        unsigned int muted = 0xC5C5C5FF;
        // the faintest text: when something was last used
        unsigned int faint = 0xA0A0A0FF;
        // a window's ground, the page on it, a card on the page
        unsigned int panel = 0x202020F0;
        unsigned int page = 0x272727FF;
        unsigned int inside = 0x2E2E2EFF;
        unsigned int cardLine = 0x1C1C1CFF;
        unsigned int line = 0xFFFFFF18;
        // the open tab and the picked page in the list, and the page's name on it
        unsigned int picked = 0x2D2D2DFF;
        unsigned int pickedText = 0xFFFFFFFF;
        unsigned int button = 0x373737FF;
        unsigned int buttonLine = 0x444444FF;
        unsigned int gold = 0x60CDFFFF;
        unsigned int accent = 0x60CDFFFF;
        // text on the accent, and a softer accent (Material's knob under the mouse)
        unsigned int onAccent = 0x000000FF;
        unsigned int accentSoft = 0x60CDFFFF;
        unsigned int badgeFill = 0x1F3A4AFF;
        unsigned int warning = 0xFCE100FF;
        unsigned int risky = 0xFF99A4FF;
        unsigned int success = 0x6CCB5FFF;
        unsigned int done = 0x9A9A9AFF;
        unsigned int shadow = 0x00000050;
        // the switch's outline and knob while off, and its track
        unsigned int switchOff = 0x9A9A9AFF;
        unsigned int switchTrack = 0x00000000;
        // the switch's fill while on, none: the accent
        unsigned int switchOn = 0;
        // how much of something done or out of reach still shows
        float faded = 0.4f;
        // in pixels, from the font size
        float font = 13;
        float gap = 8;
        float rounding = 6;
        float badge = 40;
        // the switch's track in font sizes, its knob by the track's height:
        // resting off and on, under the mouse, and its width while held
        // (Material You: its size while held, and no growing under the mouse)
        float switchWidth = 2.86f;
        float switchHeight = 1.43f;
        float knob = 0.6f;
        float knobOn = 0.6f;
        float knobHover = 0.7f;
        float knobHeld = 0.85f;
        // On or Off written before the switch
        bool switchText = true;
    };
    // the player's look at the font size now
    Theme GetTheme();
    // a named look, for pieces that keep their own whatever the player picked
    Theme GetTheme(Look look, bool dark);
    // the player's look; ImGui's colours follow at the next ApplyStyle
    void SetLook(Look look, bool dark);
    // Windows' accent in its 7 shades (Helpers::accentShades), none for
    // Windows' default blue. Windows 11 and Material You follow it.
    void SetAccent(const std::optional<std::array<unsigned int, 7>>& shades);
    // ImGui's colours and roundings from the player's look. scale: the
    // screen's DPI scale the sizes are multiplied by.
    void ApplyStyle(float scale);
    // 0xRRGGBBAA as ImGui's colours
    ImU32 Color(unsigned int rgba);
    ImVec4 ColorVec(unsigned int rgba);
    // the colour with its opacity times amount
    unsigned int Faded(unsigned int rgba, float amount);

    struct NumberOptions {
        // an attention dot in the slider's corner, or the number box's
        bool dotted = false;
        std::optional<double> min, max;
        // how far - and + move it, none: 1 for whole numbers, else a hundredth of the range
        std::optional<double> step;
        // shows Reset, greyed out while the value is this
        std::optional<double> defaultValue;
        // a typed box only, no slider
        bool field = false;
        bool whole = false;
    };

    struct Result {
        // the value moved this frame
        bool changed = false;
        // the player is done with it (let go of the slider, clicked, typed and left the box)
        bool done = false;
    };

    // what - and + add, and the typed number's decimals
    double StepFor(const NumberOptions& options);
    int DecimalsFor(const NumberOptions& options);
    // inside the range, and whole when it should be
    double Fit(double value, const NumberOptions& options);

    // A slider, then the number to click and type or drag, - and +, the label,
    // Reset. Without a range, or with field, the slider is left out. Esc while
    // typing keeps the typed number, like every text box.
    Result Number(const char* label, double& value, const NumberOptions& options);

    struct ToggleLine {
        std::string text;
        bool grey = true;
    };

    struct ToggleOptions {
        bool checkbox = false;
        // an attention dot on the switch's corner
        bool dotted = false;
        // a smaller switch after the name, a row of them wraps by itself
        bool compact = false;
        // drawn before the name, 0 for none
        ImTextureID icon = 0;
        // under the name, wrapped beside the switch (not with compact)
        std::vector<ToggleLine> lines;
    };

    // On/off: the name, then a small track with a round knob, filled while
    // on, every switch of a list lined up at its end, sized by the look; the
    // Windows 11 look writes On or Off before it. Only the switch takes the
    // click. True when it was clicked.
    bool Toggle(const char* label, bool* value, const ToggleOptions& options = {});
    struct TextOptions {
        // an attention dot in the box's corner
        bool dotted = false;
        // grey in the empty box
        std::string hint;
        // shows Reset, greyed out while the text is this
        std::optional<std::string> defaultValue;
        // an x in the box that empties it, shown while there's text
        bool clear = false;
        // the text only counts once the tick or Enter sends it, Esc drops it
        bool confirm = false;
        // with confirm: sent text is greyed with a tick in the box
        bool taken = false;
        bool backToGame = true;
        size_t maxBytes = 4096;
    };

    // A text box, the label, Reset. True when value changed. With confirm,
    // value only changes when the edit is sent; an edit left by clicking
    // elsewhere stays in the box, unsent.
    bool Text(const char* label, std::string& value, const TextOptions& options = {});
    struct ButtonOptions {
        enum class Look { Plain, Main, Risky };
        // Main: filled, for what a section is mostly for. Risky: red, asks
        // "Sure?" in its place before it counts.
        Look look = Look::Plain;
        // greyed out and not pressable while its job runs
        bool busy = false;
        // shown while busy, none: the label and "..."
        std::string busyLabel;
        ImVec2 size = ImVec2(0, 0);
        // an attention dot in its corner
        bool dotted = false;
    };

    // True when pressed, for Risky once Yes is pressed.
    bool Button(const char* label, const ButtonOptions& options = {});
    // how big Button draws the label, for lining it up before it's drawn
    ImVec2 ButtonSize(const char* label);

    // The label, then the picked name at the end of the row, lined up with
    // the switches; a click opens the list. True when another was picked.
    bool Choice(const char* label, int& picked, const std::vector<std::string>& names);
    // A setting's name at the start of a row, in the look's type, lined up
    // with the boxes after it; and how wide it draws.
    void RowName(const char* text);
    float RowNameWidth(const char* text);
    // The look's drop-down box with the picked name. While its list is open
    // it gives true, then the names (ImGui::Selectable) and EndDropDown.
    // rows: how many names, more than 8 scroll.
    bool BeginDropDown(const char* id, const char* preview, float width, int rows);
    void EndDropDown();
    // A setting row's card padding, sides and top: Number, Text, Toggle and
    // Choice each sit on a card, every one ending at the same edge, Reset's
    // room kept on the rows without it.
    ImVec2 RowPadding();
    // A tab row: plain names a little smaller than the page's text, the open
    // one marked by the look (Windows 11: a short accent bar under it, Game:
    // gold with a gold line, Material You: the accent with an accent line).
    // Gives the index of the open tab, kept by its
    // name, 0 when there are none. dotted: the tabs with an attention dot, by
    // index; each tab keeps room for one.
    int Tabs(const char* id, const std::vector<std::string>& names, const std::vector<bool>& dotted = {});
    // The same choice as a segmented button, for views inside a page (an
    // extension's ImGUI_Tabs), so they're told apart from the page's own tabs:
    // one framed row, the open piece filled (Material You: its tonal colour,
    // the others the accent). Pieces that don't fit go on a row under it.
    int Segments(const char* id, const std::vector<std::string>& names, const std::vector<bool>& dotted = {});

    // A header that opens and closes the part under it: a card row with the
    // name and a chevron at its end. open is its state, true while it's open;
    // the result says when the player changed it.
    Result Header(const char* label, bool& open, bool dotted = false);
    // A header like Header with the part under it in a card that meets it.
    // While open it gives true, then EndSection goes after the part.
    bool BeginSection(const char* label, bool defaultOpen = false, bool dotted = false);
    // BeginSection with the open state kept by the caller. While open,
    // EndSection goes after the part.
    Result BeginHeader(const char* label, bool& open, bool dotted = false);
    void EndSection();
    // A page's name at its top, larger than its text.
    void PageTitle(const char* text);
    // the look's bold font for page names, none: the text's own
    void SetTitleFont(ImFont* font);
    // A page in the menu's list: its icon (none: the name's first letter in a
    // tile), the name on up to two lines; the picked one filled, with a short
    // accent bar at its start (Material You: a filled pill, no bar; Apple:
    // filled blue, the icon on a coloured tile). tile: Apple's tile colour,
    // none: one of Apple's colours by the name. True when clicked.
    bool PageButton(const char* label, bool picked, ImTextureID icon = 0, bool dotted = false, unsigned int tile = 0);
    // A small filled tag after the item drawn last, like Official.
    void Tag(const char* text);

    // The attention dot in the top right corner of the item drawn last.
    void Dot();
    // The attention dot centred on the top right corner of a framed piece,
    // half over its edge. rounding: the frame's corner rounding.
    void CornerDot(ImVec2 min, ImVec2 max, float rounding = 0);
    // the room a dot takes in a corner, across
    float DotRoom();
    // The text broken at spaces into lines no wider than width, at most
    // maxLines; cut is set when the last one ends in "..." because the rest
    // didn't fit.
    std::vector<std::string> FitLines(const std::string& text, float width, int maxLines, bool& cut);
    // where the text's last UTF-8 character starts
    size_t lastChar(const std::string& text);

    // Where a tooltip goes: by the mouse; under the item, ending at its right
    // edge, e.g. off the game's buttons; left of the item at its middle, off
    // the items above and below it in a column.
    enum class TipAt { Mouse, Under, Left };

    // A box with text for the item drawn last, once the mouse rests on it
    // 0.4 s. Long text wraps. True while it shows.
    bool Tooltip(const char* text, TipAt at = TipAt::Mouse);

    // A muted line with a turning spinner in front, for something under way.
    void Waiting(const char* text);
    // A muted line for a list or a page with nothing in it yet.
    void Empty(const char* text);
    // The text, then a bar of how far it got, 0 to 1. Below 0 when that
    // isn't known: the spinner in front, the bar runs back and forth.
    void Progress(const char* text, float fraction);

    struct BadgeOptions {
        // the theme's text colour when none
        std::optional<unsigned int> ring;
        // drawn at the theme's faded opacity, its ring the done grey
        bool faded = false;
    };
    // A round badge like the game's map icons: a soft shadow, a light ring,
    // a dark inside and the icon in the middle. radius in pixels. Badges and
    // bubbles are in the Game look whatever the player picked.
    void Badge(ImDrawList* draw, ImVec2 center, float radius, ImTextureID icon, const BadgeOptions& options = {});
    // A small round bubble with a dark rim and short text in it, a count or
    // a sign. fill none: the theme's gold.
    void Bubble(ImDrawList* draw, ImVec2 center, float radius, const char* text, std::optional<unsigned int> fill = {});
}
