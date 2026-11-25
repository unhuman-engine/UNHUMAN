#pragma once

#include <cassert>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace UHE::RHI
{

/**
 * @brief Typed per-frame data shared between passes, one entry per type.
 *
 * A pass publishes what later passes need (the handles in its pass data, the
 * camera, the light list) and they look it up by type instead of reaching
 * through globals or capturing each other's locals:
 *
 * @code
 * const auto& scene = fg.AddPass<SceneData>("Scene", ...);
 * fg.Board().Add<SceneData>(scene);
 * // later, in another feature:
 * const SceneData& scene = fg.Board().Get<SceneData>();
 * @endcode
 *
 * Cleared with the frame graph every frame.
 */
class Blackboard
{
public:
    /// @brief Stores \p T built from \p args, replacing an existing \p T. @return the stored value.
    template <class T, class... Args> T& Add(Args&&... args)
    {
        auto value = std::make_shared<T>(std::forward<Args>(args)...);
        T& ref = *value;
        m_Entries[std::type_index(typeid(T))] = std::move(value);
        return ref;
    }

    /// @return the stored \p T, or nullptr when none was added this frame.
    template <class T> [[nodiscard]] T* TryGet()
    {
        const auto it = m_Entries.find(std::type_index(typeid(T)));
        return it == m_Entries.end() ? nullptr : static_cast<T*>(it->second.get());
    }
    template <class T> [[nodiscard]] const T* TryGet() const
    {
        const auto it = m_Entries.find(std::type_index(typeid(T)));
        return it == m_Entries.end() ? nullptr : static_cast<const T*>(it->second.get());
    }

    /// @brief The stored \p T. Asking for one that was never added is a bug in pass order.
    template <class T> [[nodiscard]] T& Get()
    {
        T* value = TryGet<T>();
        assert(value != nullptr && "Blackboard::Get: no entry of this type this frame");
        return *value;
    }
    template <class T> [[nodiscard]] const T& Get() const
    {
        const T* value = TryGet<T>();
        assert(value != nullptr && "Blackboard::Get: no entry of this type this frame");
        return *value;
    }

    template <class T> [[nodiscard]] bool Has() const { return TryGet<T>() != nullptr; }
    [[nodiscard]] size_t Size() const { return m_Entries.size(); }
    void Clear() { m_Entries.clear(); }

private:
    std::unordered_map<std::type_index, std::shared_ptr<void>> m_Entries;
};

} // namespace UHE::RHI
