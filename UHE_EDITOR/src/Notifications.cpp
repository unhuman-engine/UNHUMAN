#include "Notifications.h"

#include "EditorTheme.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace UHE
{
    namespace
    {
        ImVec4 WithAlpha(const ImVec4& color, float alpha)
        {
            return ImVec4(color.x, color.y, color.z, alpha);
        }

        ImVec4 KindColor(NotificationCenter::Kind kind)
        {
            switch (kind)
            {
                case NotificationCenter::Kind::Success: return EditorTheme::Success();
                case NotificationCenter::Kind::Warning: return EditorTheme::Warning();
                case NotificationCenter::Kind::Error:   return EditorTheme::Danger();
                case NotificationCenter::Kind::Info:
                default:                                return EditorTheme::ConsoleInfo();
            }
        }

        const char* KindLabel(NotificationCenter::Kind kind)
        {
            switch (kind)
            {
                case NotificationCenter::Kind::Success: return "Success";
                case NotificationCenter::Kind::Warning: return "Warning";
                case NotificationCenter::Kind::Error:   return "Error";
                case NotificationCenter::Kind::Info:
                default:                                return "Info";
            }
        }
    } // namespace

    void NotificationCenter::Push(Kind kind, std::string title, std::string message, float seconds)
    {
        // Keep the stack short: a burst of asset imports should not cover the
        // viewport with cards. The oldest entry goes first.
        constexpr std::size_t kMaxNotifications = 5;
        if (m_Items.size() >= kMaxNotifications)
            m_Items.erase(m_Items.begin());

        Item item;
        item.Severity = kind;
        item.Title = std::move(title);
        item.Message = std::move(message);
        item.Life = std::max(seconds, 1.0f);
        m_Items.push_back(std::move(item));
    }

    void NotificationCenter::Update(float deltaSeconds)
    {
        for (Item& item : m_Items)
        {
            if (!item.Hovered)
                item.Age += deltaSeconds;
        }

        m_Items.erase(std::remove_if(m_Items.begin(), m_Items.end(),
                                     [](const Item& item) { return item.Age >= item.Life; }),
                      m_Items.end());
    }

    void NotificationCenter::Clear()
    {
        m_Items.clear();
    }

    void NotificationCenter::Draw()
    {
        if (m_Items.empty())
            return;

        constexpr float kCardWidth = 330.0f;
        constexpr float kMargin = 8.0f;
        constexpr float kPaddingX = 12.0f;
        constexpr float kPaddingY = 10.0f;
        constexpr float kGap = 8.0f;
        constexpr float kAccentBar = 4.0f;

        ImFont* boldFont = ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : nullptr;
        const float textWidth = kCardWidth - kPaddingX * 2.0f - kAccentBar - 6.0f;

        // Heights are computed before the window so the auto-sized stack can be
        // anchored to the bottom edge instead of growing through the viewport.
        std::vector<float> heights;
        heights.reserve(m_Items.size());
        float totalHeight = kMargin;
        for (const Item& item : m_Items)
        {
            float height = kPaddingY * 2.0f + ImGui::GetTextLineHeight();
            if (!item.Message.empty())
                height += 3.0f + ImGui::CalcTextSize(item.Message.c_str(), nullptr, false, textWidth).y;
            heights.push_back(height);
            totalHeight += height + kGap;
        }
        totalHeight += kMargin - kGap;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float statusBarHeight = ImGui::GetFrameHeight() + 12.0f;
        const ImVec2 position(viewport->WorkPos.x + viewport->WorkSize.x - kCardWidth - 2.0f * kMargin - 8.0f,
                              viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight - totalHeight - 12.0f);

        ImGui::SetNextWindowPos(position);
        ImGui::SetNextWindowSize(ImVec2(kCardWidth + 2.0f * kMargin, totalHeight));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, kGap));
        ImGui::Begin("##EditorNotifications", nullptr, flags);
        ImGui::PopStyleVar(2);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        float y = origin.y + kMargin;

        for (std::size_t index = 0; index < m_Items.size(); ++index)
        {
            Item& item = m_Items[index];

            ImGui::SetCursorScreenPos(ImVec2(origin.x + kMargin, y));
            char id[32];
            std::snprintf(id, sizeof(id), "##notification%zu", index);
            ImGui::InvisibleButton(id, ImVec2(kCardWidth, heights[index]));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked();
            item.Hovered = hovered;
            if (clicked)
                item.Dismissed = true;

            // Fade in quickly, fade out over the last 0.35 s of the lifetime.
            const float fadeIn = std::min(item.Age / 0.12f, 1.0f);
            const float fadeOut = std::min((item.Life - item.Age) / 0.35f, 1.0f);
            const float alpha = std::clamp(std::min(fadeIn, fadeOut), 0.0f, 1.0f);
            if (alpha <= 0.0f)
            {
                y += heights[index] + kGap;
                continue;
            }

            const ImVec2 cardMin(origin.x + kMargin, y);
            const ImVec2 cardMax(cardMin.x + kCardWidth, cardMin.y + heights[index]);
            const ImVec4 accent = KindColor(item.Severity);

            draw->AddRectFilled(ImVec2(cardMin.x + 2.0f, cardMin.y + 3.0f),
                                ImVec2(cardMax.x + 2.0f, cardMax.y + 3.0f),
                                IM_COL32(0, 0, 0, static_cast<int>(70.0f * alpha)), 6.0f);
            draw->AddRectFilled(cardMin, cardMax,
                                ImGui::GetColorU32(WithAlpha(EditorTheme::ToolbarBg(), 0.97f * alpha)), 6.0f);

            // Left severity rail, clipped to the card's rounded corners.
            draw->PushClipRect(cardMin, cardMax, true);
            draw->AddRectFilled(cardMin, ImVec2(cardMin.x + kAccentBar, cardMax.y),
                                ImGui::GetColorU32(WithAlpha(accent, alpha)), 2.0f);
            draw->PopClipRect();

            draw->AddRect(cardMin, cardMax, ImGui::GetColorU32(ImGuiCol_Separator, alpha), 6.0f, 0, 1.0f);

            const float textX = cardMin.x + kAccentBar + kPaddingX;
            float textY = cardMin.y + kPaddingY;

            draw->AddCircleFilled(ImVec2(textX + 3.0f, textY + ImGui::GetTextLineHeight() * 0.5f), 3.0f,
                                  ImGui::GetColorU32(WithAlpha(accent, alpha)));

            if (boldFont)
                draw->AddText(boldFont, boldFont->LegacySize, ImVec2(textX + 12.0f, textY),
                              ImGui::GetColorU32(WithAlpha(accent, alpha)), item.Title.c_str());
            else
                draw->AddText(ImVec2(textX + 12.0f, textY),
                              ImGui::GetColorU32(WithAlpha(accent, alpha)), item.Title.c_str());

            if (!item.Message.empty())
            {
                textY += ImGui::GetTextLineHeight() + 3.0f;
                draw->AddText(nullptr, 0.0f, ImVec2(textX, textY),
                              ImGui::GetColorU32(ImGuiCol_Text, alpha), item.Message.c_str(), nullptr, textWidth);
            }

            if (hovered)
            {
                ImGui::SetTooltip("%s\n\nClick to dismiss", KindLabel(item.Severity));
                draw->AddRect(cardMin, cardMax, ImGui::GetColorU32(WithAlpha(accent, alpha * 0.8f)), 6.0f, 0,
                              1.5f);
            }

            y += heights[index] + kGap;
        }

        ImGui::End();

        if (std::any_of(m_Items.begin(), m_Items.end(), [](const Item& item) { return item.Dismissed; }))
        {
            m_Items.erase(std::remove_if(m_Items.begin(), m_Items.end(),
                                         [](const Item& item) { return item.Dismissed; }),
                          m_Items.end());
        }
    }
} // namespace UHE
