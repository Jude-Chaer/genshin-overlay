#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include "notices.hpp"
#include "settings.hpp"

using Notices::Kind;

namespace {
    struct Frames {
        ImGuiContext* context = ImGui::CreateContext();
        Frames() {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1920, 1080);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }
        ~Frames() { ImGui::DestroyContext(context); }

        int inMenu(double now) {
            ImGui::NewFrame();
            int drawn = Notices::DrawInMenu(now, ImVec2(60, 60), ImVec2(720, 520));
            ImGui::EndFrame();
            return drawn;
        }
        int overGame(double now) {
            ImGui::NewFrame();
            int drawn = Notices::DrawOverGame(now);
            ImGui::EndFrame();
            return drawn;
        }
    };

    void fresh() {
        std::filesystem::remove(Settings::file);
        Notices::Clear();
    }
}

TEST_CASE("a notice repeating the owner's newest is dropped, an older one comes back") {
    fresh();
    CHECK(Notices::Add("markers", "Markers", Kind::Plain, "Synced", 0) != 0);
    CHECK(Notices::Add("markers", "Markers", Kind::Plain, "Synced", 1) == 0);
    // another owner's newest doesn't count
    CHECK(Notices::Add("example", "Example", Kind::Plain, "Synced", 1) != 0);
    CHECK(Notices::Add("markers", "Markers", Kind::Error, "Sync failed", 2) != 0);
    CHECK(Notices::Add("markers", "Markers", Kind::Plain, "Synced", 3) != 0);
    CHECK(Notices::Of("markers").size() == 3);
    CHECK(Notices::Add("markers", "Markers", Kind::Plain, "", 4) == 0);
    fresh();
}

TEST_CASE("the same notice again shows once the first has faded, an error over the game once its time is up") {
    fresh();
    Frames frames;
    int first = Notices::Add("markers", "Markers", Kind::Plain, "Synced", 0);
    frames.inMenu(1);
    CHECK(Notices::Add("markers", "Markers", Kind::Plain, "Synced", 2) == 0);
    CHECK(Notices::Add("markers", "Markers", Kind::Plain, "Synced", 1 + Notices::FadeAfter) != 0);
    CHECK(Notices::Of("markers").size() == 2);
    CHECK(Notices::Of("markers").back()->id == first);

    // the menu closed: the error waits, and comes back over the game
    Notices::Add("markers", "Markers", Kind::Error, "Couldn't save", 20);
    CHECK(Notices::OverGameShowing(21));
    CHECK(Notices::Add("markers", "Markers", Kind::Error, "Couldn't save", 22) == 0);
    CHECK_FALSE(Notices::OverGameShowing(20 + Notices::OverGameFor));
    CHECK(Notices::Add("markers", "Markers", Kind::Error, "Couldn't save", 60) == 0);
    CHECK(Notices::OverGameShowing(61));
    CHECK(Notices::Of("markers").size() == 3);
    fresh();
}

TEST_CASE("each owner keeps its last 10, newest first, and long text is cut") {
    fresh();
    for (int i = 0; i < 14; i++) Notices::Add("markers", "Markers", Kind::Plain, "Synced " + std::to_string(i), i);
    Notices::Add("example", "Example", Kind::Plain, "Hello", 20);
    auto kept = Notices::Of("markers");
    REQUIRE(kept.size() == Notices::KeptPerOwner);
    CHECK(kept.front()->text == "Synced 13");
    CHECK(kept.back()->text == "Synced 4");
    CHECK(Notices::Of("example").size() == 1);

    Notices::Add("example", "Example", Kind::Plain, std::string(500, 'a'), 21);
    CHECK(Notices::Of("example").front()->text.size() == Notices::MaxText);
    fresh();
}

TEST_CASE("a closed tip never comes back, not after a restart either") {
    fresh();
    int first = Notices::Add("markers", "Markers", Kind::Tip, "Right-click a marker to hide it.", 0, "hide");
    REQUIRE(first != 0);
    // still there: not again, by id even with other text
    CHECK(Notices::Add("markers", "Markers", Kind::Tip, "Right-click to hide.", 1, "hide") == 0);
    Notices::Close(first);
    CHECK(Notices::Add("markers", "Markers", Kind::Tip, "Right-click a marker to hide it.", 2, "hide") == 0);
    // another tip, and another extension's tip of the same id
    CHECK(Notices::Add("markers", "Markers", Kind::Tip, "Drag to move.", 3, "drag") != 0);
    CHECK(Notices::Add("example", "Example", Kind::Tip, "Hide it.", 3, "hide") != 0);

    // the closed ids are in settings.json, read again after the restart
    Notices::Clear();
    CHECK(Settings::LoadClosedTips("markers") == std::set<std::string>{ "hide" });
    CHECK(Notices::Add("markers", "Markers", Kind::Tip, "Right-click a marker to hide it.", 0, "hide") == 0);
    CHECK(Notices::Add("markers", "Markers", Kind::Tip, "Drag to move.", 0, "drag") != 0);
    fresh();
}

TEST_CASE("the menu shows the newest 3, each fades 8 s after the menu first shows it") {
    fresh();
    Frames frames;
    for (int i = 0; i < 4; i++) Notices::Add("markers", "Markers", Kind::Plain, "Synced " + std::to_string(i), i);
    // added with the menu closed, they wait for it
    CHECK(frames.inMenu(100) == 3);
    CHECK(frames.inMenu(107.9) == 3);
    // the oldest of the 4 shows once the first ones are gone
    CHECK(frames.inMenu(108.1) == 1);
    CHECK(frames.inMenu(116.2) == 0);
    // they stay on the page
    CHECK(Notices::Of("markers").size() == 4);

    int id = Notices::Add("markers", "Markers", Kind::Done, "Saved", 120);
    CHECK(frames.inMenu(120) == 1);
    Notices::Close(id);
    CHECK(frames.inMenu(121) == 0);
    fresh();
}

TEST_CASE("only errors show over the game, for 5 s, and not once the menu showed them") {
    fresh();
    Frames frames;
    Notices::Add("markers", "Markers", Kind::Plain, "Synced", 0);
    Notices::Add("markers", "Markers", Kind::Tip, "Drag to move.", 0, "drag");
    CHECK_FALSE(Notices::OverGameShowing(0));
    CHECK(frames.overGame(0) == 0);

    Notices::Add("markers", "Markers", Kind::Error, "Sync failed", 10);
    CHECK(Notices::OverGameShowing(14.9));
    CHECK(frames.overGame(14.9) == 1);
    CHECK_FALSE(Notices::OverGameShowing(15.1));

    Notices::Add("example", "Example", Kind::Error, "No connection", 20);
    frames.inMenu(21);
    CHECK_FALSE(Notices::OverGameShowing(21.5));
    fresh();
}

TEST_CASE("the over-game box never takes the mouse") {
    fresh();
    Frames frames;
    Notices::Add("", "", Kind::Error, "Couldn't save the settings", 0);
    frames.overGame(0);
    ImGuiWindow* window = ImGui::FindWindowByName("##noticesOverGame");
    REQUIRE(window);
    CHECK((window->Flags & ImGuiWindowFlags_NoInputs) == ImGuiWindowFlags_NoInputs);
    fresh();
}

TEST_CASE("the menu's notices are drawn over the menu while the menu has focus") {
    fresh();
    Frames frames;
    Notices::Add("markers", "Markers", Kind::Plain, "Synced", 0);
    auto drawn = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(60, 60));
        ImGui::SetNextWindowSize(ImVec2(720, 520));
        ImGui::Begin("Menu");
        ImGui::SetWindowFocus();
        ImGui::End();
        Notices::DrawInMenu(1, ImVec2(60, 60), ImVec2(720, 520));
        ImGui::EndFrame();
    };
    drawn();
    drawn();
    ImVector<ImGuiWindow*>& order = ImGui::GetCurrentContext()->Windows;
    int menu = -1, box = -1;
    for (int i = 0; i < order.Size; i++) {
        if (std::string(order[i]->Name) == "Menu") menu = i;
        if (std::string(order[i]->Name) == "##notices") box = i;
    }
    REQUIRE(menu >= 0);
    REQUIRE(box >= 0);
    CHECK(box > menu);
    fresh();
}

TEST_CASE("a notice's x sits in its card's top corner and closes it") {
    fresh();
    Frames frames;
    int id = Notices::Add("markers", "Markers", Kind::Error, "Couldn't reach HoYoLAB.", 0);
    frames.inMenu(1);
    frames.inMenu(1);
    ImGuiWindow* box = ImGui::FindWindowByName("##notices");
    REQUIRE(box != nullptr);
    float font = ImGui::GetFontSize();
    float pad = font * 0.6f, closeSize = font * 1.2f;
    ImVec2 x(box->Pos.x + box->Size.x - pad * 0.5f - closeSize / 2, box->Pos.y + pad * 0.5f + closeSize / 2);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x.x, x.y);
    frames.inMenu(1);
    io.AddMouseButtonEvent(0, true);
    frames.inMenu(1);
    io.AddMouseButtonEvent(0, false);
    frames.inMenu(1);
    CHECK(Notices::Of("markers")[0]->id == id);
    CHECK(Notices::Of("markers")[0]->closed);
    CHECK(frames.inMenu(1) == 0);
    fresh();
}
