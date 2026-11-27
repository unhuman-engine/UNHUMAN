// The RHI thin core, exercised on the live device through the public RHI only
// (issue #30). See RHISelfTest.h. Every step records into its own command list,
// submits with a wait and reads the result back, so a failure names the call.

#include "uhepch.h"
#include "UHE/Renderer/RHISelfTest.h"
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/ShaderCache.h"

namespace UHE
{
namespace
{
using namespace RHI;

class SelfTest
{
public:
    explicit SelfTest(RHIDevice& device) : m_Device(device) {}

    RHISelfTestResult Run();

private:
    bool Check(bool condition, const std::string& what)
    {
        ++m_Result.Checks;
        if (condition)
            UHE_CORE_INFO("[RHI-SELFTEST] ok   {0}", what);
        else
        {
            ++m_Result.Failures;
            UHE_CORE_ERROR("[RHI-SELFTEST] FAIL {0}", what);
        }
        return condition;
    }

    /// Records with \p record into a fresh list on \p queue, submits and waits.
    template <class Fn> void Submit(QueueType queue, Fn&& record)
    {
        RHICommandBuffer* list = m_Device.BeginCommandList(CommandListDesc{.Queue = queue});
        if (list == nullptr)
            return;
        record(*list);
        RHICommandBuffer* lists[] = {list};
        m_Device.Submit(SubmitDesc{.Queue = queue, .Lists = lists, .Wait = true});
    }

    BufferHandle Buffer(u64 size, BufferUsage usage, bool hostVisible = false)
    {
        BufferHandle handle =
            m_Device.CreateBuffer(BufferDesc{.size = size, .usage = usage, .hostVisible = hostVisible});
        m_Buffers.push_back(handle);
        return handle;
    }

    TextureHandle Texture(const TextureDesc& desc)
    {
        TextureHandle handle = m_Device.CreateTexture(desc);
        m_Textures.push_back(handle);
        return handle;
    }

    std::vector<u8> Read(BufferHandle buffer, u64 size)
    {
        std::vector<u8> bytes(size);
        if (!m_Device.ReadBuffer(buffer, 0, bytes))
            bytes.clear();
        return bytes;
    }

    /// The barrier that makes a transfer's writes readable by ReadBuffer.
    static BufferBarrier ToHost(BufferHandle buffer)
    {
        return BufferBarrier{.Buffer = buffer,
                             .SrcStage = Stage::Transfer,
                             .DstStage = Stage::Host,
                             .SrcAccess = Access::TransferWrite,
                             .DstAccess = Access::HostRead};
    }

    static TextureBarrier Transition(TextureHandle texture, ImageState from, ImageState to, u32 mip = 0,
                                     u32 mipCount = kRemaining)
    {
        TextureBarrier barrier{.Texture = texture, .Old = from, .New = to, .BaseMip = mip, .MipCount = mipCount};
        const auto stageOf = [](ImageState state)
        {
            switch (state)
            {
                case ImageState::TransferSrc:
                case ImageState::TransferDst:
                    return Stage::Transfer;
                case ImageState::ColorAttachment:
                    return Stage::ColorOutput;
                case ImageState::Undefined:
                    return Stage::TopOfPipe;
                default:
                    return Stage::AllCommands;
            }
        };
        const auto accessOf = [](ImageState state, bool write)
        {
            switch (state)
            {
                case ImageState::TransferSrc:
                    return Access::TransferRead;
                case ImageState::TransferDst:
                    return Access::TransferWrite;
                case ImageState::ColorAttachment:
                    return Access::ColorWrite;
                case ImageState::Undefined:
                    return Access::None;
                default:
                    return write ? Access::MemoryWrite : Access::MemoryRead;
            }
        };
        barrier.SrcStage = stageOf(from);
        barrier.DstStage = stageOf(to);
        barrier.SrcAccess = from == ImageState::TransferSrc ? Access::None : accessOf(from, true);
        barrier.DstAccess = accessOf(to, false);
        return barrier;
    }

    void TestBuffers();
    void TestTextures();
    void TestTimestamps();
    void TestCompute();
    void TestRendering();

    RHIDevice& m_Device;
    RHISelfTestResult m_Result;
    std::vector<BufferHandle> m_Buffers;
    std::vector<TextureHandle> m_Textures;
};

constexpr u8 kRed[4] = {220, 30, 40, 255};
constexpr u8 kBlue[4] = {20, 60, 230, 255};

void SelfTest::TestBuffers()
{
    constexpr u64 kSize = 1024;
    BufferHandle source = Buffer(kSize, BufferUsage::Storage, true);
    BufferHandle target = Buffer(kSize, BufferUsage::Readback);
    BufferHandle viaSecondary = Buffer(kSize, BufferUsage::Readback);
    std::vector<u8> pattern(kSize);
    for (u64 i = 0; i < kSize; ++i)
        pattern[i] = static_cast<u8>(i * 7 + 3);

    Submit(QueueType::Graphics,
           [&](RHICommandBuffer& cmd)
           {
               cmd.UpdateBuffer(source, pattern.data(), kSize); // a mapped write, before the submit
               cmd.CopyBuffer(source, target, BufferCopy{.Size = kSize});
               const BufferBarrier toHost[] = {ToHost(target)};
               cmd.Barrier({}, toHost);
           });
    Check(Read(target, kSize) == pattern, "CopyBuffer, then ReadBuffer through a host barrier");

    // A secondary list, recorded on its own and run from a primary.
    RHICommandBuffer* secondary =
        m_Device.BeginCommandList(CommandListDesc{.Level = CommandListLevel::Secondary, .ThreadIndex = 1});
    if (Check(secondary != nullptr, "BeginCommandList gives a secondary list from another thread's pool"))
    {
        secondary->CopyBuffer(source, viaSecondary, BufferCopy{.Size = kSize});
        Submit(QueueType::Graphics,
               [&](RHICommandBuffer& cmd)
               {
                   RHICommandBuffer* lists[] = {secondary};
                   cmd.ExecuteSecondary(lists);
                   const BufferBarrier toHost[] = {ToHost(viaSecondary)};
                   cmd.Barrier({}, toHost);
               });
        Check(Read(viaSecondary, kSize) == pattern, "ExecuteSecondary runs what the secondary list recorded");
    }
}

void SelfTest::TestTextures()
{
    // 64 x 64, left half red, right half blue; two mips.
    constexpr u64 kSize = 64;
    std::vector<u8> pixels(kSize * kSize * 4);
    for (u64 y = 0; y < kSize; ++y)
        for (u64 x = 0; x < kSize; ++x)
            std::copy_n(x < kSize / 2 ? kRed : kBlue, 4, &pixels[(y * kSize + x) * 4]);
    BufferHandle upload = Buffer(pixels.size(), BufferUsage::Storage, true);
    const TextureDesc desc{.width = static_cast<u32>(kSize),
                           .height = static_cast<u32>(kSize),
                           .mipLevels = 2,
                           .format = TextureFormat::RGBA8_UNORM,
                           .usage = TextureUsage::TransferSrc | TextureUsage::TransferDst | TextureUsage::Sampled,
                           .debugName = "RHISelfTest.Texture"};
    TextureHandle texture = Texture(desc);
    BufferHandle mip1 = Buffer(u64{32} * 32 * 4, BufferUsage::Readback);

    Submit(QueueType::Graphics,
           [&](RHICommandBuffer& cmd)
           {
               cmd.UpdateBuffer(upload, pixels.data(), pixels.size());
               const TextureBarrier toDst[] = {Transition(texture, ImageState::Undefined, ImageState::TransferDst)};
               cmd.Barrier(toDst);
               cmd.CopyBufferToTexture(upload, texture, BufferTextureCopy{});
               const TextureBarrier mip0Src[] = {
                   Transition(texture, ImageState::TransferDst, ImageState::TransferSrc, 0, 1)};
               cmd.Barrier(mip0Src);
               cmd.BlitTexture(texture, texture, TextureBlit{.Src = {.Mip = 0}, .Dst = {.Mip = 1}});
               const TextureBarrier mip1Src[] = {
                   Transition(texture, ImageState::TransferDst, ImageState::TransferSrc, 1, 1)};
               cmd.Barrier(mip1Src);
               cmd.CopyTextureToBuffer(texture, mip1, BufferTextureCopy{.Region = {.Mip = 1}});
               const BufferBarrier toHost[] = {ToHost(mip1)};
               cmd.Barrier({}, toHost);
           });
    const std::vector<u8> half = Read(mip1, u64{32} * 32 * 4);
    const auto texel = [&](const std::vector<u8>& image, u64 width, u64 x, u64 y)
    { return image.size() >= (y * width + x + 1) * 4 ? &image[(y * width + x) * 4] : nullptr; };
    const auto is = [](const u8* t, const u8* c) { return t != nullptr && std::equal(t, t + 4, c); };
    Check(is(texel(half, 32, 0, 0), kRed) && is(texel(half, 32, 15, 31), kRed) && is(texel(half, 32, 16, 0), kBlue) &&
              is(texel(half, 32, 31, 31), kBlue),
          "CopyBufferToTexture, BlitTexture mip 0 to 1 and CopyTextureToBuffer keep left red, right blue");

    // A view of mip 1 is a texture whose mip 0 is that level.
    TextureHandle view = m_Device.CreateTextureView(texture, TextureViewDesc{.BaseMip = 1, .MipCount = 1});
    BufferHandle viewRead = Buffer(u64{32} * 32 * 4, BufferUsage::Readback);
    if (Check(view != nullptr, "CreateTextureView over one mip"))
    {
        Submit(QueueType::Graphics,
               [&](RHICommandBuffer& cmd)
               {
                   cmd.CopyTextureToBuffer(view, viewRead, BufferTextureCopy{});
                   const BufferBarrier toHost[] = {ToHost(viewRead)};
                   cmd.Barrier({}, toHost);
               });
        Check(Read(viewRead, u64{32} * 32 * 4) == half, "the view's mip 0 is the texture's mip 1");
        m_Device.DestroyTexture(view);
    }

    // An imported image: the RHI wraps it, and forgets it without freeing it.
    TextureHandle imported = m_Device.ImportTexture(TextureImportDesc{
        .NativeImage = m_Device.GetNativeTextureHandle(texture, NativeTextureObject::Image), .Desc = desc});
    BufferHandle full = Buffer(pixels.size(), BufferUsage::Readback);
    if (Check(imported != nullptr, "ImportTexture over a native image"))
    {
        Submit(QueueType::Graphics,
               [&](RHICommandBuffer& cmd)
               {
                   cmd.CopyTextureToBuffer(imported, full, BufferTextureCopy{});
                   const BufferBarrier toHost[] = {ToHost(full)};
                   cmd.Barrier({}, toHost);
               });
        Check(Read(full, pixels.size()) == pixels, "an imported texture reads the image it wraps");
        m_Device.DestroyTexture(imported);
    }

    // Texture to texture, after the import went: the image must still be there.
    TextureHandle copy = Texture(TextureDesc{.width = static_cast<u32>(kSize),
                                             .height = static_cast<u32>(kSize),
                                             .format = TextureFormat::RGBA8_UNORM,
                                             .usage = TextureUsage::TransferSrc | TextureUsage::TransferDst,
                                             .debugName = "RHISelfTest.Copy"});
    BufferHandle copyRead = Buffer(pixels.size(), BufferUsage::Readback);
    Submit(QueueType::Graphics,
           [&](RHICommandBuffer& cmd)
           {
               const TextureBarrier toDst[] = {Transition(copy, ImageState::Undefined, ImageState::TransferDst)};
               cmd.Barrier(toDst);
               cmd.CopyTexture(texture, copy, TextureCopy{});
               const TextureBarrier toSrc[] = {Transition(copy, ImageState::TransferDst, ImageState::TransferSrc)};
               cmd.Barrier(toSrc);
               cmd.CopyTextureToBuffer(copy, copyRead, BufferTextureCopy{});
               const BufferBarrier toHost[] = {ToHost(copyRead)};
               cmd.Barrier({}, toHost);
           });
    Check(Read(copyRead, pixels.size()) == pixels, "CopyTexture, after the import was destroyed");
}

void SelfTest::TestTimestamps()
{
    const DeviceLimits& limits = m_Device.GetLimits();
    if (!limits.Timestamps)
    {
        UHE_CORE_INFO("[RHI-SELFTEST] skip timestamps: the device has none");
        return;
    }
    QueryPoolHandle pool = m_Device.CreateQueryPool(QueryPoolDesc{.Type = QueryType::Timestamp, .Count = 2});
    constexpr u64 kBytes = u64{1} << 20;
    BufferHandle a = Buffer(kBytes, BufferUsage::Storage);
    BufferHandle b = Buffer(kBytes, BufferUsage::Storage);
    Submit(QueueType::Graphics,
           [&](RHICommandBuffer& cmd)
           {
               cmd.ResetQueries(pool, 0, 2);
               cmd.WriteTimestamp(pool, 0, Stage::TopOfPipe);
               cmd.CopyBuffer(a, b, BufferCopy{.Size = kBytes});
               cmd.WriteTimestamp(pool, 1, Stage::BottomOfPipe);
           });
    u64 ticks[2] = {};
    const bool ready = m_Device.GetQueryResults(pool, 0, ticks, true);
    const double ns = static_cast<double>(ticks[1] - ticks[0]) * limits.TimestampPeriodNs;
    Check(ready && ticks[1] >= ticks[0] && ns < 1e9,
          "timestamps around a 1 MB copy: " + std::to_string(ns / 1000.0) + " us");
    m_Device.DestroyQueryPool(pool);
}

void SelfTest::TestCompute()
{
    const std::filesystem::path path = FileSystem::Get().GetRootPath() / "assets/shaders/RHISelfTest.slang";
    const ShaderCache::Handle handle = ShaderCache::Get().Load(path.string());
    const ShaderCache::Shader* shader = handle != 0 ? ShaderCache::Get().Get(handle) : nullptr;
    const ShaderStageBinary* stage = shader != nullptr ? shader->FindStage(ShaderStage::Compute) : nullptr;
    if (!Check(stage != nullptr, "the self-test compute shader loads"))
        return;
    ShaderHandle module =
        m_Device.CreateShader(ShaderDesc{.stage = ShaderStage::Compute,
                                         .spirvData = stage->SPIRV.data(),
                                         .spirvSize = stage->SPIRV.size(),
                                         .entryPoint = "main", // Slang names the SPIR-V entry point main
                                         .debugName = "RHISelfTest"});

    constexpr u64 kCount = 256;
    const DescriptorSetLayoutDesc bindings{
        .Bindings = {DescriptorBinding{.Binding = 0, .Type = DescriptorType::StorageBuffer},
                     DescriptorBinding{.Binding = 1, .Type = DescriptorType::StorageBuffer}},
        .DebugName = "RHISelfTest.Set1"};
    DescriptorSetLayoutHandle layout = m_Device.CreateDescriptorSetLayout(bindings);
    ComputePipelineDesc pipelineDesc{.computeShader = module, .debugName = "RHISelfTest"};
    pipelineDesc.descriptorSetLayouts[0] = layout;
    pipelineDesc.descriptorSetLayoutCount = 1;
    PipelineHandle pipeline = m_Device.CreateComputePipeline(pipelineDesc);

    std::vector<u32> input(kCount);
    std::vector<u32> expected(kCount);
    for (u32 i = 0; i < kCount; ++i)
    {
        input[i] = i;
        expected[i] = (i * 2) + 1;
    }
    const auto run = [&](QueueType queue, bool indirect, const std::string& what)
    {
        // Fresh buffers per queue: a buffer's first use on a queue family owns
        // it, and nothing here is handed between families.
        BufferHandle in = Buffer(kCount * 4, BufferUsage::Storage, true);
        BufferHandle out = Buffer(kCount * 4, BufferUsage::Storage);
        BufferHandle args = Buffer(16, BufferUsage::Storage, true);
        BufferHandle read = Buffer(kCount * 4, BufferUsage::Readback);
        DescriptorSetHandle set = m_Device.CreateDescriptorSet(layout);
        const DescriptorWrite writes[] = {DescriptorWrite{.Binding = 0, .Buffer = in},
                                          DescriptorWrite{.Binding = 1, .Buffer = out}};
        m_Device.UpdateDescriptorSet(set, writes);
        const u32 groups[4] = {static_cast<u32>(kCount / 64), 1, 1, 0};
        Submit(queue,
               [&](RHICommandBuffer& cmd)
               {
                   cmd.UpdateBuffer(in, input.data(), kCount * 4);
                   cmd.UpdateBuffer(args, groups, sizeof(groups));
                   cmd.BindPipeline(pipeline);
                   cmd.BindDescriptorSet(1, set);
                   if (indirect)
                       cmd.DispatchIndirect(args, 0);
                   else
                       cmd.Dispatch(static_cast<u32>(kCount / 64));
                   const BufferBarrier toCopy[] = {BufferBarrier{.Buffer = out,
                                                                 .SrcStage = Stage::Compute,
                                                                 .DstStage = Stage::Transfer,
                                                                 .SrcAccess = Access::ShaderWrite,
                                                                 .DstAccess = Access::TransferRead}};
                   cmd.Barrier({}, toCopy);
                   cmd.CopyBuffer(out, read, BufferCopy{.Size = kCount * 4});
                   const BufferBarrier toHost[] = {ToHost(read)};
                   cmd.Barrier({}, toHost);
               });
        std::vector<u32> result(kCount);
        const std::vector<u8> bytes = Read(read, kCount * 4);
        if (bytes.size() == kCount * 4)
            std::memcpy(result.data(), bytes.data(), bytes.size());
        Check(result == expected, what);
        m_Device.DestroyDescriptorSet(set);
    };
    run(QueueType::Graphics, true, "a compute dispatch reads and writes through set 1, with indirect arguments");
    run(QueueType::Compute, false,
        std::string("the same on the compute queue (") +
            (m_Device.GetLimits().DedicatedComputeQueue ? "dedicated" : "the graphics queue") + ")");

    if (m_Device.GetLimits().PushDescriptors)
    {
        DescriptorSetLayoutDesc pushDesc = bindings;
        pushDesc.PushDescriptor = true;
        DescriptorSetLayoutHandle pushLayout = m_Device.CreateDescriptorSetLayout(pushDesc);
        ComputePipelineDesc pushPipelineDesc = pipelineDesc;
        pushPipelineDesc.descriptorSetLayouts[0] = pushLayout;
        PipelineHandle pushPipeline = m_Device.CreateComputePipeline(pushPipelineDesc);
        Check(pushPipeline != pipeline, "a different set layout is a different pipeline (the cache key has it)");
        BufferHandle in = Buffer(kCount * 4, BufferUsage::Storage, true);
        BufferHandle out = Buffer(kCount * 4, BufferUsage::Storage);
        BufferHandle read = Buffer(kCount * 4, BufferUsage::Readback);
        Submit(QueueType::Graphics,
               [&](RHICommandBuffer& cmd)
               {
                   cmd.UpdateBuffer(in, input.data(), kCount * 4);
                   cmd.BindPipeline(pushPipeline);
                   const DescriptorWrite writes[] = {DescriptorWrite{.Binding = 0, .Buffer = in},
                                                     DescriptorWrite{.Binding = 1, .Buffer = out}};
                   cmd.PushDescriptors(1, writes);
                   cmd.Dispatch(static_cast<u32>(kCount / 64));
                   const BufferBarrier toCopy[] = {BufferBarrier{.Buffer = out,
                                                                 .SrcStage = Stage::Compute,
                                                                 .DstStage = Stage::Transfer,
                                                                 .SrcAccess = Access::ShaderWrite,
                                                                 .DstAccess = Access::TransferRead}};
                   cmd.Barrier({}, toCopy);
                   cmd.CopyBuffer(out, read, BufferCopy{.Size = kCount * 4});
                   const BufferBarrier toHost[] = {ToHost(read)};
                   cmd.Barrier({}, toHost);
               });
        std::vector<u32> result(kCount);
        const std::vector<u8> bytes = Read(read, kCount * 4);
        if (bytes.size() == kCount * 4)
            std::memcpy(result.data(), bytes.data(), bytes.size());
        Check(result == expected, "the same through push descriptors");
        m_Device.WaitIdle();
        m_Device.DestroyComputePipeline(pushPipeline);
        m_Device.DestroyDescriptorSetLayout(pushLayout);
    }
    else
        UHE_CORE_INFO("[RHI-SELFTEST] skip push descriptors: the device has no VK_KHR_push_descriptor");

    m_Device.WaitIdle();
    m_Device.DestroyComputePipeline(pipeline);
    m_Device.DestroyDescriptorSetLayout(layout);
    m_Device.DestroyShader(module);
}

void SelfTest::TestRendering()
{
    const auto clearAndRead = [&](TextureHandle target, TextureHandle readFrom, u64 size, const glm::vec4& color)
    {
        BufferHandle read = Buffer(size * size * 4, BufferUsage::Readback);
        Submit(QueueType::Graphics,
               [&](RHICommandBuffer& cmd)
               {
                   const TextureBarrier toAttachment[] = {
                       Transition(target, ImageState::Undefined, ImageState::ColorAttachment)};
                   cmd.Barrier(toAttachment);
                   RenderPassDesc pass{};
                   pass.colorAttachmentCount = 1;
                   pass.colorAttachments[0] =
                       ColorAttachment{.texture = target, .clearColor = color, .format = TextureFormat::RGBA8_UNORM};
                   pass.renderWidth = static_cast<u32>(size);
                   pass.renderHeight = static_cast<u32>(size);
                   cmd.BeginRendering(pass);
                   cmd.EndRendering();
                   const TextureBarrier toSource[] = {
                       Transition(target, ImageState::ColorAttachment, ImageState::TransferSrc)};
                   cmd.Barrier(toSource);
                   if (readFrom != target)
                   {
                       const TextureBarrier toDst[] = {
                           Transition(readFrom, ImageState::Undefined, ImageState::TransferDst)};
                       cmd.Barrier(toDst);
                       cmd.ResolveTexture(target, readFrom);
                       const TextureBarrier resolved[] = {
                           Transition(readFrom, ImageState::TransferDst, ImageState::TransferSrc)};
                       cmd.Barrier(resolved);
                   }
                   cmd.CopyTextureToBuffer(readFrom, read, BufferTextureCopy{});
                   const BufferBarrier toHost[] = {ToHost(read)};
                   cmd.Barrier({}, toHost);
               });
        return Read(read, size * size * 4);
    };
    const auto near8 = [](u8 value, float expected) { return std::abs(value - expected * 255.0f) <= 1.5f; };

    constexpr u64 kSize = 16;
    TextureHandle target = Texture(TextureDesc{.width = static_cast<u32>(kSize),
                                               .height = static_cast<u32>(kSize),
                                               .format = TextureFormat::RGBA8_UNORM,
                                               .usage = TextureUsage::ColorAttach | TextureUsage::TransferSrc,
                                               .debugName = "RHISelfTest.Target"});
    const glm::vec4 color{0.25f, 0.5f, 0.75f, 1.0f};
    const std::vector<u8> cleared = clearAndRead(target, target, kSize, color);
    Check(cleared.size() == kSize * kSize * 4 && near8(cleared[0], color.r) && near8(cleared[1], color.g) &&
              near8(cleared[2], color.b) && near8(cleared.back(), color.a),
          "BeginRendering clears an attachment the caller moved into ColorAttachment");

    // The convenience pass, built on Barrier() and BeginRendering(): it clears
    // and leaves the attachment in ShaderRead for the sampler that reads it next.
    TextureHandle sampled =
        Texture(TextureDesc{.width = static_cast<u32>(kSize),
                            .height = static_cast<u32>(kSize),
                            .format = TextureFormat::RGBA8_UNORM,
                            .usage = TextureUsage::ColorAttach | TextureUsage::Sampled | TextureUsage::TransferSrc,
                            .debugName = "RHISelfTest.Convenience"});
    BufferHandle sampledRead = Buffer(kSize * kSize * 4, BufferUsage::Readback);
    const glm::vec4 green{0.0f, 1.0f, 0.0f, 1.0f};
    Submit(QueueType::Graphics,
           [&](RHICommandBuffer& cmd)
           {
               RenderPassDesc pass{};
               pass.colorAttachmentCount = 1;
               pass.colorAttachments[0] = ColorAttachment{.texture = sampled, .clearColor = green};
               pass.renderWidth = static_cast<u32>(kSize);
               pass.renderHeight = static_cast<u32>(kSize);
               cmd.BeginRenderPass(pass);
               cmd.EndRenderPass();
               const TextureBarrier toSource[] = {TextureBarrier{.Texture = sampled,
                                                                 .Old = ImageState::ShaderRead,
                                                                 .New = ImageState::TransferSrc,
                                                                 .SrcStage = Stage::Fragment,
                                                                 .DstStage = Stage::Transfer,
                                                                 .SrcAccess = Access::None,
                                                                 .DstAccess = Access::TransferRead}};
               cmd.Barrier(toSource);
               cmd.CopyTextureToBuffer(sampled, sampledRead, BufferTextureCopy{});
               const BufferBarrier toHost[] = {ToHost(sampledRead)};
               cmd.Barrier({}, toHost);
           });
    const std::vector<u8> greenRead = Read(sampledRead, kSize * kSize * 4);
    Check(greenRead.size() == kSize * kSize * 4 && near8(greenRead[0], 0.0f) && near8(greenRead[1], 1.0f),
          "BeginRenderPass/EndRenderPass (over the core) clear and leave the target in ShaderRead");

    if (m_Device.GetLimits().MaxSamples < 4)
    {
        UHE_CORE_INFO("[RHI-SELFTEST] skip resolve: the device has no 4x multisampling");
        return;
    }
    TextureHandle multisampled = Texture(TextureDesc{.width = static_cast<u32>(kSize),
                                                     .height = static_cast<u32>(kSize),
                                                     .format = TextureFormat::RGBA8_UNORM,
                                                     .usage = TextureUsage::ColorAttach | TextureUsage::TransferSrc,
                                                     .debugName = "RHISelfTest.MSAA",
                                                     .sampleCount = 4});
    TextureHandle resolved = Texture(TextureDesc{.width = static_cast<u32>(kSize),
                                                 .height = static_cast<u32>(kSize),
                                                 .format = TextureFormat::RGBA8_UNORM,
                                                 .usage = TextureUsage::TransferDst | TextureUsage::TransferSrc,
                                                 .debugName = "RHISelfTest.Resolved"});
    const glm::vec4 red{1.0f, 0.0f, 0.0f, 1.0f};
    const std::vector<u8> single = clearAndRead(multisampled, resolved, kSize, red);
    Check(single.size() == kSize * kSize * 4 && near8(single[0], 1.0f) && near8(single[1], 0.0f) &&
              near8(single[2], 0.0f),
          "a 4x multisampled clear resolves into a single-sampled texture");
}

RHISelfTestResult SelfTest::Run()
{
    const DeviceLimits& limits = m_Device.GetLimits();
    Check(!limits.DeviceName.empty() && limits.MaxTexture2DSize >= 4096 && limits.MaxColorAttachments >= 4,
          "GetLimits describes the device (" + limits.DeviceName + ")");
    Check(m_Device.GetNativeHandle(NativeObject::Device) != nullptr &&
              m_Device.GetNativeHandle(NativeObject::ComputeQueue) != nullptr,
          "GetNativeHandle gives the device and its queues");
    TestBuffers();
    TestTextures();
    TestTimestamps();
    TestCompute();
    TestRendering();

    m_Device.WaitIdle();
    for (BufferHandle buffer : m_Buffers)
        m_Device.DestroyBuffer(buffer);
    for (TextureHandle texture : m_Textures)
        m_Device.DestroyTexture(texture);
    UHE_CORE_INFO("[RHI-SELFTEST] {0} checks, {1} failures", m_Result.Checks, m_Result.Failures);
    return m_Result;
}
} // namespace

RHISelfTestResult RunRHISelfTest(RHI::RHIDevice& device)
{
    return SelfTest(device).Run();
}

} // namespace UHE
