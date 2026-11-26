#include "uhepch.h"
#include "UHE/UI/UISystem.h"
#include <algorithm>
#include "UHE/Scene/Components.h"
#include "UHE/Scene/Entity.h"
#include "UHE/Scene/Scene.h"

namespace UHE::UI
{
namespace
{
struct Laid
{
    entt::entity Entity = entt::null;
    int Depth = 0;
};

class Builder
{
public:
    explicit Builder(Scene& scene) : m_Scene(scene), m_Registry(scene.GetRegistry()) {}

    /// Lays out \p entity inside \p parentMin..parentMax, then its UI children, in draw order.
    void Layout(entt::entity entity, glm::vec2 parentMin, glm::vec2 parentMax)
    {
        auto& rect = m_Registry.get<UIRectComponent>(entity);
        const glm::vec2 parentSize = parentMax - parentMin;
        const glm::vec2 stretched = parentSize * (rect.AnchorMax - rect.AnchorMin);
        const glm::vec2 size = glm::max(stretched + rect.Size, glm::vec2(0.0f));
        const glm::vec2 anchorPoint = parentMin + parentSize * glm::mix(rect.AnchorMin, rect.AnchorMax, rect.Pivot);
        rect.ComputedMin = anchorPoint + rect.Position - size * rect.Pivot;
        rect.ComputedMax = rect.ComputedMin + size;
        if (!rect.Visible)
            return;
        m_Order.push_back(entity);
        for (entt::entity child : SortedChildren(entity))
            Layout(child, rect.ComputedMin, rect.ComputedMax);
    }

    /// UI entities directly under \p parent (entt::null: the roots), by Order.
    std::vector<entt::entity> SortedChildren(entt::entity parent)
    {
        std::vector<entt::entity> children;
        if (parent == entt::null)
        {
            for (entt::entity entity : m_Registry.view<UIRectComponent>())
                if (!HasUIParent(entity))
                    children.push_back(entity);
            // A stable order for equal Order values: the order of creation.
            std::sort(children.begin(), children.end());
        }
        else if (const auto* relation = m_Registry.try_get<RelationshipComponent>(parent))
        {
            for (u64 id : relation->Children)
            {
                const Entity child = m_Scene.GetEntityWithUUID(id);
                if (child && m_Registry.all_of<UIRectComponent>(child))
                    children.push_back(child);
            }
        }
        std::stable_sort(
            children.begin(), children.end(), [&](entt::entity a, entt::entity b)
            { return m_Registry.get<UIRectComponent>(a).Order < m_Registry.get<UIRectComponent>(b).Order; });
        return children;
    }

    bool HasUIParent(entt::entity entity)
    {
        const auto* relation = m_Registry.try_get<RelationshipComponent>(entity);
        if (relation == nullptr || relation->Parent == 0)
            return false;
        const Entity parent = m_Scene.GetEntityWithUUID(relation->Parent);
        return parent && m_Registry.all_of<UIRectComponent>(parent);
    }

    const std::vector<entt::entity>& Order() const { return m_Order; }

private:
    Scene& m_Scene;
    entt::registry& m_Registry;
    std::vector<entt::entity> m_Order; ///< visible elements, back to front
};

bool Contains(const UIRectComponent& rect, glm::vec2 point)
{
    return point.x >= rect.ComputedMin.x && point.y >= rect.ComputedMin.y && point.x < rect.ComputedMax.x &&
           point.y < rect.ComputedMax.y;
}

bool IsInteractable(entt::registry& registry, entt::entity entity)
{
    if (const auto* button = registry.try_get<UIButtonComponent>(entity); button && button->Interactable)
        return true;
    const auto* bar = registry.try_get<UIProgressBarComponent>(entity);
    return bar != nullptr && bar->Interactable;
}

float SliderValue(const UIRectComponent& rect, float x)
{
    const float width = rect.ComputedMax.x - rect.ComputedMin.x;
    return width > 0.0f ? std::clamp((x - rect.ComputedMin.x) / width, 0.0f, 1.0f) : 0.0f;
}
} // namespace

float Scale(bool scaleWithScreen, float referenceHeight, glm::vec2 screenSize)
{
    if (!scaleWithScreen || referenceHeight <= 0.0f || screenSize.x <= 0.0f || screenSize.y <= 0.0f)
        return 1.0f;
    const float referenceWidth = referenceHeight * 16.0f / 9.0f;
    return std::min(screenSize.y / referenceHeight, screenSize.x / referenceWidth);
}

std::vector<DrawCommand> Update(Scene& scene, glm::vec2 screenSize, const Pointer& screenPointer, Events* events,
                                float scale)
{
    scale = scale > 0.0f ? scale : 1.0f;
    // Everything below is in UI pixels; the draw list is scaled to the screen at the end.
    Pointer pointer = screenPointer;
    pointer.Position /= scale;
    entt::registry& registry = scene.GetRegistry();
    Builder builder(scene);
    for (entt::entity root : builder.SortedChildren(entt::null))
        builder.Layout(root, glm::vec2(0.0f), screenSize / scale);
    const std::vector<entt::entity>& order = builder.Order();

    // ── The mouse: only the topmost interactable element under it reacts ──
    entt::entity hovered = entt::null;
    bool overUI = false;
    if (pointer.Active)
    {
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            if (!Contains(registry.get<UIRectComponent>(*it), pointer.Position))
                continue;
            overUI = overUI || registry.any_of<UIImageComponent, UIProgressBarComponent, UIButtonComponent>(*it);
            if (IsInteractable(registry, *it))
            {
                hovered = *it;
                overUI = true;
                break;
            }
        }
    }
    for (auto [entity, button] : registry.view<UIButtonComponent>().each())
    {
        button.Hovered = entity == hovered;
        if (pointer.Pressed && button.Hovered)
            button.Pressed = true;
        if (pointer.Released || !pointer.Active || !button.Interactable)
        {
            // A click is a press and a release on the same button.
            if (pointer.Released && button.Pressed && button.Hovered)
            {
                ++button.Clicks;
                if (events != nullptr)
                    events->Clicked.push_back(entity);
            }
            button.Pressed = false;
        }
    }
    for (auto [entity, bar] : registry.view<UIProgressBarComponent>().each())
    {
        if (!bar.Interactable || !pointer.Active || pointer.Released)
        {
            bar.Dragging = false;
            continue;
        }
        if (pointer.Pressed && entity == hovered)
            bar.Dragging = true;
        if (bar.Dragging && pointer.Down && registry.all_of<UIRectComponent>(entity))
            bar.Value = SliderValue(registry.get<UIRectComponent>(entity), pointer.Position.x);
    }
    if (events != nullptr)
        events->PointerOverUI = overUI;

    // ── What to draw, back to front ──
    std::vector<DrawCommand> commands;
    for (entt::entity entity : order)
    {
        const auto& rect = registry.get<UIRectComponent>(entity);
        if (const auto* image = registry.try_get<UIImageComponent>(entity))
        {
            DrawCommand box;
            box.Min = rect.ComputedMin;
            box.Max = rect.ComputedMax;
            box.Color = image->Color;
            if (const auto* button = registry.try_get<UIButtonComponent>(entity); button && button->Interactable)
                box.Color = button->Pressed ? button->PressedColor
                                            : (button->Hovered ? button->HoverColor : button->NormalColor);
            box.CornerRadius = image->CornerRadius;
            box.BorderWidth = image->BorderWidth;
            box.BorderColor = image->BorderColor;
            box.TexturePath = image->TexturePath;
            box.Entity = entity;
            commands.push_back(std::move(box));
        }
        if (const auto* bar = registry.try_get<UIProgressBarComponent>(entity))
        {
            DrawCommand back;
            back.Min = rect.ComputedMin;
            back.Max = rect.ComputedMax;
            back.Color = bar->BackgroundColor;
            back.CornerRadius = bar->CornerRadius;
            back.Entity = entity;
            commands.push_back(back);
            const float value = std::clamp(bar->Value, 0.0f, 1.0f);
            if (value > 0.0f)
            {
                DrawCommand fill = back;
                fill.Max.x = rect.ComputedMin.x + (rect.ComputedMax.x - rect.ComputedMin.x) * value;
                fill.Color = bar->FillColor;
                commands.push_back(fill);
            }
        }
        if (const auto* text = registry.try_get<UITextComponent>(entity); text && !text->Text.empty())
        {
            DrawCommand label;
            label.Type = DrawCommand::Kind::Text;
            label.Min = rect.ComputedMin;
            label.Max = rect.ComputedMax;
            label.Color = text->Color;
            label.Text = text->Text;
            label.FontSize = std::max(text->FontSize, 1.0f);
            label.Align = static_cast<int>(text->Align);
            label.FontPath = text->FontPath;
            label.Entity = entity;
            commands.push_back(std::move(label));
        }
    }
    for (DrawCommand& command : commands)
    {
        command.Min *= scale;
        command.Max *= scale;
        command.CornerRadius *= scale;
        command.BorderWidth *= scale;
        command.FontSize *= scale;
    }
    return commands;
}
} // namespace UHE::UI
