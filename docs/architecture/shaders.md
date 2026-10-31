# Shaders

**Status:** IMPLEMENTED — offline compilation, SPIR-V reflection and hot reload
are in the tree and covered by `tests/shader`. `UHE/src/Platform/Vulkan/ShaderSystem/VulkanShaderManager.*`
is still an unrelated empty stub; the live path is the one below.

**Issues:** #29 (materials), #7 (renderer checklist).

**Code:** `tools/shaderc/main.cpp`, `UHE/src/UHE/Renderer/{ShaderCache,ShaderReflection,ShaderResources,SlangCompiler}.{h,cpp}`,
`UHE_EDITOR/assets/shaders/variants.txt`, `tests/shader/`.

## The path a shader takes

```
assets/shaders/Basic3D.slang                 # what a human edits
        │  uhe_shaderc  (build step, "uhe_shaders" target)
        ▼
assets/shaders/Basic3D.vertex.default.spv    # what the engine reads
assets/shaders/Basic3D.fragment.default.spv
assets/shaders/Basic3D.fragment.no_entity_id.spv
        │  ShaderCache::Load(source, variant)
        ▼
ShaderCache::Shader { SPIRV, ShaderReflection }   # bytes + what they declare
        │  CreateStageShader / ReflectedVertexLayout / ReflectedPushConstantSize
        ▼
RHI::ShaderHandle + RHI::GraphicsPipelineDesc
```

The engine used to run the Slang compiler inside startup — a global session, a
module load and a link per entry point, on the critical path of every launch.
The build does that work now and the runtime reads bytes, so startup costs a file
read per shader. The runtime still links Slang: that is what compiles a shader
the build has not seen, and what hot reload uses.

## Offline compilation

`uhe_shaderc --shaders <dir> --source <file>...` compiles each source once per
variant and writes `<stem>.<stage>.<variant>.spv` beside it. The `uhe_shaders`
target runs it on every build; up-to-dateness is decided per source and variant by
mtime, so a build that changed nothing compiles nothing (~0.5 s). A source that
fails to compile fails the build, which is the point: the old failure mode was a
broken shader surfacing at engine startup, or worse, a wrong one drawing garbage.

The source list is explicit in `tools/CMakeLists.txt` (`UHE_SHADER_SOURCES`)
because the shader directory also holds library fragments — `PBRBasic.slang` is a
BRDF library with no entry point, and asking it for `vertexMain` is a question
with no answer rather than a broken shader. A runtime that asks for an unlisted
source still works: `ShaderCache` compiles it and writes the artifact, and says so
in the log.

The artifacts are build outputs and are git-ignored. `Texture.vert.spv` and
`Texture.frag.spv` predate this system and are still tracked; nothing loads them.

## Variants

A variant is a named define set, declared once in `UHE_EDITOR/assets/shaders/variants.txt`:

```
default:
no_entity_id: UHE_NO_ENTITY_ID=1
```

`default` always exists, even without the file. The build compiles every source
once per variant and the runtime asks for a variant *by name*
(`ShaderCache::Load(path, "no_entity_id")`), so a define is written down once
instead of at each call site. The rule it serves — one source, two pipelines that
differ in an output — is the reason the swapchain pass can reuse `Basic3D.slang`
without writing the entity-id target it has no attachment for.

## Reflection

`ReflectSPIRV` reads a single-entry-point module and reports:

| Field | From |
|---|---|
| stage, entry point name | `OpEntryPoint` (Slang always emits `main`) |
| workgroup size | `OpExecutionMode LocalSize` / `LocalSizeId` |
| push constant bytes | the `PushConstant` variable's struct: member offsets + std430 sizes |
| descriptor bindings | variables in `Uniform` / `StorageBuffer` / `UniformConstant`, with set, binding, kind and array size |
| vertex inputs | `Input` variables in the entry point's interface with a `Location`, in location order |

What it is *for*: the vertex layout and the push constant range of a pipeline are
now the shader's own words. They used to be a list in `Renderer3D.cpp` kept in
step with `Basic3D.slang` by a comment ("Location 5 (a_Tangent) must match
VertexInput in Basic3D.slang"), where a drifted entry drew garbage instead of
failing. `tests/shader` pins the reflected layout against the values the renderers
used to hard-code, so an edit that changes the interface fails a test rather than
a frame.

Two rules keep it honest:

- The severity-style mirror of Vulkan constants is `static_assert`ed where both
  sides are visible (`VulkanInstance.cpp`), never assumed.
- Malformed input is refused, not read: a blob with the wrong magic, a truncated
  instruction or an instruction that claims more words than the blob has yields
  `Valid == false` with a reason, and the caller keeps its null shader.

Nothing consumes the compute workgroup size yet, and descriptor bindings are
reported but not used to build layouts: the engine's descriptors are bindless and
fixed. Both are there because a module that cannot describe itself cannot be
checked.

## Hot reload

`ShaderCache::Tick()` (called once per frame from `Application`) polls the source
mtimes of loaded shaders at most four times a second, and recompiles whatever
changed — in-process, then writing the artifact back so the next launch is on the
fast path. It also notices an artifact a build replaced under a running engine and
reads it back rather than compiling again.

Listeners are notified with the reloaded handle; both renderers register one that
waits for the device to be idle, destroys their shader modules and pipelines, and
creates them again (`Renderer3D::ReloadShaders` / `Renderer2D::ReloadShaders`).
Waiting is not optional: frames already queued reference the objects being
replaced.

Off by default — a shipped build has no sources to watch. The editor and the
sandbox enable it; the editor's **File > Reload Shaders** forces a recompile of
everything, for the cases an mtime check cannot see (a restored file, a changed
define).

## Adding a shader or a variant

1. Write the source with the engine's fixed entry point names: `vertexMain`,
   `fragmentMain`, `computeMain`. Slang emits them as `main`, which is what the
   pipeline names.
2. Add it to `UHE_SHADER_SOURCES` in `tools/CMakeLists.txt` (the build's list) and
   load it with `ShaderCache::Get().Load(path)` at the call site.
3. A new define set goes in `variants.txt`; the build picks it up and the runtime
   asks for it by name.
4. `tests/shader` compiles every source the engine loads; a shader with an
   interface the renderers depend on belongs in its case list.

## Verification

- `ctest -L unit` — `tests/shader` compiles the real sources, reflects them,
  compares each artifact against a fresh compile, and drives a hot reload against
  a throwaway copy.
- `script/check_validation.sh` — the GPU side: the reflected layouts produce a
  valid pipeline, and a frame that used them is not a validation error.
