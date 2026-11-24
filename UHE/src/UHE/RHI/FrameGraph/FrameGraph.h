#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "UHE/RHI/FrameGraph/Blackboard.h"
#include "UHE/RHI/FrameGraph/FrameGraphTypes.h"

namespace UHE::RHI
{

class RHICommandBuffer;
class FrameGraph;

/**
 * @brief What a pass's execute callback records with.
 *
 * A backend subclasses it to hand out the frame's command buffer and the
 * physical resource behind each handle. The callback never sees a barrier or
 * a layout: the backend has put every declared resource in the state its
 * usage needs before the callback runs.
 */
class UHE_API FGContext
{
public:
    virtual ~FGContext() = default;

    /// @brief The command buffer this pass records into, inside its rendering scope.
    virtual RHICommandBuffer& Cmd() = 0;
    /// @brief The texture behind \p handle this frame; nullptr if the pass did not declare it.
    [[nodiscard]] virtual TextureHandle Get(FGHandle<FGTexture> handle) const = 0;
    /// @brief The buffer behind \p handle this frame; nullptr if the pass did not declare it.
    [[nodiscard]] virtual BufferHandle Get(FGHandle<FGBuffer> handle) const = 0;
    /// @brief Render area of a graphics pass (its attachments' size); 0x0 otherwise.
    [[nodiscard]] virtual u32 Width() const = 0;
    [[nodiscard]] virtual u32 Height() const = 0;

    [[nodiscard]] const std::string& PassName() const { return m_PassName; }
    [[nodiscard]] const Blackboard& Board() const { return *m_Board; }

protected:
    FGContext(std::string passName, const Blackboard& board) : m_PassName(std::move(passName)), m_Board(&board) {}

private:
    std::string m_PassName;
    const Blackboard* m_Board;
};

/**
 * @brief Declares one pass's resources during its setup callback.
 *
 * Every call checks its arguments and records an FGError instead of throwing;
 * FrameGraph::Validate() reports them all, so one broken pass names itself
 * rather than taking the frame down with the first mistake.
 */
class UHE_API FGBuilder
{
public:
    /// @brief A new resource this pass creates; the backend allocates it. Version 0 until written.
    FGHandle<FGTexture> Create(const std::string& name, const FGTexture::Desc& desc);
    FGHandle<FGBuffer> Create(const std::string& name, const FGBuffer::Desc& desc);

    /// @brief Declares a read of the current version of \p handle. @return \p handle.
    FGHandle<FGTexture> Read(FGHandle<FGTexture> handle, FGRead usage = FGRead::Sampled);
    FGHandle<FGBuffer> Read(FGHandle<FGBuffer> handle, FGRead usage = FGRead::Sampled);

    /// @brief Declares a write of \p handle. @return the version the write produces; use it from here on.
    FGHandle<FGTexture> Write(FGHandle<FGTexture> handle, FGWrite usage, const FGAttachment& attachment = {});
    FGHandle<FGBuffer> Write(FGHandle<FGBuffer> handle, FGWrite usage);

private:
    friend class FrameGraph;
    FGBuilder(FrameGraph& graph, u32 pass) : m_Graph(graph), m_Pass(pass) {}
    /// @brief Shared read path for textures and buffers; resource \p index was already checked.
    void RecordRead(u32 index, FGRead usage);

    FrameGraph& m_Graph;
    u32 m_Pass;
};

/**
 * @brief Frostbite-style frame description: passes declared by meaning, realized by a backend.
 *
 * @code
 * struct SceneData { FGHandle<FGTexture> Hdr, Depth; };
 * const SceneData& scene = fg.AddPass<SceneData>("Scene",
 *     [&](FGBuilder& b, SceneData& d) {
 *         d.Hdr = b.Write(b.Create("SceneHDR", {w, h, TextureFormat::RGBA16F}), FGWrite::RenderTarget);
 *         d.Depth = b.Write(b.Create("SceneDepth", {w, h, TextureFormat::D32_FLOAT}), FGWrite::DepthTarget);
 *     },
 *     [](const SceneData& d, FGContext& ctx) { ... ctx.Cmd() ... });
 * fg.Board().Add<SceneData>(scene);
 * @endcode
 *
 * Setup runs inside AddPass; execute runs later, when the backend records the
 * frame, and only if the backend keeps the pass (one whose output nothing
 * consumes is culled). Pass data lives as long as the frame, so the execute
 * callback may hold references into it. Reset() starts the next frame and
 * makes every handle of this one stale.
 */
class UHE_API FrameGraph
{
public:
    FrameGraph() = default;
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    /// @brief Brings an existing texture into the graph (swapchain image, editor framebuffer).
    /// @param initial the state it is in when the frame starts
    /// @param final   the state the frame must leave it in
    FGHandle<FGTexture> Import(const std::string& name, const FGTexture::Desc& desc, TextureHandle texture,
                               FGState initial, FGState final);
    FGHandle<FGBuffer> Import(const std::string& name, const FGBuffer::Desc& desc, BufferHandle buffer);

    /**
     * @brief Declares a pass. @p setup(FGBuilder&, Data&) runs now;
     *        @p execute(const Data&, FGContext&) runs when the backend records it.
     * @return the pass data, filled in by setup, valid until Reset().
     */
    template <class Data, class Setup, class Execute>
    const Data& AddPass(const std::string& name, FGPassType type, Setup&& setup, Execute&& execute)
    {
        static_assert(std::is_invocable_v<Setup, FGBuilder&, Data&>, "setup must be void(FGBuilder&, Data&)");
        static_assert(std::is_invocable_v<Execute, const Data&, FGContext&>,
                      "execute must be void(const Data&, FGContext&)");
        auto data = std::make_shared<Data>();
        Data& ref = *data;
        const u32 pass = BeginPass(name, type, data);
        FGBuilder builder(*this, pass);
        setup(builder, ref);
        m_Passes[pass].Execute = [&ref, fn = std::forward<Execute>(execute)](FGContext& context)
        { fn(static_cast<const Data&>(ref), context); };
        return ref;
    }

    /// @brief AddPass for a graphics pass.
    template <class Data, class Setup, class Execute>
    const Data& AddPass(const std::string& name, Setup&& setup, Execute&& execute)
    {
        return AddPass<Data>(name, FGPassType::Graphics, std::forward<Setup>(setup), std::forward<Execute>(execute));
    }

    [[nodiscard]] Blackboard& Board() { return m_Board; }
    [[nodiscard]] const Blackboard& Board() const { return m_Board; }

    /// @brief Sets a named frame parameter; parameters are part of Hash().
    void SetParameter(const std::string& name, FGParameter value);
    /// @return the parameter, or nullopt when unset or of another type.
    template <class T> [[nodiscard]] std::optional<T> GetParameter(const std::string& name) const
    {
        const auto it = m_Parameters.find(name);
        if (it == m_Parameters.end() || !std::holds_alternative<T>(it->second))
            return std::nullopt;
        return std::get<T>(it->second);
    }

    /// @brief Every error recorded while declaring this frame; empty means the frame can be lowered.
    [[nodiscard]] const std::vector<FGError>& Errors() const { return m_Errors; }
    [[nodiscard]] bool Ok() const { return m_Errors.empty(); }

    /// @brief The frame as a backend consumes it. Meaningful only when Ok().
    [[nodiscard]] FGLoweredFrame Lower() const;
    /// @brief Hash of the frame's structure: passes, uses, descs, imports and parameters.
    [[nodiscard]] u64 Hash() const;

    /// @brief Runs pass \p pass's execute callback (declaration index). Called by the backend.
    void ExecutePass(u32 pass, FGContext& context) const;

    [[nodiscard]] u32 PassCount() const { return static_cast<u32>(m_Passes.size()); }
    [[nodiscard]] u32 ResourceCount() const { return static_cast<u32>(m_Resources.size()); }
    [[nodiscard]] const std::string& PassName(u32 pass) const { return m_Passes[pass].Name; }
    /// @brief The frame number handles are checked against; bumped by Reset().
    [[nodiscard]] u32 Frame() const { return m_Frame; }

    /// @brief Ends the frame: drops passes, resources, parameters and the blackboard.
    void Reset();

private:
    friend class FGBuilder;

    struct Resource
    {
        std::string Name;
        bool IsTexture = true;
        FGTexture::Desc Texture;
        FGBuffer::Desc Buffer;
        bool Imported = false;
        TextureHandle ImportedTexture = nullptr;
        BufferHandle ImportedBuffer = nullptr;
        FGState InitialState = FGState::Undefined;
        FGState FinalState = FGState::Undefined;
        u32 Version = 0; ///< latest version; imported resources start readable at 0
    };

    struct Pass
    {
        std::string Name;
        FGPassType Type = FGPassType::Graphics;
        std::shared_ptr<void> Data; ///< owns the pass data the execute callback refers to
        std::function<void(FGContext&)> Execute;
        std::vector<FGLoweredAccess> Accesses;
    };

    u32 BeginPass(const std::string& name, FGPassType type, std::shared_ptr<void> data);
    u32 AddResource(Resource resource, const std::string& pass);
    /// @return the resource index, or kInvalidIndex after recording why the handle is unusable.
    u32 CheckHandle(u32 index, u32 version, u32 frame, bool isTexture, const std::string& pass, const char* action);
    void PushError(FGErrorType type, const std::string& pass, const std::string& resource, std::string message);

    std::vector<Resource> m_Resources;
    std::vector<Pass> m_Passes;
    std::map<std::string, FGParameter> m_Parameters; // ordered: the hash must not depend on insertion order
    std::vector<FGError> m_Errors;
    Blackboard m_Board;
    u32 m_Frame = 1;
};

} // namespace UHE::RHI
