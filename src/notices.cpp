#include "notices.hpp"
#include <imgui_internal.h>
#include "controls.hpp"
#include "settings.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace Notices {
    static std::deque<Notice> notices;
    static int nextId = 1;
    // closed tips per owner, read from settings.json the first time
    static std::map<std::string, std::set<std::string>> closedTips;

    static std::set<std::string>& closedOf(const std::string& owner) {
        auto found = closedTips.find(owner);
        if (found != closedTips.end()) return found->second;
        std::set<std::string> saved;
        if (!owner.empty()) saved = Settings::LoadClosedTips(owner);
        return closedTips[owner] = saved;
    }

    int Add(const std::string& owner, const std::string& from, Kind kind, const std::string& text, double now, const std::string& tip,
        const std::string& actionLabel, std::function<void()> action) {
        if (text.empty()) return 0;
        std::string shown = text.substr(0, MaxText);
        std::string tipId = kind == Kind::Tip ? (tip.empty() ? shown : tip) : "";
        if (kind == Kind::Tip) {
            if (closedOf(owner).count(tipId)) return 0;
            for (const Notice& notice : notices) {
                if (notice.owner == owner && notice.kind == Kind::Tip && notice.tip == tipId && !notice.closed) return 0;
            }
        }
        for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
            if (it->owner != owner) continue;
            bool same = it->text == shown && it->kind == kind && !it->closed;
            if (same && it->shownAt < 0) {
                if (kind == Kind::Error && now - it->addedAt >= OverGameFor) it->addedAt = now;
                return 0;
            }
            if (same && now - it->shownAt < FadeAfter) return 0;
            break;
        }

        Notice notice;
        notice.id = nextId++;
        notice.owner = owner;
        notice.from = from;
        notice.kind = kind;
        notice.text = shown;
        notice.tip = tipId;
        notice.addedAt = now;
        notice.actionLabel = actionLabel;
        notice.action = std::move(action);
        notices.push_back(std::move(notice));

        size_t kept = 0;
        for (auto it = notices.rbegin(); it != notices.rend();) {
            if (it->owner == owner && ++kept > KeptPerOwner) it = std::make_reverse_iterator(notices.erase(std::next(it).base()));
            else ++it;
        }
        return notices.back().id;
    }

    void Close(int id) {
        for (Notice& notice : notices) {
            if (notice.id != id || notice.closed) continue;
            notice.closed = true;
            if (notice.kind == Kind::Tip && !notice.owner.empty()) {
                std::set<std::string>& closed = closedOf(notice.owner);
                closed.insert(notice.tip);
                Settings::SaveClosedTips(notice.owner, closed);
            }
        }
    }

    std::vector<const Notice*> Of(const std::string& owner) {
        std::vector<const Notice*> found;
        for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
            if (it->owner == owner) found.push_back(&*it);
        }
        return found;
    }

    static bool overGame(const Notice& notice, double now) {
        return notice.kind == Kind::Error && !notice.closed && notice.shownAt < 0 && now - notice.addedAt < OverGameFor;
    }

    bool OverGameShowing(double now) {
        return std::any_of(notices.begin(), notices.end(), [now](const Notice& notice) { return overGame(notice, now); });
    }

    static unsigned int kindColor(Kind kind) {
        Controls::Theme theme = Controls::GetTheme();
        switch (kind) {
        case Kind::Error: return theme.risky;
        case Kind::Tip: return theme.accent;
        case Kind::Done: return theme.success;
        default: return theme.gold;
        }
    }

    // the kind's round icon: ! error, i tip, a tick done, a dot plain
    static void kindIcon(ImDrawList* draw, ImVec2 center, float radius, Kind kind) {
        draw->AddCircleFilled(center, radius, ImGui::GetColorU32(Controls::Color(kindColor(kind))));
        ImU32 mark = ImGui::GetColorU32(Controls::Color(Controls::GetTheme().inside));
        float thick = std::max(1.5f, radius * 0.22f);
        float dot = thick * 0.6f;
        switch (kind) {
        case Kind::Error:
            draw->AddLine(ImVec2(center.x, center.y - radius * 0.5f), ImVec2(center.x, center.y + radius * 0.1f), mark, thick);
            draw->AddCircleFilled(ImVec2(center.x, center.y + radius * 0.45f), dot, mark);
            break;
        case Kind::Tip:
            draw->AddCircleFilled(ImVec2(center.x, center.y - radius * 0.45f), dot, mark);
            draw->AddLine(ImVec2(center.x, center.y - radius * 0.1f), ImVec2(center.x, center.y + radius * 0.5f), mark, thick);
            break;
        case Kind::Done: {
            ImVec2 tick[3] = { ImVec2(center.x - radius * 0.45f, center.y), ImVec2(center.x - radius * 0.1f, center.y + radius * 0.35f),
                ImVec2(center.x + radius * 0.45f, center.y - radius * 0.35f) };
            draw->AddPolyline(tick, 3, mark, ImDrawFlags_None, thick);
            break;
        }
        default:
            draw->AddCircleFilled(center, radius * 0.3f, mark);
        }
    }

    // an x of two lines in the middle of a square button, true when pressed
    static bool closeButton(ImVec2 at, float size) {
        ImGui::SetCursorScreenPos(at);
        bool pressed = ImGui::InvisibleButton("close", ImVec2(size, size));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (ImGui::IsItemHovered()) draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), ImGui::GetColorU32(ImGuiCol_ButtonHovered), size * 0.2f);
        ImVec2 center(at.x + size / 2, at.y + size / 2);
        float arm = size * 0.22f;
        ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
        float thick = std::max(1.0f, size * 0.08f);
        draw->AddLine(ImVec2(center.x - arm, center.y - arm), ImVec2(center.x + arm, center.y + arm), color, thick);
        draw->AddLine(ImVec2(center.x - arm, center.y + arm), ImVec2(center.x + arm, center.y - arm), color, thick);
        return pressed;
    }

    // a card: the kind's icon, who sent it in small muted text over the
    // message, the action button under it, the x in the top corner
    static void drawNotice(Notice& notice, bool closable, bool named) {
        ImGui::PushID(notice.id);
        Controls::Theme theme = Controls::GetTheme();
        float font = ImGui::GetFontSize();
        float pad = font * 0.6f;
        float icon = font * 0.75f;
        float closeSize = font * 1.2f;
        float textWidth = font * 20;
        float width = pad + icon * 2 + pad + textWidth + pad;
        ImVec2 top = ImGui::GetCursorScreenPos();
        float textX = top.x + pad + icon * 2 + pad;
        float right = top.x + width;

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        float y = top.y + pad;
        if (named && !notice.from.empty()) {
            ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.85f);
            bool cut = false;
            float room = right - (closable ? closeSize + pad * 0.5f : pad) - textX;
            std::vector<std::string> name = Controls::FitLines(notice.from, room, 1, cut);
            ImGui::SetCursorScreenPos(ImVec2(textX, y));
            ImGui::PushStyleColor(ImGuiCol_Text, Controls::ColorVec(theme.muted));
            ImGui::TextUnformatted(name.empty() ? "" : name[0].c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            y = ImGui::GetItemRectMax().y + font * 0.1f;
        }
        // with no name over it, a line of text level with the icon
        else y += std::max(0.0f, icon - ImGui::GetTextLineHeight() / 2);
        ImGui::SetCursorScreenPos(ImVec2(textX, y));
        float room = named || !closable ? textWidth : textWidth - closeSize;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + room);
        ImGui::TextUnformatted(notice.text.c_str());
        ImGui::PopTextWrapPos();
        float bottom = ImGui::GetItemRectMax().y;
        if (notice.action) {
            ImGui::SetCursorScreenPos(ImVec2(textX, bottom + font * 0.3f));
            if (Controls::Button(notice.actionLabel.c_str())) {
                notice.action();
                Close(notice.id);
            }
            bottom = ImGui::GetItemRectMax().y;
        }
        if (closable && closeButton(ImVec2(right - pad * 0.5f - closeSize, top.y + pad * 0.5f), closeSize)) Close(notice.id);
        bottom = std::max(bottom, top.y + pad + icon * 2) + pad;

        draw->ChannelsSetCurrent(0);
        ImVec2 end(right, bottom);
        draw->AddRectFilled(top, end, ImGui::GetColorU32(Controls::Color((theme.panel & 0xFFFFFF00) | 0xF0)), theme.rounding);
        draw->AddRect(top, end, ImGui::GetColorU32(Controls::Color(theme.line)), theme.rounding);
        kindIcon(draw, ImVec2(top.x + pad + icon, top.y + pad + icon), icon, notice.kind);
        draw->ChannelsMerge();

        ImGui::SetCursorScreenPos(top);
        ImGui::Dummy(ImVec2(width, bottom - top.y));
        ImGui::PopID();
    }

    static const ImGuiWindowFlags boxFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    int DrawInMenu(double now, ImVec2 menuPos, ImVec2 menuSize) {
        std::vector<Notice*> fading;
        for (auto it = notices.rbegin(); it != notices.rend() && fading.size() < MaxShown; ++it) {
            if (it->closed) continue;
            if (it->shownAt >= 0 && now - it->shownAt >= FadeAfter) continue;
            fading.push_back(&*it);
        }
        if (fading.empty()) return 0;

        float pad = Controls::GetTheme().gap * 2;
        ImGui::SetNextWindowPos(ImVec2(menuPos.x + menuSize.x - pad, menuPos.y + menuSize.y - pad), ImGuiCond_Always, ImVec2(1, 1));
        // only the cards show
        ImGui::SetNextWindowBgAlpha(0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##notices", nullptr, boxFlags);
        ImGui::PopStyleVar(2);
        // over the menu, which has focus
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        // the oldest on top, the newest by the corner
        for (auto it = fading.rbegin(); it != fading.rend(); ++it) {
            Notice& notice = **it;
            if (notice.shownAt < 0) notice.shownAt = now;
            float left = (float)(FadeAfter - (now - notice.shownAt));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(left, 0.0f, 1.0f));
            drawNotice(notice, true, true);
            ImGui::PopStyleVar();
        }
        // the mouse on it keeps them from fading
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            for (Notice* notice : fading) notice->shownAt = std::max(notice->shownAt, now - FadeAfter + 1.0);
        }
        ImGui::End();
        return (int)fading.size();
    }

    int DrawOverGame(double now) {
        std::vector<Notice*> errors;
        for (Notice& notice : notices) {
            if (overGame(notice, now)) errors.push_back(&notice);
        }
        if (errors.empty()) return 0;
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, Controls::GetTheme().badge * 2), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##noticesOverGame", nullptr, boxFlags | ImGuiWindowFlags_NoInputs);
        ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.9f);
        for (Notice* notice : errors) {
            float left = (float)(OverGameFor - (now - notice->addedAt));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(left, 0.0f, 1.0f));
            ImGui::PushID(notice->id);
            float font = ImGui::GetFontSize();
            ImVec2 at = ImGui::GetCursorScreenPos();
            float lineHeight = ImGui::GetTextLineHeight();
            kindIcon(ImGui::GetWindowDrawList(), ImVec2(at.x + font * 0.5f, at.y + lineHeight / 2), font * 0.5f, notice->kind);
            ImGui::Dummy(ImVec2(font, lineHeight));
            ImGui::SameLine();
            if (!notice->from.empty()) {
                ImGui::TextUnformatted((notice->from + ": " + notice->text).c_str());
            }
            else ImGui::TextUnformatted(notice->text.c_str());
            ImGui::PopID();
            ImGui::PopStyleVar();
        }
        ImGui::PopFont();
        ImGui::End();
        return (int)errors.size();
    }

    void DrawList(const std::string& owner) {
        for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
            if (it->owner != owner) continue;
            // a closed tip is gone, the rest stay for reading back
            if (it->kind == Kind::Tip && it->closed) continue;
            ImGui::BeginDisabled(it->closed && it->kind != Kind::Error);
            drawNotice(*it, it->kind == Kind::Tip, false);
            ImGui::EndDisabled();
        }
    }

    void Clear() {
        notices.clear();
        closedTips.clear();
    }
}
