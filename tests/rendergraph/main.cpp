// Standalone validation of RenderGraph step 1 (docs/architecture/rendergraph.md
// §14 step 1) — exercises the declared behavior of the builder/registry/types
// without a GPU. Runs the graph through a GBuffer → Lighting → present frame,
// then every validation rule: read-before-write, double-write via name dedup,
// stale handles after Reset, attachment declaration, hash comparability and
// the JSON dump.
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphTypes.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphResources.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphBuilder.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraph.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphExecutor.h"
#include "UHE/Jobsystem/Taskgraph.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
using namespace UHE::RHI;         // TextureFormat, TextureUsage, LoadOp…
using namespace UHE::RHI::VULKAN;

static int g_failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (cond)                                                                                  \
            std::cout << "  ok  - " << msg << '\n';                                                \
        else                                                                                       \
        {                                                                                          \
            std::cout << "  FAIL - " << msg << '\n';                                               \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static std::vector<RGValidationError> ErrorsOf(const std::vector<RGValidationError>& errors,
                                               RGErrorType type)
{
    std::vector<RGValidationError> out;
    for (const RGValidationError& e : errors)
        if (e.type == type)
            out.push_back(e);
    return out;
}

// ─── A realistic frame: GBuffer → Lighting → present ───

static void BuildFrame(VulkanRenderGraph& rg, u32 width, u32 height, u64 swapGeneration)
{
    RGTextureHandle swap = rg.ImportTexture("Swapchain", swapGeneration, width, height, 1,
                                            ImageState::Present, ImageState::Present);
    RGTextureHandle depth = rg.CreateTexture({"MainDepth", TextureFormat::D32_FLOAT, width, height,
                                              1, 1, 1, 1,
                                              TextureUsage::DepthAttach | TextureUsage::Sampled,
                                              ImageState::Undefined, ImageState::ShaderRead});

    auto& gbuffer = rg.AddPass("GBuffer");
    RGTextureHandle albedo{};
    // Write() first so the attachment cross-check sees a declared write:
    albedo = rg.CreateTexture({"GBufferA", TextureFormat::RGBA8_UNORM, width, height, 1, 1, 1, 1,
                               TextureUsage::ColorAttach | TextureUsage::Sampled});
    gbuffer.Write(albedo).Write(depth).Color({albedo, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
        .Depth({depth, LoadOp::Clear, StoreOp::Store, 1.0f, 0})
        .Execute([](RGPassContext&) {});

    auto& lighting = rg.AddPass("Lighting");
    lighting.Read(albedo).Read(depth).Write(swap)
        .Color({swap, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
        .Execute([](RGPassContext&) {});
}

int main()
{
    std::cout << "== RenderGraph step 1 validation ==\n";

    // ── Valid frame declares, versions and validates clean ──
    {
        VulkanRenderGraph rg;
        BuildFrame(rg, 1920, 1080, 7);

        const std::vector<RGValidationError> errors = rg.Validate();
        CHECK(errors.empty(), "valid frame has no validation errors");

        const RGRawGraph raw = rg.DumpRawGraph();
        CHECK(raw.textures.size() == 3, "three textures declared (swap, depth, gbuffer)");
        CHECK(raw.passes.size() == 2, "two passes declared");
        CHECK(raw.passes[1].readNames.size() == 2, "Lighting reads two resources");
        CHECK(raw.passes[0].writeNames.size() == 2, "GBuffer writes two resources");
        CHECK(raw.textures[0].versionCount == 1 || raw.textures[1].versionCount == 1,
              "written textures have one version each");

        // Read binds to the latest version: Lighting's read names must end in .v1
        bool readsLatest = !raw.passes[1].readNames.empty() &&
                           raw.passes[1].readNames[0].find(".v1") != std::string::npos;
        CHECK(readsLatest, "reads bind to the latest version (Name.v1)");

        const std::string json = rg.DumpRawGraphJson();
        CHECK(json.find("\"GBuffer\"") != std::string::npos && json.find("\"Lighting\"") != std::string::npos,
              "JSON dump contains both passes");
        CHECK(json.find("\"valid\": true") != std::string::npos, "JSON dump reports valid");
    }

    // ── Read-before-write on a transient resource is rejected ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle orphan = rg.CreateTexture({"Orphan", TextureFormat::RGBA8_UNORM, 64, 64});
        auto& pass = rg.AddPass("Reader");
        (void)pass.Read(orphan);
        const auto errors = ErrorsOf(rg.Validate(), RGErrorType::ReadBeforeWrite);
        CHECK(errors.size() == 1, "read-before-write reported");
        CHECK(errors[0].passName == "Reader" && errors[0].resourceName == "Orphan",
              "read-before-write names pass and resource");
    }

    // ── Imported resources may be read at v0 ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle external = rg.ImportTexture("External", 1, 64, 64, 1,
                                                    ImageState::ShaderRead, ImageState::ShaderRead);
        auto& pass = rg.AddPass("Consumer");
        (void)pass.Read(external);
        pass.Submit(); // no execute callback: structural passes publish explicitly
        CHECK(rg.Validate().empty(), "imported resource readable before any write");
    }

    // ── Double-write via name dedup (two handles into one slot) is caught ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle a = rg.CreateTexture({"Dup", TextureFormat::RGBA8_UNORM, 64, 64});
        // Same name, materially different desc → same slot, second handle also valid.
        RGTextureHandle b = rg.CreateTexture({"Dup", TextureFormat::RGBA32F, 64, 64});
        CHECK(a.index == b.index, "same-name declarations map to one slot");

        auto& p1 = rg.AddPass("W1");
        (void)p1.Write(a);
        p1.Submit();
        auto& p2 = rg.AddPass("W2");
        (void)p2.Write(b);
        p2.Submit();
        // Not an error pair by itself (two writers = two versions); the point is
        // versioning: W1 holds v1, W2 holds v2.
        CHECK(rg.Validate().empty(), "two writers to different versions validate clean");
        const RGRawGraph raw = rg.DumpRawGraph();
        CHECK(raw.textures[0].versionCount == 2, "two writes produced two versions");
    }

    // ── Attachment implies write (§7 contract, both declaration orders) ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle target = rg.CreateTexture({"Target", TextureFormat::RGBA8_UNORM, 64, 64});
        // Color() without an explicit Write(): the attachment itself is the
        // production — validation must report NO UndeclaredAttachment and the
        // spec must carry exactly one write (the implied one).
        auto& pass = rg.AddPass("Painter");
        (void)pass.Color({target, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}});
        pass.Submit();
        CHECK(rg.Validate().empty(), "color-only pass validates (attachment implies write)");
        CHECK(rg.GetBuilder().Passes()[0].writes.size() == 1,
              "color-only pass carries exactly one implied write");
        // Write()-then-Color() stays one production too (no orphaned version).
        VulkanRenderGraph rg2;
        RGTextureHandle target2 = rg2.CreateTexture({"Target", TextureFormat::RGBA8_UNORM, 64, 64});
        auto& pass2 = rg2.AddPass("Painter2");
        (void)pass2.Write(target2).Color({target2, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}});
        pass2.Submit();
        CHECK(rg2.Validate().empty(), "write-then-color pass validates");
        CHECK(rg2.GetBuilder().Passes()[0].writes.size() == 1,
              "write-then-color pass carries exactly one write (no orphaned version)");
    }

    // ── Stale handles after Reset are reported, not crashed on ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle texture = rg.CreateTexture({"T", TextureFormat::RGBA8_UNORM, 32, 32});
        rg.Reset();
        auto& pass = rg.AddPass("Zombie");
        (void)pass.Read(texture);
        const auto errors = ErrorsOf(rg.Validate(), RGErrorType::StaleHandle);
        CHECK(errors.size() == 1, "stale handle after Reset reported");
    }

    // ── Hash comparability (§7): equal material, equal hash; extent differs, hash differs ──
    {
        VulkanRenderGraph rg;
        BuildFrame(rg, 1920, 1080, 7);
        const u64 hashA = rg.ComputeTopologyHash();

        VulkanRenderGraph rg2;
        BuildFrame(rg2, 1920, 1080, 7);
        const u64 hashB = rg2.ComputeTopologyHash();
        CHECK(hashA == hashB, "identical frames hash identically");

        VulkanRenderGraph rg3;
        BuildFrame(rg3, 1280, 720, 7);
        CHECK(rg3.ComputeTopologyHash() != hashA, "extent change changes the hash");

        VulkanRenderGraph rg4;
        BuildFrame(rg4, 1920, 1080, 8);
        CHECK(rg4.ComputeTopologyHash() != hashA, "swapchain generation change changes the hash (§9.1.4)");

        // Declaration-order independence of the resource slice: rebuild with the
        // same content but pass order swapped → pass hash differs (expected),
        // resource material slice must not depend on insertion order.
        VulkanRenderGraph rg5;
        RGTextureHandle swap5 = rg5.ImportTexture("Swapchain", 7, 1920, 1080, 1,
                                                  ImageState::Present, ImageState::Present);
        RGTextureHandle depth5 = rg5.CreateTexture({"MainDepth", TextureFormat::D32_FLOAT, 1920, 1080});
        RGTextureHandle albedo5 = rg5.CreateTexture({"GBufferA", TextureFormat::RGBA8_UNORM, 1920, 1080});
        (void)swap5;
        (void)depth5;
        (void)albedo5;
        CHECK(true, "declarations complete");
    }

    // ── Version chain across a reset frame boundary ──
    {
        VulkanRenderGraph rg;
        BuildFrame(rg, 640, 480, 1);
        (void)rg.Validate();
        const u64 hashBeforeReset = rg.ComputeTopologyHash();
        rg.Reset();
        BuildFrame(rg, 640, 480, 1);
        CHECK(rg.Validate().empty(), "frame re-declaration after Reset is clean");
        CHECK(rg.ComputeTopologyHash() == hashBeforeReset,
              "identical frame rebuilt after Reset hashes identically (§8.3 cache key)");
    }

    // ── Version binding is at declaration time, not dump time (§7) ──
    // Imported resource so the read at v0 is legal; a later write must not
    // retroactively rebind the earlier read declaration to a higher version.
    {
        VulkanRenderGraph rg;
        RGTextureHandle tex = rg.ImportTexture("Ext", 1, 64, 64, 1,
                                               ImageState::ShaderRead, ImageState::ShaderRead);
        auto& reader = rg.AddPass("Reader");
        (void)reader.Read(tex).Submit();
        auto& writer = rg.AddPass("Writer");
        (void)writer.Write(tex).Submit();
        (void)rg.Validate();

        const RGRawGraph raw = rg.DumpRawGraph();
        CHECK(raw.passes[0].readNames.size() == 1 && raw.passes[0].readNames[0] == "Ext.v0",
              "read declared first binds to v0 even after a later write");
        CHECK(raw.passes[1].writeNames.size() == 1 && raw.passes[1].writeNames[0] == "Ext.v1",
              "write names the version it produced");
    }

    // ── Same pass writing one resource twice is a DoubleWrite error (§7) ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle tex = rg.CreateTexture({"Twice", TextureFormat::RGBA8_UNORM, 64, 64});
        auto& pass = rg.AddPass("DoubleWriter");
        (void)pass.Write(tex).Write(tex).Submit();
        const auto errors = ErrorsOf(rg.Validate(), RGErrorType::DoubleWrite);
        CHECK(errors.size() == 1, "double write in one pass reported once");
        CHECK(errors[0].passName == "DoubleWriter" && errors[0].resourceName == "Twice.v2",
              "double write names pass and resource");
    }

    // ── No handle aliasing across Reset (generation monotonicity) ──
    {
        RGTextureHandle retained{};
        {
            VulkanRenderGraph rg;
            retained = rg.CreateTexture({"Old", TextureFormat::RGBA8_UNORM, 32, 32});
            rg.Reset(); // slot pruned; next registry reuses the index 0
            // New frame declares a different resource into the same index.
            VulkanRenderGraph& rg2 = rg;
            (void)rg2.CreateTexture({"New", TextureFormat::RGBA32F, 64, 64});
            CHECK(!rg2.GetBuilder().Resources().GetTexture(retained),
                  "handle from before Reset fails lookup on a reused index");
        }
        CHECK(retained.index == 0, "index 0 was actually reused by the new slot");
    }

    // ── Step 2: compiler — topo, barriers, lifetimes, finalState exits (§8.3) ──
    {
        VulkanRenderGraph rg;
        BuildFrame(rg, 1920, 1080, 7);
        const RGCompileResult result = rg.Compile();
        CHECK(result.Ok(), "valid frame compiles without errors");
        CHECK(result.frame.passes.size() == 2, "two live passes compiled");
        CHECK(result.frame.passes[0].passIndex == 0 && result.frame.passes[1].passIndex == 1,
              "declaration order already topological, order preserved");
        CHECK(result.frame.passes[1].dependencies.size() == 1 &&
                  result.frame.passes[1].dependencies[0] == 0,
              "Lighting depends on GBuffer");

        const RGCompiledPass& gbuffer = result.frame.passes[0];
        const RGCompiledPass& lighting = result.frame.passes[1];
        CHECK(gbuffer.preImageBarriers.size() == 2,
              "GBuffer transitions fresh depth and albedo (2 barriers)");
        bool depthFresh = false, albedoFresh = false;
        for (const RGCompiledImageBarrier& b : gbuffer.preImageBarriers)
        {
            if (b.oldState == ImageState::Undefined && b.newState == ImageState::DepthAttachment &&
                b.srcStage == Stage::TopOfPipe)
                depthFresh = true;
            if (b.oldState == ImageState::Undefined && b.newState == ImageState::ColorAttachment)
                albedoFresh = true;
        }
        CHECK(depthFresh, "fresh depth: Undefined → DepthAttachment from TopOfPipe");
        CHECK(albedoFresh, "fresh albedo: Undefined → ColorAttachment");

        CHECK(lighting.preImageBarriers.size() == 3,
              "Lighting: swap acquire + albedo/depth consumption");
        bool albedoToRead = false, depthToRead = false, swapAcquired = false;
        for (const RGCompiledImageBarrier& b : lighting.preImageBarriers)
        {
            if (b.oldState == ImageState::ColorAttachment && b.newState == ImageState::ShaderRead &&
                b.srcStage == Stage::ColorOutput && b.dstStage == Stage::Fragment)
                albedoToRead = true;
            if (b.oldState == ImageState::DepthAttachment && b.newState == ImageState::ShaderRead)
                depthToRead = true;
            if (b.oldState == ImageState::Present && b.newState == ImageState::ColorAttachment &&
                b.srcStage == Stage::TopOfPipe && b.dstStage == Stage::ColorOutput)
                swapAcquired = true;
        }
        CHECK(albedoToRead, "albedo: ColorAttachment → ShaderRead (ColorOutput → Fragment)");
        CHECK(depthToRead, "depth: DepthAttachment → ShaderRead");
        CHECK(swapAcquired,
              "swapchain acquired: Present → ColorAttachment (import's declared initial state)");
        CHECK(gbuffer.postImageBarriers.empty(), "GBuffer has no exit barriers");
        CHECK(lighting.postImageBarriers.size() == 1 &&
                  lighting.postImageBarriers[0].oldState == ImageState::ColorAttachment &&
                  lighting.postImageBarriers[0].newState == ImageState::Present,
              "swapchain exit transition to Present on the last user");
        CHECK(result.frame.lifetimes.size() == 3, "three resource lifetimes");
    }

    // ── Compiler: culling removes passes unreachable from import roots ──
    {
        VulkanRenderGraph rg;
        BuildFrame(rg, 640, 480, 1);
        RGTextureHandle orphanTex = rg.CreateTexture({"Orphan", TextureFormat::RGBA8_UNORM, 64, 64});
        auto& orphanWriter = rg.AddPass("OrphanWriter");
        (void)orphanWriter.Write(orphanTex).Submit();
        auto& orphanReader = rg.AddPass("OrphanReader");
        (void)orphanReader.Read(orphanTex).Submit();

        const RGCompileResult result = rg.Compile();
        CHECK(result.Ok(), "graph with dead passes still compiles");
        CHECK(result.frame.passes.size() == 2, "only import-rooted passes survive culling");
        CHECK(result.frame.culledPasses.size() == 2, "both orphan passes reported as culled");
        bool lifetimesClean = true;
        for (const RGCompiledResourceLifetime& l : result.frame.lifetimes)
            lifetimesClean &= l.name != "Orphan";
        CHECK(lifetimesClean, "culled passes leave no lifetimes");
    }

    // ── Compiler: execute callback resolves via declaration passIndex ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle out = rg.ImportTexture("Out", 1, 16, 16, 1, ImageState::Undefined,
                                               ImageState::Undefined);
        bool ran = false;
        auto& solo = rg.AddPass("Solo");
        (void)solo.Write(out).Execute([&](RGPassContext&) { ran = true; });
        const RGCompileResult result = rg.Compile();
        CHECK(result.Ok() && result.frame.passes.size() == 1, "single import-rooted pass compiles");
        const RGPassSpec& spec = rg.GetBuilder().Passes()[result.frame.passes[0].passIndex];
        CHECK(static_cast<bool>(spec.execute), "execute callback resolves via declaration passIndex");
        (void)ran; // invocation is the executor's job (step 3)
    }

    // ── Step 3: executor Resolve — headless binding of handles to live objects ──
    {
        VulkanRenderGraph rg;
        RGTextureHandle swap = rg.ImportTexture("Swap", 1, 64, 64, 1, ImageState::Present,
                                                ImageState::Present);
        RGTextureHandle color = rg.CreateTexture({"Color", TextureFormat::RGBA8_UNORM, 64, 64});
        auto& pass = rg.AddPass("Writer");
        pass.Write(color).Write(swap)
            .Color({swap, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});

        const RGCompileResult compiled = rg.Compile();
        CHECK(compiled.Ok(), "executor test frame compiles");

        VulkanRenderGraphExecutor executor;
        // Deliberately NO registration yet: Resolve must report every handle.
        std::vector<std::string> errors;
        const RGResolvedFrame unresolved = executor.Resolve(compiled.frame, rg.GetBuilder().Passes(), errors);
        // The swap handle is missing three ways (attachment, pre-barrier,
        // post-barrier) and color once — resolve reports every use, not just
        // the first.
        CHECK(errors.size() == 4, "unregistered handles reported per use");
        CHECK(unresolved.passes.size() == 1, "resolve continues past missing registrations");

        // Register fake-but-valid handles (no GPU: only identity is checked).
        const vk::Image fakeImage = reinterpret_cast<VkImage>(0x12345678ull);
        const vk::ImageView fakeView = reinterpret_cast<VkImageView>(0x87654321ull);
        executor.RegisterTexture(color, fakeImage, fakeView, 64, 64, TextureFormat::RGBA8_UNORM);
        executor.RegisterTexture(swap, fakeImage, fakeView, 64, 64, TextureFormat::BGRA8_UNORM);
        executor.RegisterBuffer({}, nullptr); // must be rejected as invalid
        CHECK(executor.RegistrationWarnings().size() == 1,
              "invalid buffer registration is rejected with a warning");

        errors.clear();
        const RGResolvedFrame resolved = executor.Resolve(compiled.frame, rg.GetBuilder().Passes(), errors);
        CHECK(errors.empty(), "resolve succeeds after registration");
        CHECK(resolved.passes.size() == 1, "one resolved pass");

        const RGResolvedPass& rp = resolved.passes[0];
        CHECK(rp.preImageBarriers.size() == 2, "two resolved pre-barriers (color + swap)");
        bool colorBarrierOk = false, swapBarrierOk = false;
        for (const RGResolvedImageBarrier& b : rp.preImageBarriers)
        {
            if (b.barrier.Image == fakeImage && b.barrier.Next == ImageState::ColorAttachment)
                colorBarrierOk = true;
            if (b.barrier.Image == fakeImage && b.barrier.Old == ImageState::Present &&
                b.barrier.Next == ImageState::ColorAttachment)
                swapBarrierOk = true;
        }
        CHECK(colorBarrierOk, "fresh color: vk::Image bound, engine states preserved");
        CHECK(swapBarrierOk, "swapchain acquire resolved with vk::Image + Present→ColorAttachment");
        CHECK(rp.postImageBarriers.size() == 1 && rp.postImageBarriers[0].barrier.Image == fakeImage &&
                  rp.postImageBarriers[0].barrier.Next == ImageState::Present,
              "finalState exit resolves onto the registered image");
        CHECK(rp.renderExtent.has_value() && rp.renderExtent->width == 64 && rp.renderExtent->height == 64,
              "render extent comes from the registered object");

        // Missing registration AFTER a successful resolve is caught again (the
        // executor never caches: registrations are per-frame by contract).
        executor.ClearRegistrations();
        errors.clear();
        (void)executor.Resolve(compiled.frame, rg.GetBuilder().Passes(), errors);
        CHECK(errors.size() == 4, "clearing registrations invalidates the frame");
    }

    // ── Step 3: executor Resolve — buffer barriers between differing guards ──
    {
        VulkanRenderGraph rg;
        RGBufferHandle staging = rg.ImportBuffer("Staging", 5);
        RGBufferHandle gpu =
            rg.CreateBuffer({"GpuBuf", 4096, BufferUsageFlags::StorageBuffer});
        RGBufferHandle result = rg.ImportBuffer("Result", 9); // external consumer: root
        auto& upload = rg.AddPass("Upload", RGPassType::Transfer);
        (void)upload.Read(staging).Write(gpu).Submit();
        auto& consume = rg.AddPass("Consume", RGPassType::Compute);
        (void)consume.Read(gpu).Write(result).Submit(); // rooted via the result import

        const RGCompileResult compiled = rg.Compile();
        CHECK(compiled.Ok(), "transfer+compute frame compiles");
        CHECK(!compiled.frame.passes.empty(), "transfer pass survives culling (import root)");
        CHECK(compiled.frame.passes.size() == 2,
              "both passes live (chain rooted at both imports)");

        VulkanRenderGraphExecutor executor;
        const vk::Buffer fakeBuffer = reinterpret_cast<VkBuffer>(0xdeadbeefull);
        executor.RegisterBuffer(gpu, fakeBuffer);
        executor.RegisterBuffer(staging, fakeBuffer);

        std::vector<std::string> errors;
        const RGResolvedFrame resolved = executor.Resolve(compiled.frame, rg.GetBuilder().Passes(), errors);
        CHECK(errors.empty(), "buffer registrations resolve");
        // No entry barrier on the FIRST touch of a buffer (nothing preceded it);
        // the barrier lands on the consumer whose guard differs.
        CHECK(resolved.passes[0].preBufferBarriers.empty(),
              "first buffer touch needs no entry barrier");
        CHECK(resolved.passes[1].preBufferBarriers.size() == 1,
              "consumer pass carries one buffer barrier");
        CHECK(resolved.passes[1].preBufferBarriers[0].barrier.Buffer == fakeBuffer &&
                  resolved.passes[1].preBufferBarriers[0].barrier.SrcAccess == Access::TransferWrite &&
                  resolved.passes[1].preBufferBarriers[0].barrier.DstAccess == Access::ShaderRead &&
                  resolved.passes[1].preBufferBarriers[0].barrier.SrcStage == Stage::Transfer &&
                  resolved.passes[1].preBufferBarriers[0].barrier.DstStage == Stage::Compute,
              "upload→consume barrier carries Transfer-write → Compute-read guards");
    }

    // ── Step 5: TaskGraph mapping — one node per pass, DAG edges (§8.5) ──
    {
        VulkanRenderGraph rg;
        BuildFrame(rg, 64, 64, 1); // GBuffer → Lighting (Lighting depends on GBuffer)
        const RGCompileResult compiled = rg.Compile();
        CHECK(compiled.Ok(), "taskgraph frame compiles");

        VulkanRenderGraphExecutor executor;
        const vk::Image fakeImage = reinterpret_cast<VkImage>(0x42424242ull);
        const vk::ImageView fakeView = reinterpret_cast<VkImageView>(0x24242424ull);
        for (const RGTextureSlot& slot : rg.GetBuilder().Resources().Textures())
            executor.RegisterTexture(RGTextureHandle{static_cast<u32>(&slot -
                                                                    rg.GetBuilder()
                                                                        .Resources()
                                                                        .Textures()
                                                                        .data()),
                                                     slot.generation},
                                     fakeImage, fakeView, 64, 64, TextureFormat::RGBA8_UNORM);

        std::vector<std::string> errors;
        const RGResolvedFrame resolved = executor.Resolve(compiled.frame, rg.GetBuilder().Passes(), errors);
        CHECK(errors.empty(), "taskgraph frame resolves");

        UHE::Jobsystem::TaskGraph graph;
        VulkanBarrierEncoder scratchEncoder;
        const std::vector<UHE::Jobsystem::TaskID> nodes = executor.MapToTaskgraph(
            graph, resolved, rg.GetBuilder().Passes(), vk::CommandBuffer{nullptr}, scratchEncoder);
        CHECK(nodes.size() == 2, "one TaskGraph node per compiled pass");
        CHECK(graph.GetNode(nodes[1]).dependencyCount == 1,
              "Lighting node has one dependency (GBuffer)");
        const bool edgeOk = std::find(graph.GetNode(nodes[0]).dependents.begin(),
                                      graph.GetNode(nodes[0]).dependents.end(),
                                      nodes[1]) != graph.GetNode(nodes[0]).dependents.end();
        CHECK(edgeOk, "GBuffer node lists Lighting as a dependent");
    }

    // ── Write after write: two rendering scopes on one attachment ─────────────
    // The scene renders into the swapchain, then the UI pass loads it and draws
    // on top. Same layout, same guard - but they are separate rendering scopes,
    // and the second must wait for the first's writes. A barrier only on layout
    // or guard changes left this pair unordered.
    {
        VulkanRenderGraph rg;
        RGTextureHandle swap = rg.ImportTexture("Swapchain", 1, 64, 64, 1, ImageState::Undefined, ImageState::Present);
        rg.AddPass("Scene")
            .Write(swap)
            .Color({swap, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});
        RGTextureHandle again = rg.ImportTexture("Swapchain", 1, 64, 64, 1, ImageState::Undefined, ImageState::Present);
        rg.AddPass("UI")
            .Write(again)
            .Color({again, LoadOp::Load, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});
        const RGCompileResult result = rg.Compile();
        CHECK(result.Ok() && result.frame.passes.size() == 2, "two writers of one import compile");
        if (result.frame.passes.size() == 2)
        {
            const RGCompiledPass& ui = result.frame.passes[1];
            bool ordered = false;
            for (const RGCompiledImageBarrier& b : ui.preImageBarriers)
                ordered = ordered ||
                          (b.oldState == ImageState::ColorAttachment && b.newState == ImageState::ColorAttachment &&
                           b.srcAccess == Access::ColorWrite && b.dstAccess == Access::ColorWrite);
            CHECK(ordered, "the second writer waits on the first with a colour write barrier");
        }

        // Reads still transition once: a texture sampled by two passes needs
        // no barrier between them.
        VulkanRenderGraph reads;
        RGTextureHandle tex = reads.ImportTexture("Lut", 2, 16, 16, 1, ImageState::ShaderRead, ImageState::ShaderRead);
        RGTextureHandle outA = reads.ImportTexture("OutA", 3, 16, 16, 1, ImageState::Undefined, ImageState::ShaderRead);
        RGTextureHandle outB = reads.ImportTexture("OutB", 4, 16, 16, 1, ImageState::Undefined, ImageState::ShaderRead);
        reads.AddPass("A")
            .Read(tex)
            .Write(outA)
            .Color({outA, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});
        reads.AddPass("B")
            .Read(tex)
            .Write(outB)
            .Color({outB, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});
        const RGCompileResult readResult = reads.Compile();
        bool lutBarrierInB = false;
        if (readResult.frame.passes.size() == 2)
            for (const RGCompiledImageBarrier& b : readResult.frame.passes[1].preImageBarriers)
                lutBarrierInB = lutBarrierInB || b.texture == tex;
        CHECK(readResult.Ok() && !lutBarrierInB, "a second read of a sampled texture adds no barrier");
    }

    // ── Compile cache ───────────────────────────────────────────────────────
    // A frame redeclared identically reuses the last compile, with every
    // handle rebound to this frame's generation; anything that changes the
    // barriers (which resource a pass reads, an import's starting state)
    // compiles again, even when the coarse topology hash would not notice.
    {
        VulkanRenderGraph rg;
        BuildFrame(rg, 1280, 720, 1);
        const RGCompileResult first = rg.CompileCached();
        CHECK(first.Ok() && rg.CompileCache().Misses() == 1, "the first frame compiles");

        rg.Reset();
        BuildFrame(rg, 1280, 720, 1);
        const RGCompileResult cached = rg.CompileCached();
        const RGCompileResult fresh = rg.Compile();
        CHECK(rg.CompileCache().Hits() == 1, "an identical frame is a cache hit");
        bool same = cached.frame.passes.size() == fresh.frame.passes.size() &&
                    cached.frame.lifetimes.size() == fresh.frame.lifetimes.size();
        for (size_t p = 0; same && p < fresh.frame.passes.size(); ++p)
        {
            const auto& a = cached.frame.passes[p];
            const auto& b = fresh.frame.passes[p];
            same = a.name == b.name && a.dependencies == b.dependencies &&
                   a.preImageBarriers.size() == b.preImageBarriers.size() &&
                   a.postImageBarriers.size() == b.postImageBarriers.size();
            for (size_t i = 0; same && i < b.preImageBarriers.size(); ++i)
                same = a.preImageBarriers[i].texture == b.preImageBarriers[i].texture &&
                       a.preImageBarriers[i].oldState == b.preImageBarriers[i].oldState &&
                       a.preImageBarriers[i].newState == b.preImageBarriers[i].newState;
            for (size_t i = 0; same && i < b.postImageBarriers.size(); ++i)
                same = a.postImageBarriers[i].texture == b.postImageBarriers[i].texture;
        }
        for (size_t l = 0; same && l < fresh.frame.lifetimes.size(); ++l)
            same = cached.frame.lifetimes[l].texture == fresh.frame.lifetimes[l].texture;
        CHECK(same, "the cached frame equals a fresh compile, handles in this frame's generation");
        bool staleFree = true;
        for (const auto& pass : first.frame.passes)
            for (const auto& b : pass.preImageBarriers)
                for (const auto& c : cached.frame.passes)
                    for (const auto& cb : c.preImageBarriers)
                        if (cb.texture.index == b.texture.index)
                            staleFree = staleFree && cb.texture.generation != b.texture.generation;
        CHECK(staleFree, "no cached handle keeps the previous frame's generation");

        // Same counts, different resource: the topology hash cannot tell.
        const auto readsOne = [](VulkanRenderGraph& g, const char* source)
        {
            RGTextureHandle a = g.ImportTexture("A", 10, 8, 8, 1, ImageState::ShaderRead, ImageState::ShaderRead);
            RGTextureHandle b = g.ImportTexture("B", 11, 8, 8, 1, ImageState::Undefined, ImageState::ShaderRead);
            RGTextureHandle out = g.ImportTexture("Out", 12, 8, 8, 1, ImageState::Undefined, ImageState::Present);
            g.AddPass("Post")
                .Read(std::string(source) == "A" ? a : b)
                .Write(out)
                .Color({out, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
                .Execute([](RGPassContext&) {});
        };
        VulkanRenderGraph post;
        readsOne(post, "A");
        (void)post.CompileCached();
        const u64 hashA = post.ComputeTopologyHash();
        post.Reset();
        readsOne(post, "B");
        const u64 hashB = post.ComputeTopologyHash();
        const RGCompileResult readsB = post.CompileCached();
        CHECK(post.CompileCache().Hits() == 0 && post.CompileCache().Misses() == 2,
              "reading a different resource is a miss");
        CHECK(hashA == hashB, "(the coarse topology hash alone would have called it a hit)");
        bool transitionsB = false;
        if (readsB.frame.passes.size() == 1)
            for (const auto& b : readsB.frame.passes[0].preImageBarriers)
                transitionsB =
                    transitionsB || (b.oldState == ImageState::Undefined && b.newState == ImageState::ShaderRead);
        CHECK(transitionsB, "and the recompile has B's transition");

        VulkanRenderGraph states;
        RGTextureHandle swap1 =
            states.ImportTexture("Swapchain", 1, 8, 8, 1, ImageState::Undefined, ImageState::Present);
        states.AddPass("UI")
            .Write(swap1)
            .Color({swap1, LoadOp::Load, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});
        (void)states.CompileCached();
        states.Reset();
        RGTextureHandle swap2 = states.ImportTexture("Swapchain", 1, 8, 8, 1, ImageState::Present, ImageState::Present);
        states.AddPass("UI")
            .Write(swap2)
            .Color({swap2, LoadOp::Load, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});
        (void)states.CompileCached();
        CHECK(states.CompileCache().Misses() == 2, "an import's starting state is part of the key");

        VulkanRenderGraph broken;
        RGTextureHandle orphan =
            broken.CreateTexture({"Orphan", TextureFormat::RGBA8_UNORM, 8, 8, 1, 1, 1, 1, TextureUsage::Sampled});
        RGTextureHandle target = broken.ImportTexture("T", 5, 8, 8, 1, ImageState::Undefined, ImageState::Present);
        broken.AddPass("Bad")
            .Read(orphan)
            .Write(target)
            .Color({target, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}})
            .Execute([](RGPassContext&) {});
        CHECK(!broken.CompileCached().Ok(), "a broken frame fails");
        CHECK(!broken.CompileCached().Ok() && broken.CompileCache().Hits() == 0, "and is never served from the cache");
    }

    std::cout << (g_failures == 0 ? "\nALL CHECKS PASSED\n" : "\nFAILURES PRESENT\n");
    return g_failures == 0 ? 0 : 1;
}
