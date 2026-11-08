#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace UHE
{
    // Transient, theme-aware feedback for editor actions ("Scene saved",
    // "Project opened", ...). Rendered as a stack above the status bar; the
    // timer pauses while the pointer rests on a card and a click dismisses it.
    class NotificationCenter
    {
    public:
        enum class Kind
        {
            Info,
            Success,
            Warning,
            Error,
        };

        void Push(Kind kind, std::string title, std::string message = {}, float seconds = 5.0f);
        void Update(float deltaSeconds);
        void Draw();
        void Clear();

        [[nodiscard]] std::size_t Count() const { return m_Items.size(); }

    private:
        struct Item
        {
            Kind Severity = Kind::Info;
            std::string Title;
            std::string Message;
            float Age = 0.0f;
            float Life = 5.0f;
            bool Hovered = false;
            bool Dismissed = false;
        };

        std::vector<Item> m_Items;
    };
} // namespace UHE
