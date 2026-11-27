// FrameGraph frontend harness: Frostbite-style passes declare logical
// resources by usage, the blackboard carries data between features, every
// misuse is reported with the pass and resource it concerns, and the lowered
// frame (what the backend consumes) carries exactly what was declared. Pure
// CPU: the context is a fake that records what a pass asks for.

#include <iostream>
#include <string>
#include <vector>
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/RHI/FrameGraph/TransientPlacement.h"
#include "UHE/RHI/FrameGraph/TransientPool.h"

using namespace UHE;
using namespace UHE::RHI;

namespace
{
int g_failures = 0;

void check(bool condition, const std::string& what)
{
    std::cout << (condition ? "  ok   - " : "  FAIL - ") << what << '\n';
    g_failures += condition ? 0 : 1;
}

void section(const char* name)
{
    std::cout << "\n[ " << name << " ]\n";
}

/// Stands in for the backend: a fixed command buffer is not needed, so Cmd()
/// is never called by these passes; Get() maps handles to fake objects.
class FakeContext final : public FGContext
{
public:
    FakeContext(const std::string& pass, const Blackboard& board) : FGContext(pass, board) {}
    RHICommandBuffer& Cmd() override { std::abort(); }
    TextureHandle Get(FGHandle<FGTexture> handle) const override
    {
        return reinterpret_cast<TextureHandle>(static_cast<uintptr_t>(0x1000 + handle.Index));
    }
    BufferHandle Get(FGHandle<FGBuffer> handle) const override
    {
        return reinterpret_cast<BufferHandle>(static_cast<uintptr_t>(0x2000 + handle.Index));
    }
    u32 Width() const override { return 1280; }
    u32 Height() const override { return 720; }
};

TextureHandle FakeTexture(uintptr_t id)
{
    return reinterpret_cast<TextureHandle>(id);
}

bool HasError(const FrameGraph& fg, FGErrorType type, const std::string& pass = "")
{
    for (const FGError& error : fg.Errors())
        if (error.Type == type && (pass.empty() || error.Pass == pass))
            return true;
    return false;
}

const FGTexture::Desc kHdr{1280, 720, TextureFormat::RGBA16F};
const FGTexture::Desc kDepth{1280, 720, TextureFormat::D32_FLOAT};
const FGTexture::Desc kBackbuffer{1280, 720, TextureFormat::BGRA8_SRGB};

struct SceneData
{
    FGHandle<FGTexture> Hdr;
    FGHandle<FGTexture> Depth;
};
struct TonemapData
{
    FGHandle<FGTexture> Output;
};
struct Backbuffer
{
    FGHandle<FGTexture> Handle;
};

/// The frame the engine is heading for: Scene -> Tonemap -> backbuffer.
void DeclareSceneAndTonemap(FrameGraph& fg, std::vector<std::string>* executed = nullptr)
{
    fg.Board().Add<Backbuffer>(
        Backbuffer{fg.Import("Backbuffer", kBackbuffer, FakeTexture(0xB0), FGState::Undefined, FGState::Present)});
    const SceneData& scene = fg.AddPass<SceneData>(
        "Scene",
        [](FGBuilder& b, SceneData& d)
        {
            d.Hdr = b.Write(b.Create("SceneHDR", kHdr), FGWrite::RenderTarget);
            d.Depth = b.Write(b.Create("SceneDepth", kDepth), FGWrite::DepthTarget);
        },
        [executed](const SceneData& d, FGContext& ctx)
        {
            if (executed != nullptr)
                executed->push_back(ctx.PassName() + ":" + std::to_string(d.Hdr.Version));
        });
    fg.Board().Add<SceneData>(scene);

    fg.AddPass<TonemapData>(
        "Tonemap",
        [&fg](FGBuilder& b, TonemapData& d)
        {
            b.Read(fg.Board().Get<SceneData>().Hdr);
            d.Output = b.Write(fg.Board().Get<Backbuffer>().Handle, FGWrite::RenderTarget,
                               FGAttachment{LoadOp::Clear, StoreOp::Store});
        },
        [executed](const TonemapData&, FGContext& ctx)
        {
            if (executed != nullptr)
                executed->push_back(ctx.PassName() + ":" + std::to_string(ctx.Board().Get<SceneData>().Hdr.Index));
        });
}
} // namespace

int main()
{
    section("a scene -> tonemap frame");
    {
        FrameGraph fg;
        DeclareSceneAndTonemap(fg);
        check(fg.Ok(), "the frame declares without errors");
        check(fg.PassCount() == 2 && fg.ResourceCount() == 3, "two passes, three resources");

        const FGLoweredFrame frame = fg.Lower();
        check(frame.Passes.size() == 2 && frame.Passes[0].Name == "Scene" && frame.Passes[1].Name == "Tonemap",
              "lowered passes keep declaration order");
        check(frame.Resources[0].Imported && frame.Resources[0].ImportedTexture == FakeTexture(0xB0),
              "the backbuffer is lowered as an import of the given texture");
        check(frame.Resources[0].InitialState == FGState::Undefined &&
                  frame.Resources[0].FinalState == FGState::Present,
              "with its initial and final states");
        check(!frame.Resources[1].Imported && frame.Resources[1].Texture.Format == TextureFormat::RGBA16F,
              "SceneHDR is a created texture with its desc");

        const FGLoweredPass& scene = frame.Passes[0];
        check(scene.Accesses.size() == 2 && scene.Accesses[0].IsWrite &&
                  scene.Accesses[0].Write == FGWrite::RenderTarget && scene.Accesses[0].Version == 1,
              "Scene writes SceneHDR as a render target, producing version 1");
        check(scene.Accesses[1].Write == FGWrite::DepthTarget, "and SceneDepth as its depth target");

        const FGLoweredPass& tonemap = frame.Passes[1];
        check(tonemap.Accesses.size() == 2 && !tonemap.Accesses[0].IsWrite && tonemap.Accesses[0].Resource == 1 &&
                  tonemap.Accesses[0].Version == 1,
              "Tonemap reads the version Scene produced");
        check(tonemap.Accesses[1].Resource == 0 && tonemap.Accesses[1].Version == 1 &&
                  tonemap.Accesses[1].Attachment.Load == LoadOp::Clear,
              "and writes the backbuffer with its attachment ops");
    }

    section("execute callbacks");
    {
        FrameGraph fg;
        std::vector<std::string> executed;
        DeclareSceneAndTonemap(fg, &executed);
        const FGLoweredFrame frame = fg.Lower();
        for (const FGLoweredPass& pass : frame.Passes)
        {
            FakeContext context(pass.Name, fg.Board());
            fg.ExecutePass(pass.Index, context);
        }
        check(executed.size() == 2, "each pass runs once when the backend executes it");
        check(executed.size() == 2 && executed[0] == "Scene:1", "execute sees the pass data setup filled in");
        check(executed.size() == 2 && executed[1] == "Tonemap:1", "and the blackboard from the context");
        check(fg.PassName(0) == "Scene", "pass names are kept for the backend");

        FakeContext context("x", fg.Board());
        fg.ExecutePass(99, context);
        check(true, "executing an index that does not exist is a no-op");
        check(context.Get(fg.Board().Get<SceneData>().Hdr) == reinterpret_cast<TextureHandle>(0x1001),
              "the context maps a handle to its physical texture");
    }

    section("blackboard");
    {
        Blackboard board;
        struct Camera
        {
            float Fov = 60.0f;
        };
        struct Lights
        {
            int Count = 0;
        };
        check(!board.Has<Camera>() && board.TryGet<Camera>() == nullptr, "an empty board has nothing");
        board.Add<Camera>(Camera{75.0f});
        board.Add<Lights>(Lights{3});
        check(board.Get<Camera>().Fov == 75.0f && board.Get<Lights>().Count == 3, "entries are found by type");
        board.Add<Camera>(Camera{90.0f});
        check(board.Get<Camera>().Fov == 90.0f && board.Size() == 2, "adding a type again replaces it");
        board.Get<Lights>().Count = 4;
        check(board.Get<Lights>().Count == 4, "entries are mutable through Get");
        const Blackboard& constBoard = board;
        check(constBoard.TryGet<Lights>() != nullptr, "and readable through a const board");
        board.Clear();
        check(board.Size() == 0 && !board.Has<Camera>(), "Clear empties it");
    }

    section("parameters and the frame hash");
    {
        FrameGraph a;
        FrameGraph b;
        DeclareSceneAndTonemap(a);
        DeclareSceneAndTonemap(b);
        check(a.Hash() == b.Hash(), "identical frames hash identically");
        check(a.Lower().Hash == a.Hash(), "the lowered frame carries the hash");

        a.SetParameter("Shadows.Cascades", i64{4});
        check(a.Hash() != b.Hash(), "a parameter changes the hash");
        b.SetParameter("Shadows.Cascades", i64{4});
        check(a.Hash() == b.Hash(), "the same parameter value restores it");
        check(a.GetParameter<i64>("Shadows.Cascades") == i64{4}, "a parameter reads back as its type");
        check(!a.GetParameter<double>("Shadows.Cascades"), "and not as another type");
        check(!a.GetParameter<i64>("Missing"), "an unknown parameter is nullopt");
        a.SetParameter("Tonemap", std::string("aces"));
        b.SetParameter("Tonemap", std::string("reinhard"));
        check(a.Hash() != b.Hash(), "string parameters are hashed by value");

        FrameGraph c;
        FrameGraph d;
        c.Import("Backbuffer", kBackbuffer, FakeTexture(0xB0), FGState::Undefined, FGState::Present);
        d.Import("Backbuffer", kBackbuffer, FakeTexture(0xB1), FGState::Undefined, FGState::Present);
        check(c.Hash() != d.Hash(), "a recreated imported texture changes the hash");
        FrameGraph e;
        e.Import("Backbuffer", {1920, 1080, TextureFormat::BGRA8_SRGB}, FakeTexture(0xB0), FGState::Undefined,
                 FGState::Present);
        check(c.Hash() != e.Hash(), "a resize changes the hash");
    }

    section("reset and stale handles");
    {
        FrameGraph fg;
        DeclareSceneAndTonemap(fg);
        const FGHandle<FGTexture> oldHdr = fg.Board().Get<SceneData>().Hdr;
        const u32 frame = fg.Frame();
        fg.Reset();
        check(fg.PassCount() == 0 && fg.ResourceCount() == 0 && fg.Board().Size() == 0 && fg.Ok(),
              "Reset drops passes, resources, the blackboard and errors");
        check(fg.Frame() == frame + 1, "and starts a new frame");

        fg.Import("Backbuffer", kBackbuffer, FakeTexture(0xB0), FGState::Undefined, FGState::Present);
        fg.Import("Other", kHdr, FakeTexture(0xC0), FGState::ShaderRead, FGState::ShaderRead);
        struct Empty
        {
        };
        fg.AddPass<Empty>("Late", [&](FGBuilder& b, Empty&) { b.Read(oldHdr); }, [](const Empty&, FGContext&) {});
        check(HasError(fg, FGErrorType::InvalidHandle, "Late"),
              "a handle from the previous frame is refused, even though its index exists again");
    }

    section("versions");
    {
        FrameGraph fg;
        struct Data
        {
            FGHandle<FGTexture> First, Second;
        };
        const Data& data = fg.AddPass<Data>(
            "Writer",
            [](FGBuilder& b, Data& d)
            {
                const auto created = b.Create("Target", kHdr);
                d.First = b.Write(created, FGWrite::RenderTarget);
            },
            [](const Data&, FGContext&) {});
        const Data& again = fg.AddPass<Data>(
            "Overwriter", [&](FGBuilder& b, Data& d) { d.Second = b.Write(data.First, FGWrite::RenderTarget); },
            [](const Data&, FGContext&) {});
        check(fg.Ok() && data.First.Version == 1 && again.Second.Version == 2, "each write produces a new version");

        struct Empty
        {
        };
        fg.AddPass<Empty>(
            "StaleReader", [&](FGBuilder& b, Empty&) { b.Read(data.First); }, [](const Empty&, FGContext&) {});
        check(HasError(fg, FGErrorType::StaleVersion, "StaleReader"),
              "reading a version a later write replaced is refused");
        fg.AddPass<Empty>(
            "StaleWriter", [&](FGBuilder& b, Empty&) { (void)b.Write(data.First, FGWrite::RenderTarget); },
            [](const Empty&, FGContext&) {});
        check(HasError(fg, FGErrorType::StaleVersion, "StaleWriter"), "and so is writing it");
        fg.AddPass<Empty>(
            "Reader", [&](FGBuilder& b, Empty&) { b.Read(again.Second); }, [](const Empty&, FGContext&) {});
        check(!HasError(fg, FGErrorType::StaleVersion, "Reader"), "the latest version reads fine");
    }

    section("misuse is reported, by pass and resource");
    {
        struct Empty
        {
        };
        const auto none = [](const Empty&, FGContext&) {};

        FrameGraph fg;
        const auto backbuffer =
            fg.Import("Backbuffer", kBackbuffer, FakeTexture(0xB0), FGState::Undefined, FGState::Present);

        fg.AddPass<Empty>("Early", [](FGBuilder& b, Empty&) { b.Read(b.Create("Never", kHdr)); }, none);
        check(HasError(fg, FGErrorType::ReadBeforeWrite, "Early"), "reading a created, unwritten texture");

        fg.AddPass<Empty>("Default", [](FGBuilder& b, Empty&) { b.Read(FGHandle<FGTexture>{}); }, none);
        check(HasError(fg, FGErrorType::InvalidHandle, "Default"), "a default-constructed handle");

        fg.AddPass<Empty>(
            "DepthAsColor", [](FGBuilder& b, Empty&) { (void)b.Write(b.Create("D1", kDepth), FGWrite::RenderTarget); },
            none);
        check(HasError(fg, FGErrorType::UsageNotAllowed, "DepthAsColor"), "a depth format as a render target");

        fg.AddPass<Empty>(
            "ColorAsDepth", [](FGBuilder& b, Empty&) { (void)b.Write(b.Create("C1", kHdr), FGWrite::DepthTarget); },
            none);
        check(HasError(fg, FGErrorType::UsageNotAllowed, "ColorAsDepth"), "a colour format as a depth target");

        fg.AddPass<Empty>(
            "StorageInGraphics", [](FGBuilder& b, Empty&) { (void)b.Write(b.Create("S1", kHdr), FGWrite::Storage); },
            none);
        check(HasError(fg, FGErrorType::UsageNotAllowed, "StorageInGraphics"), "a storage write in a graphics pass");

        fg.AddPass<Empty>(
            "TargetInCompute", FGPassType::Compute,
            [](FGBuilder& b, Empty&) { (void)b.Write(b.Create("S2", kHdr), FGWrite::RenderTarget); }, none);
        check(HasError(fg, FGErrorType::UsageNotAllowed, "TargetInCompute"), "a render target in a compute pass");

        fg.AddPass<Empty>("CopyInGraphics", [&](FGBuilder& b, Empty&) { b.Read(backbuffer, FGRead::CopySrc); }, none);
        check(HasError(fg, FGErrorType::UsageNotAllowed, "CopyInGraphics"), "a copy read in a graphics pass");

        fg.AddPass<Empty>(
            "BufferInGraphics",
            [](FGBuilder& b, Empty&) { (void)b.Write(b.Create("Buf", FGBuffer::Desc{256}), FGWrite::Storage); }, none);
        check(HasError(fg, FGErrorType::UsageNotAllowed, "BufferInGraphics"), "a buffer write in a graphics pass");

        fg.AddPass<Empty>(
            "Twice",
            [](FGBuilder& b, Empty&)
            {
                const auto t = b.Create("T2", kHdr);
                (void)b.Write(t, FGWrite::RenderTarget);
                (void)b.Write(t, FGWrite::RenderTarget);
            },
            none);
        check(HasError(fg, FGErrorType::StaleVersion, "Twice") || HasError(fg, FGErrorType::DoubleWrite, "Twice"),
              "writing a resource twice in one pass");

        fg.AddPass<Empty>(
            "Feedback",
            [&](FGBuilder& b, Empty&)
            {
                b.Read(backbuffer);
                (void)b.Write(backbuffer, FGWrite::RenderTarget);
            },
            none);
        check(HasError(fg, FGErrorType::ReadAndWrite, "Feedback"), "reading and writing one resource in a pass");

        fg.AddPass<Empty>(
            "TwoDepths",
            [](FGBuilder& b, Empty&)
            {
                (void)b.Write(b.Create("Da", kDepth), FGWrite::DepthTarget);
                (void)b.Write(b.Create("Db", kDepth), FGWrite::DepthTarget);
            },
            none);
        check(HasError(fg, FGErrorType::TooManyTargets, "TwoDepths"), "a second depth target");

        fg.AddPass<Empty>(
            "NineTargets",
            [](FGBuilder& b, Empty&)
            {
                for (int i = 0; i < 9; ++i)
                    (void)b.Write(b.Create("RT" + std::to_string(i), kHdr), FGWrite::RenderTarget);
            },
            none);
        check(HasError(fg, FGErrorType::TooManyTargets, "NineTargets"), "a ninth render target");

        fg.AddPass<Empty>(
            "Zero", [](FGBuilder& b, Empty&) { (void)b.Create("ZeroSized", FGTexture::Desc{0, 720}); }, none);
        check(HasError(fg, FGErrorType::EmptyDesc, "Zero"), "a zero-sized texture");

        fg.AddPass<Empty>("Dup", [](FGBuilder& b, Empty&) { (void)b.Create("SceneHDR2", kHdr); }, none);
        fg.AddPass<Empty>("Dup2", [](FGBuilder& b, Empty&) { (void)b.Create("SceneHDR2", kHdr); }, none);
        check(HasError(fg, FGErrorType::DuplicateName, "Dup2"), "two resources with one name");

        fg.Import("Null", kHdr, nullptr, FGState::Undefined, FGState::Undefined);
        check(HasError(fg, FGErrorType::InvalidHandle), "importing a null texture");

        bool named = true;
        for (const FGError& error : fg.Errors())
            named = named && !error.Message.empty() && ToString(error.Type)[0] != '?';
        check(named, "every error carries a message and a type name");
        check(!fg.Ok(), "and the frame is not Ok");
    }

    section("valid uses per pass type");
    {
        struct Empty
        {
        };
        const auto none = [](const Empty&, FGContext&) {};
        FrameGraph fg;
        FGHandle<FGTexture> blurred;
        FGHandle<FGBuffer> histogram;
        fg.AddPass<Empty>(
            "Blur", FGPassType::Compute,
            [&](FGBuilder& b, Empty&)
            {
                blurred = b.Write(b.Create("Blurred", kHdr), FGWrite::Storage);
                histogram = b.Write(b.Create("Histogram", FGBuffer::Desc{1024}), FGWrite::Storage);
            },
            none);
        fg.AddPass<Empty>(
            "Readback", FGPassType::Transfer,
            [&](FGBuilder& b, Empty&)
            {
                b.Read(blurred, FGRead::CopySrc);
                (void)b.Write(b.Create("Staging", FGBuffer::Desc{4096}), FGWrite::CopyDst);
            },
            none);
        fg.AddPass<Empty>(
            "Composite",
            [&](FGBuilder& b, Empty&)
            {
                b.Read(blurred);
                b.Read(histogram);
                b.Read(histogram); // a repeated read is harmless
                (void)b.Write(b.Create("Final", kBackbuffer), FGWrite::RenderTarget);
            },
            none);
        check(fg.Ok(), "compute storage, transfer copies and graphics reads all validate");
        const FGLoweredFrame frame = fg.Lower();
        check(frame.Passes[2].Accesses.size() == 3, "a repeated read is recorded once");
        check(frame.Passes[0].Type == FGPassType::Compute && frame.Passes[1].Type == FGPassType::Transfer,
              "pass types are lowered");
        check(!frame.Resources[1].IsTexture && frame.Resources[1].Buffer.Size == 1024, "buffers are lowered too");
    }

    section("backbuffer import");
    {
        FrameGraph fg;
        const auto bb = fg.ImportBackbuffer(kBackbuffer);
        check(fg.Ok() && bb.IsValid(), "the backbuffer imports without a texture handle");
        const FGLoweredFrame frame = fg.Lower();
        check(frame.Resources[0].Backbuffer && frame.Resources[0].Imported, "it is lowered as the backbuffer");
        check(frame.Resources[0].FinalState == FGState::Present && frame.Resources[0].Name == "Backbuffer",
              "named Backbuffer and left ready to present");
        FrameGraph plain;
        plain.Import("Backbuffer", kBackbuffer, FakeTexture(0xB0), FGState::Undefined, FGState::Present);
        check(plain.Hash() != fg.Hash(), "and it hashes apart from an ordinary import");
        fg.ImportBackbuffer(kBackbuffer);
        check(HasError(fg, FGErrorType::DuplicateName), "a second backbuffer import in one frame is refused");
        FrameGraph empty;
        empty.ImportBackbuffer(FGTexture::Desc{});
        check(HasError(empty, FGErrorType::EmptyDesc), "a zero-sized backbuffer says where the size comes from");
    }

    section("transient texture pool");
    {
        int created = 0;
        int destroyed = 0;
        std::vector<TextureHandle> live;
        TransientTexturePool pool(
            [&](const TextureDesc&)
            {
                ++created;
                live.push_back(FakeTexture(0x5000 + static_cast<uintptr_t>(created)));
                return live.back();
            },
            [&](TextureHandle) { ++destroyed; }, 3);

        TextureDesc hdr;
        hdr.width = 1280;
        hdr.height = 720;
        hdr.format = TextureFormat::RGBA16F;
        hdr.usage = TextureUsage::ColorAttach | TextureUsage::Sampled;
        TextureDesc depth = hdr;
        depth.format = TextureFormat::D32_FLOAT;
        depth.usage = TextureUsage::DepthAttach;

        const TextureHandle a = pool.Acquire(hdr);
        const TextureHandle b = pool.Acquire(hdr);
        check(a != nullptr && b != nullptr && a != b, "two requests in one frame get two textures");
        check(pool.Acquire(depth) != a && created == 3, "another shape gets its own texture");
        pool.EndFrame();

        const TextureHandle again = pool.Acquire(hdr);
        check((again == a || again == b) && created == 3, "the next frame reuses instead of allocating");
        TextureDesc labelled = hdr;
        labelled.debugName = "Scene.HDR";
        check(pool.Acquire(labelled) != nullptr && created == 3, "a debug name does not change the shape");
        pool.EndFrame();

        // A resize: the old shape is never asked for again and retires.
        TextureDesc resized = hdr;
        resized.width = 1920;
        resized.height = 1080;
        for (int frame = 0; frame < 3; ++frame)
        {
            check(pool.Acquire(resized) != nullptr, "the new size is served");
            pool.EndFrame();
        }
        check(destroyed == 3, "textures unused for three frames are destroyed (two HDR, one depth)");
        check(pool.Size() == 1, "only the texture still in use remains");
        check(pool.CreatedCount() == 4 && pool.DestroyedCount() == 3, "and the counters agree");

        pool.Clear();
        check(destroyed == 4 && pool.Size() == 0, "Clear destroys the rest");

        TransientTexturePool failing([](const TextureDesc&) { return TextureHandle{nullptr}; }, [](TextureHandle) {});
        check(failing.Acquire(hdr) == nullptr && failing.Size() == 0, "a failed allocation is reported and not kept");

        int buffersCreated = 0;
        TransientBufferPool buffers(
            [&](const BufferDesc&)
            {
                ++buffersCreated;
                return reinterpret_cast<BufferHandle>(static_cast<uintptr_t>(0x7000 + buffersCreated));
            },
            [](BufferHandle) {});
        BufferDesc histogram;
        histogram.size = 1024;
        histogram.usage = BufferUsage::Storage;
        // Aliasing: same-shape requests whose pass intervals do not touch share an object.
        {
            UHE::RHI::TransientTexturePool aliasPool(
                [&](const TextureDesc&) { return reinterpret_cast<TextureHandle>(static_cast<uintptr_t>(++created)); },
                [](TextureHandle) {});
            const u32 before = created;
            const TextureHandle ssaoRaw = aliasPool.Acquire(hdr, 2, 3);
            const TextureHandle bloomTemp = aliasPool.Acquire(hdr, 5, 6);
            check(ssaoRaw == bloomTemp && created == before + 1 && aliasPool.AliasedCount() == 1,
                  "disjoint lifetimes share one texture");
            const TextureHandle overlapping = aliasPool.Acquire(hdr, 3, 5);
            check(overlapping != ssaoRaw && created == before + 2, "an overlapping lifetime gets its own");
            // [6, 8] shares pass 6 with the first texture's [5, 6] (a pass that reads one
            // and writes the other needs both), but not the second's [3, 5].
            const TextureHandle touching = aliasPool.Acquire(hdr, 6, 8);
            check(touching == overlapping && created == before + 2,
                  "sharing an end pass is an overlap; a texture free in that interval serves it");
            check(aliasPool.Acquire(hdr) != ssaoRaw, "a request without an interval conflicts with every claim");
            aliasPool.EndFrame();
            check(aliasPool.Acquire(hdr, 0, 1) == ssaoRaw, "claims end with the frame");
        }

        const BufferHandle first = buffers.Acquire(histogram);
        buffers.EndFrame();
        check(buffers.Acquire(histogram) == first && buffersCreated == 1, "buffers are pooled the same way");
        BufferDesc bigger = histogram;
        bigger.size = 2048;
        check(buffers.Acquire(bigger) != first && buffersCreated == 2, "and a different size is a different buffer");
    }

    section("exports: a created texture kept past the frame");
    {
        const FGTexture::Desc color{1280, 720, TextureFormat::RGBA8_SRGB};
        const FGTexture::Desc ids{1280, 720, TextureFormat::R32_SINT};
        struct Targets
        {
            FGHandle<FGTexture> Color, Ids;
        };
        FrameGraph fg;
        const Targets& t = fg.AddPass<Targets>(
            "Editor.Scene",
            [&](FGBuilder& b, Targets& d)
            {
                d.Color = b.Write(b.Create("Scene.Color", color), FGWrite::RenderTarget);
                d.Ids = b.Write(b.Create("Scene.Ids", ids), FGWrite::RenderTarget);
            },
            [](const Targets&, FGContext&) {});
        const u64 unexported = fg.Hash();
        fg.Export(t.Color);
        fg.Export(t.Ids, {.FinalState = FGState::ShaderRead, .CopySource = true});
        check(fg.Ok(), "exporting the versions the writes returned is accepted");

        const FGLoweredFrame frame = fg.Lower();
        check(frame.Resources[0].Exported && !frame.Resources[0].Imported &&
                  frame.Resources[0].FinalState == FGState::ShaderRead && !frame.Resources[0].ExportCopySource,
              "a default export is lowered as created, kept, and left ShaderRead");
        check(frame.Resources[1].Exported && frame.Resources[1].ExportCopySource,
              "an export copied from outside the graph says so");
        check(fg.Hash() != unexported, "exporting changes the hash");

        fg.Export(t.Color);
        check(HasError(fg, FGErrorType::NotExportable), "exporting one texture twice is refused");
    }
    {
        FrameGraph fg;
        FGHandle<FGTexture> created;
        FGHandle<FGTexture> written;
        FGHandle<FGTexture> imported =
            fg.Import("Imported", kHdr, FakeTexture(0xA0), FGState::Undefined, FGState::ShaderRead);
        struct Data
        {
            FGHandle<FGTexture> Out;
        };
        fg.AddPass<Data>(
            "Writer",
            [&](FGBuilder& b, Data& d)
            {
                created = b.Create("Unwritten", kHdr);
                const FGHandle<FGTexture> fresh = b.Create("Written", kHdr);
                (void)b.Write(fresh, FGWrite::RenderTarget);
                written = fresh; // the version before the write: stale once it ran
                d.Out = imported;
            },
            [](const Data&, FGContext&) {});
        fg.Export(created);
        check(HasError(fg, FGErrorType::ReadBeforeWrite), "exporting a texture nothing wrote is refused");
        fg.Export(written);
        check(HasError(fg, FGErrorType::StaleVersion), "exporting a version a write replaced is refused");
        fg.Export(imported);
        check(HasError(fg, FGErrorType::NotExportable),
              "exporting an import is refused: it already outlives the frame");
        fg.Export(FGHandle<FGTexture>{});
        check(HasError(fg, FGErrorType::InvalidHandle), "exporting a default handle is refused");
    }
    {
        FrameGraph fg;
        struct Data
        {
            FGHandle<FGTexture> Out;
        };
        const Data& d = fg.AddPass<Data>(
            "Writer", [&](FGBuilder& b, Data& out) { out.Out = b.Write(b.Create("T", kHdr), FGWrite::RenderTarget); },
            [](const Data&, FGContext&) {});
        fg.Export(d.Out, {.FinalState = FGState::Present});
        check(HasError(fg, FGErrorType::NotExportable), "an export cannot end in Present");
        fg.Export(d.Out, {.FinalState = FGState::Undefined});
        check(fg.Errors().size() == 2, "nor in Undefined");
        const FGHandle<FGTexture> kept = d.Out;
        fg.Reset();
        fg.Export(kept);
        check(HasError(fg, FGErrorType::InvalidHandle), "a handle from the last frame cannot be exported");
    }

    section("export pool: one texture per name across frames");
    {
        int created = 0;
        std::vector<TextureHandle> destroyed;
        ExportPool pool(
            [&](const TextureDesc&)
            {
                ++created;
                return FakeTexture(0x9000 + static_cast<uintptr_t>(created));
            },
            [&](TextureHandle texture) { destroyed.push_back(texture); }, 3);

        TextureDesc color;
        color.width = 1280;
        color.height = 720;
        color.format = TextureFormat::RGBA8_SRGB;
        color.usage = TextureUsage::ColorAttach | TextureUsage::Sampled;

        const ExportPool::Slot* slot = pool.Acquire("Scene.Color", color);
        check(slot != nullptr && slot->State == FGState::Undefined, "a new export starts Undefined");
        const TextureHandle first = slot->Object;
        check(pool.Acquire("Scene.Color", color)->Object == first && created == 1,
              "the UI and the frame asking in one frame get one object");
        pool.SetState("Scene.Color", FGState::ShaderRead);
        pool.EndFrame();

        slot = pool.Acquire("Scene.Color", color);
        check(slot->Object == first && slot->State == FGState::ShaderRead,
              "the next frame gets the same object, in the state the last one left");
        check(pool.Find("Scene.Color") == slot && pool.Find("Other") == nullptr, "Find looks up by name");

        TextureDesc fewer = color;
        fewer.usage = TextureUsage::ColorAttach;
        check(pool.Acquire("Scene.Color", fewer)->Object == first, "an object with more usage than asked serves");
        TextureDesc more = color;
        more.usage = color.usage | TextureUsage::TransferSrc;
        slot = pool.Acquire("Scene.Color", more);
        const TextureHandle widened = slot->Object;
        check(widened != first && slot->State == FGState::Undefined && created == 2,
              "a missing usage replaces the object, which starts Undefined again");
        check(pool.Acquire("Scene.Color", color)->Object == widened && created == 2,
              "the replacement keeps both usages, so the first request does not replace it back");
        pool.EndFrame();

        TextureDesc resized = more;
        resized.width = 1920;
        resized.height = 1080;
        const TextureHandle big = pool.Acquire("Scene.Color", resized)->Object;
        check(big != widened && created == 3, "a resize replaces the object");
        check(destroyed.empty() && pool.RetiredSize() == 2, "replaced objects wait: frames in flight read them");
        pool.EndFrame();
        pool.Acquire("Scene.Color", resized);
        pool.EndFrame();
        check(destroyed.empty(), "still alive two frames after it was replaced");
        pool.Acquire("Scene.Color", resized);
        pool.EndFrame();
        check(destroyed.size() == 1 && destroyed[0] == first, "the first is destroyed at the end of the third");
        pool.Acquire("Scene.Color", resized);
        pool.EndFrame();
        check(destroyed.size() == 2 && destroyed[1] == widened && pool.RetiredSize() == 0, "and so is the second");

        pool.Acquire("Gone", color);
        for (int frame = 0; frame < 4; ++frame)
        {
            pool.Acquire("Scene.Color", resized);
            pool.EndFrame();
        }
        check(pool.Find("Gone") == nullptr && pool.Size() == 1, "a name nothing asks for is released");
        check(pool.Find("Scene.Color")->Object == big, "while the one in use stays");

        pool.Clear();
        check(pool.Size() == 0 && pool.DestroyedCount() == pool.CreatedCount(), "Clear destroys everything");

        ExportPool failing([](const TextureDesc&) { return TextureHandle{nullptr}; }, [](TextureHandle) {});
        check(failing.Acquire("X", color) == nullptr && failing.Size() == 0, "a failed allocation is reported");
    }

    section("cross-shape aliasing: textures pack into blocks by lifetime (issue #32)");
    {
        // A 4096² shadow atlas (64 MB) used by passes 0-3, an 8 MB SSAO target
        // in 4-5, and a bloom chain 6-9 of shrinking levels, all one memory type.
        std::vector<PlacementRequest> requests = {
            {.Size = 64 << 20, .Alignment = 4096, .First = 0, .Last = 3}, // shadow atlas
            {.Size = 8 << 20, .Alignment = 4096, .First = 4, .Last = 5},  // SSAO
            {.Size = 16 << 20, .Alignment = 4096, .First = 6, .Last = 7}, // bloom 0
            {.Size = 4 << 20, .Alignment = 4096, .First = 7, .Last = 8},  // bloom 1
            {.Size = 32 << 20, .Alignment = 4096, .First = 2, .Last = 9}, // scene HDR, alive throughout
        };
        const Placement placement = PlanTransientPlacement(requests);
        check(placement.RequestedBytes == u64{124} << 20, "one allocation each would take 124 MB");
        check(placement.BlockOf[1] == placement.BlockOf[0] && placement.BlockOf[2] == placement.BlockOf[0],
              "SSAO and bloom 0 live in the shadow atlas's memory, which is dead by then");
        check(placement.BlockOf[3] != placement.BlockOf[2],
              "bloom 1 overlaps bloom 0 in pass 7, so it does not share its block");
        check(placement.BlockOf[4] != placement.BlockOf[0], "the scene HDR overlaps the atlas and keeps its own");
        check(placement.AllocatedBytes < placement.RequestedBytes && placement.AllocatedBytes == u64{100} << 20,
              "the blocks take 100 MB: 64 + 32 + 4");
        bool disjoint = true;
        for (const PlacementBlock& block : placement.Blocks)
            for (u32 a : block.Members)
                for (u32 b : block.Members)
                    if (a != b && requests[a].First <= requests[b].Last && requests[b].First <= requests[a].Last)
                        disjoint = false;
        check(disjoint, "no two members of a block are ever alive at once");

        requests[1].MemoryTypeBits = 0b10;
        requests[0].MemoryTypeBits = 0b01;
        const Placement typed = PlanTransientPlacement(requests);
        check(typed.BlockOf[1] != typed.BlockOf[0], "memory types that do not intersect never share a block");
        check(PlanTransientPlacement({}).Blocks.empty(), "no textures, no blocks");
    }

    std::cout << '\n' << (g_failures == 0 ? "all frame graph checks passed" : "frame graph checks FAILED") << '\n';
    return g_failures == 0 ? 0 : 1;
}
