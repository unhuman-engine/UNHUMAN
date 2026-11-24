#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include <cstring>

namespace UHE::RHI
{

namespace
{
/// @brief FNV-1a, 64-bit: stable across runs and platforms, unlike std::hash.
class Hasher
{
public:
    void Bytes(const void* data, size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            m_Value ^= bytes[i];
            m_Value *= 1099511628211ull;
        }
    }
    template <class T> void Value(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "hash plain values only");
        Bytes(&value, sizeof(value));
    }
    void String(const std::string& text)
    {
        Value(text.size()); // length first: ("ab","c") and ("a","bc") differ
        Bytes(text.data(), text.size());
    }
    [[nodiscard]] u64 Result() const { return m_Value; }

private:
    u64 m_Value = 14695981039346656037ull;
};

const char* ToString(FGPassType type)
{
    switch (type)
    {
        case FGPassType::Graphics:
            return "graphics";
        case FGPassType::Compute:
            return "compute";
        case FGPassType::Transfer:
            return "transfer";
    }
    return "?";
}

const char* ToString(FGRead usage)
{
    return usage == FGRead::Sampled ? "Sampled" : "CopySrc";
}

const char* ToString(FGWrite usage)
{
    switch (usage)
    {
        case FGWrite::RenderTarget:
            return "RenderTarget";
        case FGWrite::DepthTarget:
            return "DepthTarget";
        case FGWrite::Storage:
            return "Storage";
        case FGWrite::CopyDst:
            return "CopyDst";
    }
    return "?";
}

/// @brief The read usages a pass type may declare: the backend derives the state from the pass type.
bool ReadAllowed(FGPassType type, FGRead usage)
{
    return type == FGPassType::Transfer ? usage == FGRead::CopySrc : usage == FGRead::Sampled;
}

bool TextureWriteAllowed(FGPassType type, FGWrite usage)
{
    switch (type)
    {
        case FGPassType::Graphics:
            return usage == FGWrite::RenderTarget || usage == FGWrite::DepthTarget;
        case FGPassType::Compute:
            return usage == FGWrite::Storage;
        case FGPassType::Transfer:
            return usage == FGWrite::CopyDst;
    }
    return false;
}

/// @brief Buffer writes are compute or transfer work; a graphics pass only reads buffers.
bool BufferWriteAllowed(FGPassType type, FGWrite usage)
{
    return (type == FGPassType::Compute && usage == FGWrite::Storage) ||
           (type == FGPassType::Transfer && usage == FGWrite::CopyDst);
}
} // namespace

const char* ToString(FGErrorType type)
{
    switch (type)
    {
        case FGErrorType::InvalidHandle:
            return "InvalidHandle";
        case FGErrorType::StaleVersion:
            return "StaleVersion";
        case FGErrorType::ReadBeforeWrite:
            return "ReadBeforeWrite";
        case FGErrorType::UsageNotAllowed:
            return "UsageNotAllowed";
        case FGErrorType::DoubleWrite:
            return "DoubleWrite";
        case FGErrorType::ReadAndWrite:
            return "ReadAndWrite";
        case FGErrorType::TooManyTargets:
            return "TooManyTargets";
        case FGErrorType::EmptyDesc:
            return "EmptyDesc";
        case FGErrorType::DuplicateName:
            return "DuplicateName";
    }
    return "?";
}

bool IsDepthFormat(TextureFormat format)
{
    return format == TextureFormat::D24_UNORM_S8 || format == TextureFormat::D32_FLOAT;
}

// ─── FGBuilder ──────────────────────────────────────────────────────────────

FGHandle<FGTexture> FGBuilder::Create(const std::string& name, const FGTexture::Desc& desc)
{
    FrameGraph::Resource resource;
    resource.Name = name;
    resource.IsTexture = true;
    resource.Texture = desc;
    if (desc.Width == 0 || desc.Height == 0 || desc.MipLevels == 0 || desc.Layers == 0 || desc.SampleCount == 0)
        m_Graph.PushError(FGErrorType::EmptyDesc, m_Graph.m_Passes[m_Pass].Name, name,
                          "creates a texture with a zero dimension");
    const u32 index = m_Graph.AddResource(std::move(resource), m_Graph.m_Passes[m_Pass].Name);
    return {index, 0, m_Graph.m_Frame};
}

FGHandle<FGBuffer> FGBuilder::Create(const std::string& name, const FGBuffer::Desc& desc)
{
    FrameGraph::Resource resource;
    resource.Name = name;
    resource.IsTexture = false;
    resource.Buffer = desc;
    if (desc.Size == 0)
        m_Graph.PushError(FGErrorType::EmptyDesc, m_Graph.m_Passes[m_Pass].Name, name, "creates an empty buffer");
    const u32 index = m_Graph.AddResource(std::move(resource), m_Graph.m_Passes[m_Pass].Name);
    return {index, 0, m_Graph.m_Frame};
}

void FGBuilder::RecordRead(u32 index, FGRead usage)
{
    auto& pass = m_Graph.m_Passes[m_Pass];
    const FrameGraph::Resource& resource = m_Graph.m_Resources[index];
    if (!ReadAllowed(pass.Type, usage))
    {
        m_Graph.PushError(FGErrorType::UsageNotAllowed, pass.Name, resource.Name,
                          std::string("a ") + ToString(pass.Type) + " pass cannot read as " + ToString(usage));
        return;
    }
    if (!resource.Imported && resource.Version == 0)
    {
        m_Graph.PushError(FGErrorType::ReadBeforeWrite, pass.Name, resource.Name,
                          "reads '" + resource.Name + "' before any pass wrote it");
        return;
    }
    for (const FGLoweredAccess& access : pass.Accesses)
    {
        if (access.Resource != index)
            continue;
        if (access.IsWrite)
            m_Graph.PushError(FGErrorType::ReadAndWrite, pass.Name, resource.Name,
                              "reads and writes '" + resource.Name + "' in one pass");
        return; // either an error, or a second read of the same version that adds nothing
    }
    FGLoweredAccess access;
    access.Resource = index;
    access.Version = resource.Version;
    access.IsWrite = false;
    access.Read = usage;
    pass.Accesses.push_back(access);
}

FGHandle<FGTexture> FGBuilder::Read(FGHandle<FGTexture> handle, FGRead usage)
{
    auto& pass = m_Graph.m_Passes[m_Pass];
    const u32 index = m_Graph.CheckHandle(handle.Index, handle.Version, handle.Frame, true, pass.Name, "reads");
    if (index != FG::kInvalidIndex)
        RecordRead(index, usage);
    return handle;
}

FGHandle<FGBuffer> FGBuilder::Read(FGHandle<FGBuffer> handle, FGRead usage)
{
    auto& pass = m_Graph.m_Passes[m_Pass];
    const u32 index = m_Graph.CheckHandle(handle.Index, handle.Version, handle.Frame, false, pass.Name, "reads");
    if (index != FG::kInvalidIndex)
        RecordRead(index, usage);
    return handle;
}

FGHandle<FGTexture> FGBuilder::Write(FGHandle<FGTexture> handle, FGWrite usage, const FGAttachment& attachment)
{
    auto& pass = m_Graph.m_Passes[m_Pass];
    const u32 index = m_Graph.CheckHandle(handle.Index, handle.Version, handle.Frame, true, pass.Name, "writes");
    if (index == FG::kInvalidIndex)
        return handle;
    FrameGraph::Resource& resource = m_Graph.m_Resources[index];

    if (!TextureWriteAllowed(pass.Type, usage))
    {
        m_Graph.PushError(FGErrorType::UsageNotAllowed, pass.Name, resource.Name,
                          std::string("a ") + ToString(pass.Type) + " pass cannot write a texture as " +
                              ToString(usage));
        return handle;
    }
    const bool depth = IsDepthFormat(resource.Texture.Format);
    if (usage == FGWrite::RenderTarget && depth)
    {
        m_Graph.PushError(FGErrorType::UsageNotAllowed, pass.Name, resource.Name,
                          "'" + resource.Name + "' has a depth format; write it as DepthTarget");
        return handle;
    }
    if (usage == FGWrite::DepthTarget && !depth)
    {
        m_Graph.PushError(FGErrorType::UsageNotAllowed, pass.Name, resource.Name,
                          "'" + resource.Name + "' has a colour format; write it as RenderTarget");
        return handle;
    }

    u32 colorTargets = 0;
    bool hasDepth = false;
    for (const FGLoweredAccess& access : pass.Accesses)
    {
        if (access.Resource == index)
        {
            const bool doubleWrite = access.IsWrite;
            m_Graph.PushError(doubleWrite ? FGErrorType::DoubleWrite : FGErrorType::ReadAndWrite, pass.Name,
                              resource.Name,
                              doubleWrite ? "writes '" + resource.Name + "' twice in one pass"
                                          : "reads and writes '" + resource.Name + "' in one pass");
            return handle;
        }
        if (access.IsWrite && access.Write == FGWrite::RenderTarget)
            ++colorTargets;
        if (access.IsWrite && access.Write == FGWrite::DepthTarget)
            hasDepth = true;
    }
    if ((usage == FGWrite::DepthTarget && hasDepth) ||
        (usage == FGWrite::RenderTarget && colorTargets >= FG::kMaxRenderTargets))
    {
        m_Graph.PushError(FGErrorType::TooManyTargets, pass.Name, resource.Name,
                          usage == FGWrite::DepthTarget ? "a pass has at most one depth target"
                                                        : "a pass has at most 8 render targets");
        return handle;
    }

    ++resource.Version;
    FGLoweredAccess access;
    access.Resource = index;
    access.Version = resource.Version;
    access.IsWrite = true;
    access.Write = usage;
    access.Attachment = attachment;
    pass.Accesses.push_back(access);
    return {index, resource.Version, m_Graph.m_Frame};
}

FGHandle<FGBuffer> FGBuilder::Write(FGHandle<FGBuffer> handle, FGWrite usage)
{
    auto& pass = m_Graph.m_Passes[m_Pass];
    const u32 index = m_Graph.CheckHandle(handle.Index, handle.Version, handle.Frame, false, pass.Name, "writes");
    if (index == FG::kInvalidIndex)
        return handle;
    FrameGraph::Resource& resource = m_Graph.m_Resources[index];

    if (!BufferWriteAllowed(pass.Type, usage))
    {
        m_Graph.PushError(FGErrorType::UsageNotAllowed, pass.Name, resource.Name,
                          std::string("a ") + ToString(pass.Type) + " pass cannot write a buffer as " +
                              ToString(usage) + "; buffer writes are compute Storage or transfer CopyDst");
        return handle;
    }
    for (const FGLoweredAccess& access : pass.Accesses)
    {
        if (access.Resource == index)
        {
            m_Graph.PushError(access.IsWrite ? FGErrorType::DoubleWrite : FGErrorType::ReadAndWrite, pass.Name,
                              resource.Name,
                              access.IsWrite ? "writes '" + resource.Name + "' twice in one pass"
                                             : "reads and writes '" + resource.Name + "' in one pass");
            return handle;
        }
    }

    ++resource.Version;
    FGLoweredAccess access;
    access.Resource = index;
    access.Version = resource.Version;
    access.IsWrite = true;
    access.Write = usage;
    pass.Accesses.push_back(access);
    return {index, resource.Version, m_Graph.m_Frame};
}

// ─── FrameGraph ─────────────────────────────────────────────────────────────

FGHandle<FGTexture> FrameGraph::Import(const std::string& name, const FGTexture::Desc& desc, TextureHandle texture,
                                       FGState initial, FGState final)
{
    Resource resource;
    resource.Name = name;
    resource.IsTexture = true;
    resource.Texture = desc;
    resource.Imported = true;
    resource.ImportedTexture = texture;
    resource.InitialState = initial;
    resource.FinalState = final;
    if (texture == nullptr)
        PushError(FGErrorType::InvalidHandle, "", name, "imports a null texture");
    if (desc.Width == 0 || desc.Height == 0)
        PushError(FGErrorType::EmptyDesc, "", name, "imports a texture with a zero dimension");
    const u32 index = AddResource(std::move(resource), "");
    return {index, 0, m_Frame};
}

FGHandle<FGBuffer> FrameGraph::Import(const std::string& name, const FGBuffer::Desc& desc, BufferHandle buffer)
{
    Resource resource;
    resource.Name = name;
    resource.IsTexture = false;
    resource.Buffer = desc;
    resource.Imported = true;
    resource.ImportedBuffer = buffer;
    if (buffer == nullptr)
        PushError(FGErrorType::InvalidHandle, "", name, "imports a null buffer");
    if (desc.Size == 0)
        PushError(FGErrorType::EmptyDesc, "", name, "imports an empty buffer");
    const u32 index = AddResource(std::move(resource), "");
    return {index, 0, m_Frame};
}

void FrameGraph::SetParameter(const std::string& name, FGParameter value)
{
    m_Parameters[name] = std::move(value);
}

u32 FrameGraph::BeginPass(const std::string& name, FGPassType type, std::shared_ptr<void> data)
{
    Pass pass;
    pass.Name = name;
    pass.Type = type;
    pass.Data = std::move(data);
    m_Passes.push_back(std::move(pass));
    return static_cast<u32>(m_Passes.size() - 1);
}

u32 FrameGraph::AddResource(Resource resource, const std::string& pass)
{
    for (const Resource& existing : m_Resources)
    {
        if (existing.Name == resource.Name)
        {
            PushError(FGErrorType::DuplicateName, pass, resource.Name,
                      "a resource named '" + resource.Name + "' already exists this frame");
            break;
        }
    }
    m_Resources.push_back(std::move(resource));
    return static_cast<u32>(m_Resources.size() - 1);
}

u32 FrameGraph::CheckHandle(u32 index, u32 version, u32 frame, bool isTexture, const std::string& pass,
                            const char* action)
{
    if (index == FG::kInvalidIndex)
    {
        PushError(FGErrorType::InvalidHandle, pass, "", std::string(action) + " a default-constructed handle");
        return FG::kInvalidIndex;
    }
    if (frame != m_Frame)
    {
        PushError(FGErrorType::InvalidHandle, pass, "",
                  std::string(action) + " a handle from an earlier frame; handles do not survive Reset()");
        return FG::kInvalidIndex;
    }
    if (index >= m_Resources.size() || m_Resources[index].IsTexture != isTexture)
    {
        PushError(FGErrorType::InvalidHandle, pass, "", std::string(action) + " a handle that names no resource");
        return FG::kInvalidIndex;
    }
    const Resource& resource = m_Resources[index];
    if (version != resource.Version)
    {
        PushError(FGErrorType::StaleVersion, pass, resource.Name,
                  std::string(action) + " version " + std::to_string(version) + " of '" + resource.Name +
                      "', but a later write made version " + std::to_string(resource.Version) +
                      " current; use the handle that write returned");
        return FG::kInvalidIndex;
    }
    return index;
}

void FrameGraph::PushError(FGErrorType type, const std::string& pass, const std::string& resource, std::string message)
{
    m_Errors.push_back({type, pass, resource, std::move(message)});
}

FGLoweredFrame FrameGraph::Lower() const
{
    FGLoweredFrame frame;
    frame.Resources.reserve(m_Resources.size());
    for (const Resource& resource : m_Resources)
    {
        FGLoweredResource lowered;
        lowered.Name = resource.Name;
        lowered.IsTexture = resource.IsTexture;
        lowered.Texture = resource.Texture;
        lowered.Buffer = resource.Buffer;
        lowered.Imported = resource.Imported;
        lowered.ImportedTexture = resource.ImportedTexture;
        lowered.ImportedBuffer = resource.ImportedBuffer;
        lowered.InitialState = resource.InitialState;
        lowered.FinalState = resource.FinalState;
        frame.Resources.push_back(std::move(lowered));
    }
    frame.Passes.reserve(m_Passes.size());
    for (u32 i = 0; i < m_Passes.size(); ++i)
    {
        FGLoweredPass lowered;
        lowered.Name = m_Passes[i].Name;
        lowered.Type = m_Passes[i].Type;
        lowered.Index = i;
        lowered.Accesses = m_Passes[i].Accesses;
        frame.Passes.push_back(std::move(lowered));
    }
    frame.Hash = Hash();
    return frame;
}

u64 FrameGraph::Hash() const
{
    Hasher hash;
    hash.Value(m_Resources.size());
    for (const Resource& resource : m_Resources)
    {
        hash.String(resource.Name);
        hash.Value(resource.IsTexture);
        hash.Value(resource.Imported);
        if (resource.IsTexture)
        {
            const FGTexture::Desc& d = resource.Texture;
            hash.Value(d.Width);
            hash.Value(d.Height);
            hash.Value(d.Format);
            hash.Value(d.MipLevels);
            hash.Value(d.Layers);
            hash.Value(d.SampleCount);
            hash.Value(resource.InitialState);
            hash.Value(resource.FinalState);
            // A recreated texture is a new object: the backend must rebind it.
            hash.Value(reinterpret_cast<uintptr_t>(resource.ImportedTexture));
        }
        else
        {
            hash.Value(resource.Buffer.Size);
            hash.Value(resource.Buffer.Usage);
            hash.Value(reinterpret_cast<uintptr_t>(resource.ImportedBuffer));
        }
    }
    hash.Value(m_Passes.size());
    for (const Pass& pass : m_Passes)
    {
        hash.String(pass.Name);
        hash.Value(pass.Type);
        hash.Value(pass.Accesses.size());
        for (const FGLoweredAccess& access : pass.Accesses)
        {
            hash.Value(access.Resource);
            hash.Value(access.Version);
            hash.Value(access.IsWrite);
            hash.Value(access.IsWrite ? static_cast<u8>(access.Write) : static_cast<u8>(access.Read));
            if (access.IsWrite)
            {
                // Load/store change the rendering scope; clear values do not.
                hash.Value(access.Attachment.Load);
                hash.Value(access.Attachment.Store);
            }
        }
    }
    hash.Value(m_Parameters.size());
    for (const auto& [name, value] : m_Parameters)
    {
        hash.String(name);
        hash.Value(value.index());
        std::visit(
            [&hash](const auto& v)
            {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, std::string>)
                    hash.String(v);
                else
                    hash.Value(v);
            },
            value);
    }
    return hash.Result();
}

void FrameGraph::ExecutePass(u32 pass, FGContext& context) const
{
    if (pass < m_Passes.size() && m_Passes[pass].Execute)
        m_Passes[pass].Execute(context);
}

void FrameGraph::Reset()
{
    // The blackboard first: entries may refer to pass data owned by m_Passes.
    m_Board.Clear();
    m_Passes.clear();
    m_Resources.clear();
    m_Parameters.clear();
    m_Errors.clear();
    ++m_Frame;
    if (m_Frame == 0)
        m_Frame = 1; // 0 never matches a live frame
}

} // namespace UHE::RHI
