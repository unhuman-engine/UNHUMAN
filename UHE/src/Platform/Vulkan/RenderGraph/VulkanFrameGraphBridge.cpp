#include "uhepch.h"
#include "Platform/Vulkan/RenderGraph/VulkanFrameGraphBridge.h"
#include <algorithm>
#include <cstdlib>
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphExecutor.h"
#include "Platform/Vulkan/VulkanDevice.h"

namespace UHE::RHI::VULKAN
{

namespace
{
ImageState ToImageState(FGState state)
{
    switch (state)
    {
        case FGState::Undefined:
            return ImageState::Undefined;
        case FGState::RenderTarget:
            return ImageState::ColorAttachment;
        case FGState::DepthTarget:
            return ImageState::DepthAttachment;
        case FGState::ShaderRead:
            return ImageState::ShaderRead;
        case FGState::Present:
            return ImageState::Present;
        case FGState::Storage:
            return ImageState::Storage;
        case FGState::CopySrc:
            return ImageState::TransferSrc;
        case FGState::CopyDst:
            return ImageState::TransferDst;
    }
    return ImageState::Undefined;
}

RGPassType ToPassType(FGPassType type)
{
    switch (type)
    {
        case FGPassType::Graphics:
            return RGPassType::Graphics;
        case FGPassType::Compute:
            return RGPassType::Compute;
        case FGPassType::Transfer:
            return RGPassType::Transfer;
    }
    return RGPassType::Graphics;
}

/// @brief What a created texture must support: the union of how the frame uses it.
TextureUsage UsageOf(const FGLoweredFrame& frame, u32 resource)
{
    TextureUsage usage = TextureUsage::None;
    for (const FGLoweredPass& pass : frame.Passes)
    {
        for (const FGLoweredAccess& access : pass.Accesses)
        {
            if (access.Resource != resource)
                continue;
            if (!access.IsWrite)
                usage = usage | (access.Read == FGRead::CopySrc ? TextureUsage::TransferSrc : TextureUsage::Sampled);
            else if (access.Write == FGWrite::RenderTarget)
                usage = usage | TextureUsage::ColorAttach;
            else if (access.Write == FGWrite::DepthTarget)
                usage = usage | TextureUsage::DepthAttach;
            else if (access.Write == FGWrite::Storage)
                usage = usage | TextureUsage::Storage;
            else
                usage = usage | TextureUsage::TransferDst;
        }
    }
    return usage;
}

/// @brief What code after the frame needs an exported texture to support.
TextureUsage ExportUsage(const FGLoweredResource& resource)
{
    TextureUsage usage = TextureUsage::None;
    switch (resource.FinalState)
    {
        case FGState::ShaderRead:
            usage = TextureUsage::Sampled;
            break;
        case FGState::Storage:
            usage = TextureUsage::Storage;
            break;
        case FGState::CopySrc:
            usage = TextureUsage::TransferSrc;
            break;
        case FGState::CopyDst:
            usage = TextureUsage::TransferDst;
            break;
        default: // RenderTarget / DepthTarget: the writing pass already asks for the attachment usage
            break;
    }
    if (resource.ExportCopySource)
        usage = usage | TextureUsage::TransferSrc;
    return usage;
}

/// @brief The physical description of created texture \p resource of \p frame.
TextureDesc PhysicalDesc(const FGLoweredFrame& frame, u32 resource)
{
    const FGLoweredResource& logical = frame.Resources[resource];
    TextureDesc desc;
    desc.width = logical.Texture.Width;
    desc.height = logical.Texture.Height;
    desc.mipLevels = logical.Texture.MipLevels;
    desc.format = logical.Texture.Format;
    desc.usage = UsageOf(frame, resource);
    if (logical.Exported)
        desc.usage = desc.usage | ExportUsage(logical);
    // The frame graph's name ("Scene.HDR", "SSAO.Raw") names the image in a GPU debugger.
    desc.debugName = logical.Name.c_str();
    return desc;
}

/// @brief The frontend's view of one render graph pass while it records.
class VulkanFGContext final : public FGContext
{
public:
    VulkanFGContext(const std::string& pass, const Blackboard& board, RHICommandBuffer& cmd, vk::Extent2D extent,
                    const std::vector<TextureHandle>& textures, const std::vector<BufferHandle>& buffers,
                    std::vector<u32> declared)
        : FGContext(pass, board), m_Cmd(cmd), m_Extent(extent), m_Textures(textures), m_Buffers(buffers),
          m_Declared(std::move(declared))
    {
    }

    RHICommandBuffer& Cmd() override { return m_Cmd; }
    [[nodiscard]] TextureHandle Get(FGHandle<FGTexture> handle) const override
    {
        return Declared(handle.Index) && handle.Index < m_Textures.size() ? m_Textures[handle.Index] : nullptr;
    }
    [[nodiscard]] BufferHandle Get(FGHandle<FGBuffer> handle) const override
    {
        return Declared(handle.Index) && handle.Index < m_Buffers.size() ? m_Buffers[handle.Index] : nullptr;
    }
    [[nodiscard]] u32 Width() const override { return m_Extent.width; }
    [[nodiscard]] u32 Height() const override { return m_Extent.height; }

private:
    /// A pass gets only what it declared: anything else may be in the wrong state.
    [[nodiscard]] bool Declared(u32 index) const
    {
        return std::find(m_Declared.begin(), m_Declared.end(), index) != m_Declared.end();
    }

    RHICommandBuffer& m_Cmd;
    vk::Extent2D m_Extent;
    const std::vector<TextureHandle>& m_Textures;
    const std::vector<BufferHandle>& m_Buffers;
    std::vector<u32> m_Declared;
};
} // namespace

VulkanFrameGraphBridge::VulkanFrameGraphBridge(VulkanDevice& device)
    : m_Device(device), m_Pool([&device](const TextureDesc& desc) { return device.CreateTexture(desc); },
                               [&device](TextureHandle texture) { device.DestroyTexture(texture); }),
      m_BufferPool([&device](const BufferDesc& desc) { return device.CreateBuffer(desc); },
                   [&device](BufferHandle buffer) { device.DestroyBuffer(buffer); }),
      m_Exports([&device](const TextureDesc& desc) { return device.CreateTexture(desc); },
                [&device](TextureHandle texture) { device.DestroyTexture(texture); })
{
}

TextureHandle VulkanFrameGraphBridge::ExportedTexture(const FrameGraph& frontend, const std::string& name)
{
    if (frontend.Ok())
    {
        const FGLoweredFrame frame = frontend.Lower();
        for (u32 i = 0; i < frame.Resources.size(); ++i)
        {
            const FGLoweredResource& resource = frame.Resources[i];
            if (!resource.IsTexture || !resource.Exported || resource.Name != name)
                continue;
            const ExportPool::Slot* slot = m_Exports.Acquire(name, PhysicalDesc(frame, i));
            return slot != nullptr ? slot->Object : nullptr;
        }
    }
    // Not declared this frame, or the frame will be dropped: whatever an
    // earlier frame left, as long as one ran with it.
    const ExportPool::Slot* slot = m_Exports.Find(name);
    return slot != nullptr && slot->State != FGState::Undefined ? slot->Object : nullptr;
}

void VulkanFrameGraphBridge::CommitExports()
{
    for (const auto& [name, state] : m_PendingExports)
        m_Exports.SetState(name, state);
    m_PendingExports.clear();
}

bool VulkanFrameGraphBridge::Translate(const FrameGraph& frontend, RenderGraph& graph, std::vector<std::string>& errors)
{
    if (frontend.PassCount() == 0)
        return true;
    if (!frontend.Ok())
    {
        for (const FGError& error : frontend.Errors())
        {
            errors.push_back(std::string(ToString(error.Type)) + " in pass '" + error.Pass + "'" +
                             (error.Resource.empty() ? "" : " on '" + error.Resource + "'") + ": " + error.Message);
        }
        return false;
    }

    const FGLoweredFrame frame = frontend.Lower();

    // Pass interval each resource is used in. Passes compile in declaration
    // order (single queue, ties toward declaration), so these are the
    // compiled lifetimes, or wider when a pass is culled. Two created textures
    // of one shape whose intervals do not touch share an image (#32): the
    // second's first barrier discards and waits on everything before it.
    // UHE_RENDERGRAPH_NO_ALIAS=1 gives every texture its own image, for an A/B.
    static const bool aliasing = []
    {
        const char* off = std::getenv("UHE_RENDERGRAPH_NO_ALIAS");
        return off == nullptr || *off == '\0' || std::string(off) == "0";
    }();
    std::vector<std::pair<u32, u32>> lifetimes(frame.Resources.size(), {TransientTexturePool::kWholeFrame, 0});
    for (u32 p = 0; p < frame.Passes.size(); ++p)
    {
        for (const FGLoweredAccess& access : frame.Passes[p].Accesses)
        {
            if (access.Resource >= lifetimes.size())
                continue;
            auto& [first, last] = lifetimes[access.Resource];
            first = std::min(first, p);
            last = std::max(last, p);
        }
    }

    const u64 aliasedBefore = m_Pool.AliasedCount();
    u32 createdTextures = 0;

    // Resources first: every physical object must exist before a pass names it.
    std::vector<RGTextureHandle> rgTextures(frame.Resources.size());
    std::vector<RGBufferHandle> rgBuffers(frame.Resources.size());
    m_Textures.assign(frame.Resources.size(), nullptr);
    m_Buffers.assign(frame.Resources.size(), nullptr);
    for (u32 i = 0; i < frame.Resources.size(); ++i)
    {
        const FGLoweredResource& resource = frame.Resources[i];
        if (!resource.IsTexture)
        {
            if (resource.Imported)
            {
                m_Buffers[i] = resource.ImportedBuffer;
                const u64 identity = static_cast<u64>(reinterpret_cast<uintptr_t>(resource.ImportedBuffer));
                rgBuffers[i] = graph.ImportBuffer(resource.Name, identity);
                continue;
            }
            // Device-local storage: written by compute passes, read by shaders,
            // and a copy source or destination for transfer passes.
            BufferDesc desc;
            desc.size = resource.Buffer.Size;
            desc.usage = BufferUsage::Storage;
            desc.hostVisible = false;
            m_Buffers[i] = m_BufferPool.Acquire(desc);
            if (m_Buffers[i] == nullptr)
            {
                errors.push_back("'" + resource.Name + "': could not allocate a " + std::to_string(desc.size) +
                                 "-byte buffer");
                return false;
            }
            RGBufferDesc rgDesc;
            rgDesc.name = resource.Name;
            rgDesc.size = desc.size;
            rgDesc.usage = resource.Buffer.Usage;
            rgBuffers[i] = graph.CreateBuffer(rgDesc);
            continue;
        }

        if (resource.Backbuffer)
        {
            // The swapchain image: the device imports it, so the UI pass that
            // draws on top later gets the very same render graph slot.
            const FGTexture::Desc expected = m_Device.GetBackbufferDesc();
            if (resource.Texture.Width != expected.Width || resource.Texture.Height != expected.Height ||
                resource.Texture.Format != expected.Format)
            {
                errors.push_back("'" + resource.Name + "': declared as " + std::to_string(resource.Texture.Width) +
                                 "x" + std::to_string(resource.Texture.Height) + ", but the backbuffer is " +
                                 std::to_string(expected.Width) + "x" + std::to_string(expected.Height) +
                                 "; import it with RHIDevice::GetBackbufferDesc()");
                return false;
            }
            rgTextures[i] = m_Device.ImportSwapchain();
            continue; // registered by the device with the image it acquired
        }

        if (resource.Imported)
        {
            m_Textures[i] = resource.ImportedTexture;
            // The identity is the object: a recreated texture is a new import.
            const u64 identity = static_cast<u64>(reinterpret_cast<uintptr_t>(resource.ImportedTexture));
            rgTextures[i] = graph.ImportTexture(resource.Name, identity, resource.Texture.Width,
                                                resource.Texture.Height, resource.Texture.SampleCount,
                                                ToImageState(resource.InitialState), ToImageState(resource.FinalState));
            continue;
        }

        const TextureDesc desc = PhysicalDesc(frame, i);
        if (resource.Exported)
        {
            // One object per name across frames: to the render graph it is an
            // import, which also makes the pass that writes it a culling root.
            // Its barrier starts from the state the last frame that ran left it in.
            const ExportPool::Slot* slot = m_Exports.Acquire(resource.Name, desc);
            if (slot == nullptr)
            {
                errors.push_back("'" + resource.Name + "': could not allocate a " + std::to_string(desc.width) + "x" +
                                 std::to_string(desc.height) + " exported texture");
                return false;
            }
            m_Textures[i] = slot->Object;
            const u64 identity = static_cast<u64>(reinterpret_cast<uintptr_t>(slot->Object));
            rgTextures[i] =
                graph.ImportTexture(resource.Name, identity, desc.width, desc.height, resource.Texture.SampleCount,
                                    ToImageState(slot->State), ToImageState(resource.FinalState));
            m_PendingExports.emplace_back(resource.Name, resource.FinalState);
            continue;
        }
        ++createdTextures;
        const auto [first, last] = lifetimes[i];
        m_Textures[i] = aliasing && first <= last ? m_Pool.Acquire(desc, first, last) : m_Pool.Acquire(desc);
        if (m_Textures[i] == nullptr)
        {
            errors.push_back("'" + resource.Name + "': could not allocate a " + std::to_string(desc.width) + "x" +
                             std::to_string(desc.height) + " texture");
            return false;
        }
        RGTextureDesc rgDesc;
        rgDesc.name = resource.Name;
        rgDesc.format = desc.format;
        rgDesc.width = desc.width;
        rgDesc.height = desc.height;
        rgDesc.mipLevels = desc.mipLevels;
        rgDesc.layers = resource.Texture.Layers;
        rgDesc.sampleCount = resource.Texture.SampleCount;
        rgDesc.usage = desc.usage;
        rgTextures[i] = graph.CreateTexture(rgDesc);
    }

    // Said once, so a capture log shows whether aliasing is doing anything.
    if (static bool reported = false; !reported && m_Pool.AliasedCount() > aliasedBefore)
    {
        reported = true;
        UHE_CORE_INFO("[RENDERGRAPH] aliasing: {0} of {1} transient textures share an image with another this frame",
                      m_Pool.AliasedCount() - aliasedBefore, createdTextures);
    }

    for (const FGLoweredPass& pass : frame.Passes)
    {
        RGPassBuilder& builder = graph.AddPass(pass.Name, ToPassType(pass.Type));
        std::vector<u32> declared;
        for (const FGLoweredAccess& access : pass.Accesses)
        {
            declared.push_back(access.Resource);
            const bool isTexture = frame.Resources[access.Resource].IsTexture;
            if (!access.IsWrite)
            {
                if (isTexture)
                    (void)builder.Read(rgTextures[access.Resource]);
                else
                    (void)builder.Read(rgBuffers[access.Resource]);
                continue;
            }
            if (!isTexture)
            {
                (void)builder.Write(rgBuffers[access.Resource]);
                continue;
            }

            // Write() first, then the attachment re-binds to the version this
            // pass produced (the render graph's rule for attachments).
            const RGTextureHandle texture = rgTextures[access.Resource];
            (void)builder.Write(texture);
            const FGAttachment& ops = access.Attachment;
            if (access.Write == FGWrite::RenderTarget)
            {
                RGColorAttachment color;
                color.texture = texture;
                color.load = ops.Load;
                color.store = ops.Store;
                std::copy(ops.ClearColor.begin(), ops.ClearColor.end(), color.clearColor);
                (void)builder.Color(color);
            }
            else if (access.Write == FGWrite::DepthTarget)
            {
                RGDepthAttachment depth;
                depth.texture = texture;
                depth.load = ops.Load;
                depth.store = ops.Store;
                depth.clearDepth = ops.ClearDepth;
                depth.clearStencil = ops.ClearStencil;
                (void)builder.Depth(depth);
            }
        }

        // The frontend and this bridge outlive the frame's recording: both are
        // owned by the device, and the frontend is reset only afterwards.
        builder.Execute(
            [this, &frontend, index = pass.Index, name = pass.Name,
             declared = std::move(declared)](RGPassContext& context)
            {
                VulkanFGContext fgContext(name, frontend.Board(), m_Device.GetCurrentCommandBuffer(), context.Extent(),
                                          m_Textures, m_Buffers, declared);
                m_Device.BeginGpuPass(name);
                frontend.ExecutePass(index, fgContext);
                m_Device.EndGpuPass();
            });
    }

    // Registration last: the executor resolves graph handles to these objects.
    for (u32 i = 0; i < frame.Resources.size(); ++i)
    {
        if (frame.Resources[i].Backbuffer)
            continue;
        if (frame.Resources[i].IsTexture)
            m_Device.RegisterFrameTexture(rgTextures[i], m_Textures[i]);
        else
            m_Device.RegisterFrameBuffer(rgBuffers[i], m_Buffers[i]);
    }
    return true;
}

} // namespace UHE::RHI::VULKAN
