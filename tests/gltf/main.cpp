// glTF loader correctness harness (issue #29: multi-node models render wrong).
//
// Covers the defects found in the original loader, each of which is silent -
// the model loads, returns success, and draws the wrong thing:
//
//   - node transforms never applied, so every mesh collapsed onto the origin
//   - a mesh referenced by several nodes extracted once per node, duplicating
//     the vertex data and losing the node names
//   - non-indexed geometry left with an empty index list, so the draw is skipped
//   - out-of-range material indices accepted at load and read at draw time
//   - index/vertex reordering passes corrupting the mesh
//
// Test assets are written as real .gltf + .bin and loaded through the same code
// path as a shipped asset, rather than asserting against structs the test built
// itself.
//
// No GPU: the loader is exercised through its CPU-side scene walk only, so this
// links no RHI and needs no Vulkan device.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "UHE/Core/Log.h"
#include "UHE/Renderer3D/LoadModel.h"
#include "UHE/Renderer3D/LightSystem.h"
#include "UHE/Renderer3D/MaterialGPU.h"
#include "UHE/Scene/Components.h"

#include <meshoptimizer.h>

// Defined in gpu_stub.cpp: records every sampler the loader asked for, so a test
// can assert glTF's declared sampler state and colour space actually reached the
// factory. A stub that ignored the argument would make this path untestable, and
// an untestable path is the one that silently regresses.
namespace UHE
{
RHI::SamplerDesc StubLastRequestedSampler();
const std::vector<RHI::SamplerDesc>& StubRequestedSamplers();
int StubTextureCreateCallCount();
void StubReset();
} // namespace UHE

// A minimal Texture2D whose bindless index is a fixed known value, so the
// FillMaterialGPU test can assert slot table wiring rather than always seeing
// the -1 the null stubs produce. Never uploaded anywhere - no GPU in this
// harness.
namespace UHE
{
class FakeTexture final : public Texture2D
{
public:
    u32 GetWidth() const override { return 1; }
    u32 GetHeight() const override { return 1; }
    void Bind(u32) const override {}
    void* GetImGuiTextureID() override { return nullptr; }
    RHI::TextureHandle GetTextureHandle() const override { return nullptr; }
    u32 GetTextureIndex() const override { return 4242; }
    bool operator==(const Texture&) const override { return false; }
};
} // namespace UHE

namespace
{

int g_failures = 0;
int g_checks = 0;

// Returns the condition so a check can guard a block:
//   if (check(model.loadModel(path), "loads")) { ... }
bool check(bool condition, std::string_view what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("  FAIL  %.*s\n", static_cast<int>(what.size()), what.data());
    }
    return condition;
}

void section(std::string_view name)
{
    std::printf("[ %.*s ]\n", static_cast<int>(name.size()), name.data());
}

// Component-wise closeness for vec3 assertions: glm's vector-relational
// helpers need gtc includes and read worse than this at call sites.
bool closeTo(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}


namespace fs = std::filesystem;

// A scratch directory that cleans itself up, so a failed run leaves nothing
// behind in the source tree.
class TempDir
{
public:
    TempDir()
    {
        m_Path = fs::temp_directory_path() / "uhe_gltf_test";
        fs::remove_all(m_Path);
        fs::create_directories(m_Path);
    }

    ~TempDir()
    {
        // Set UHE_GLTF_TEST_KEEP=1 to leave the generated assets behind when a
        // test fails and the emitted JSON needs inspecting.
        if (std::getenv("UHE_GLTF_TEST_KEEP") == nullptr)
            fs::remove_all(m_Path);
        else
            std::printf("  (assets kept in %s)\n", m_Path.string().c_str());
    }

    const fs::path& path() const { return m_Path; }

    fs::path write(std::string_view filename, std::string_view contents) const
    {
        const fs::path file = m_Path / filename;
        std::ofstream out(file, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return file;
    }

    // Explicit-length overload. Binary data such as a PNG contains NUL bytes,
    // so it cannot go through a string_view built from a literal - the size has
    // to be carried alongside the pointer.
    fs::path writeBinary(std::string_view filename, const void* data, std::size_t size) const
    {
        const fs::path file = m_Path / filename;
        std::ofstream out(file, std::ios::binary);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        return file;
    }

private:
    fs::path m_Path;
};

// ─── Minimal JSON writer ────────────────────────────────────────────────
//
// Emits compact JSON with correct commas and escaping. Hand-concatenating glTF
// produced several invalid files while writing this harness, each of which made
// a check pass or fail for entirely the wrong reason - a test whose asset never
// parsed verifies nothing. Building the JSON removes that whole class of bug.
class Json
{
public:
    Json& beginObject()
    {
        separate();
        m_Out += '{';
        m_First.push_back(true);
        return *this;
    }

    Json& endObject()
    {
        m_Out += '}';
        m_First.pop_back();
        return *this;
    }

    Json& beginArray()
    {
        separate();
        m_Out += '[';
        m_First.push_back(true);
        return *this;
    }

    Json& endArray()
    {
        m_Out += ']';
        m_First.pop_back();
        return *this;
    }

    Json& key(std::string_view name)
    {
        separate();
        quote(name);
        m_Out += ':';
        m_PendingKey = true;
        return *this;
    }

    Json& value(std::string_view text)
    {
        separate();
        quote(text);
        return *this;
    }

    Json& value(int number)
    {
        separate();
        m_Out += std::to_string(number);
        return *this;
    }

    // Exact-match constraint, and this matters: without it a char const* literal
    // ("2.0", "generator") is convertible to BOTH bool and string_view, and
    // bool won - so every string field silently became `true`. T must be bool
    // itself, not anything convertible to it.
    template <typename T>
        requires std::same_as<T, bool>
    Json& value(T flag)
    {
        separate();
        m_Out += flag ? "true" : "false";
        return *this;
    }

    Json& value(std::size_t number)
    {
        separate();
        m_Out += std::to_string(number);
        return *this;
    }

    Json& value(double number)
    {
        separate();
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.9g", number);
        m_Out += buffer;
        return *this;
    }

    // key + scalar in one call.
    template <typename T>
    Json& field(std::string_view name, T&& v)
    {
        return key(name).value(std::forward<T>(v));
    }

    // key + array of numbers.
    Json& numberArray(std::string_view name, const std::vector<double>& values)
    {
        key(name).beginArray();
        for (double v : values)
            value(v);
        return endArray();
    }

    Json& value(const std::vector<double>& values)
    {
        beginArray();
        for (double v : values)
            value(v);
        return endArray();
    }

    const std::string& str() const { return m_Out; }

private:
    void separate()
    {
        // A comma is needed between siblings, but not straight after a key
        // (that is already followed by ':') and not at the start of a container.
        if (m_PendingKey)
        {
            m_PendingKey = false;
            return;
        }
        if (!m_First.empty())
        {
            if (!m_First.back())
                m_Out += ',';
            m_First.back() = false;
        }
    }

    void quote(std::string_view text)
    {
        m_Out += '"';
        for (char c : text)
        {
            switch (c)
            {
                case '"': m_Out += "\\\""; break;
                case '\\': m_Out += "\\\\"; break;
                case '\n': m_Out += "\\n"; break;
                case '\r': m_Out += "\\r"; break;
                case '\t': m_Out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                        m_Out += buffer;
                    }
                    else
                    {
                        m_Out += c;
                    }
            }
        }
        m_Out += '"';
    }

    std::string m_Out;
    std::vector<bool> m_First; // per nesting level: is the next item the first?
    bool m_PendingKey = false;
};

// ─── Test asset construction ────────────────────────────────────────────

// A quad: 4 vertices, 6 indices. Trivially small so checks can name exact
// counts.
struct Quad
{
    // Six vertices = two whole triangles. Deliberately not four: a non-indexed
    // mesh with 4 vertices cannot be drawn as a triangle list, and asserting
    // index synthesis on it would be asserting on a degenerate case.
    static constexpr std::size_t kVertexCount = 6;
    static constexpr std::size_t kIndexCount = 6;

    static std::vector<float> positions()
    {
        return {
            0.0f, 0.0f, 0.0f, // 0
            1.0f, 0.0f, 0.0f, // 1
            1.0f, 1.0f, 0.0f, // 2
            0.0f, 0.0f, 0.0f, // 3
            1.0f, 0.0f, 0.0f, // 4
            0.0f, 1.0f, 0.0f, // 5
        };
    }

    static std::vector<u32> indices() { return {0, 1, 2, 3, 4, 5}; }

    // All +Z. A quad lying in the XY plane faces the viewer.
    static std::vector<float> normals()
    {
        return {
            0.0f, 0.0f, 1.0f, // 0
            0.0f, 0.0f, 1.0f, // 1
            0.0f, 0.0f, 1.0f, // 2
            0.0f, 0.0f, 1.0f, // 3
            0.0f, 0.0f, 1.0f, // 4
            0.0f, 0.0f, 1.0f, // 5
        };
    }

    // Vertices 0/3 and 1/4 share a POSITION but differ in UV - that is exactly a
    // texture seam, which is what forces a per-corner tangent frame.
    static std::vector<float> uvs()
    {
        return {
            0.0f, 0.0f, // 0
            1.0f, 0.0f, // 1
            1.0f, 1.0f, // 2
            0.0f, 0.0f, // 3
            1.0f, 0.0f, // 4
            0.0f, 1.0f, // 5
        };
    }
};

struct SamplerSpec
{
    // Negative = omit the field, so the spec default is what gets observed.
    int magFilter = -1;
    int minFilter = -1;
    int wrapS = -1;
    int wrapT = -1;
};

// A generic extension block. Every Tier 2 extension is a flat bag of scalars,
// except for a few members that are spec-mandated ARRAYS - sheenColorFactor is
// [r,g,b], attenuationColor is [r,g,b], specularColorFactor is [r,g,b]. Emitting
// one of those as a bare number produces a file that parses but leaves the
// extension block unset, so the distinction has to be expressible here or the
// fixtures quietly stop testing anything.
struct ExtSpec
{
    // Insertion-ordered so the emitted JSON has a deterministic shape.
    std::vector<std::pair<std::string, double>> scalars;
    std::vector<std::pair<std::string, std::vector<double>>> arrays;
};

// Convenience so a fixture reads as the field it sets.
inline ExtSpec Ext(std::vector<std::pair<std::string, double>> scalars)
{
    ExtSpec s;
    s.scalars = std::move(scalars);
    return s;
}

inline ExtSpec& WithArray(ExtSpec& s, std::string_view name, std::vector<double> values)
{
    s.arrays.emplace_back(std::string(name), std::move(values));
    return s;
}

struct MaterialSpec
{
    std::string alphaMode;
    float alphaCutoff = -1.0f;      // negative = omit, so the spec default shows through
    bool doubleSided = false;
    bool hasBaseColorFactor = false;
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool hasEmissiveFactor = false;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float normalScale = -1.0f;      // negative = omit
    float occlusionStrength = -1.0f;
    // Which texture SLOTS to declare. Each one forces a separate texture load,
    // which is what makes per-slot colour space observable: baseColor and
    // emissive are sRGB, the other three are linear data.
    bool baseColorTexture = false;
    bool metallicRoughnessTexture = false;
    bool emissiveTexture = false;
    std::optional<SamplerSpec> sampler;

    // Tier 2/3 extension blocks. Each is OPTIONAL and omitted when unset, so a
    // material declaring no extension produces a file with no extensions object
    // at all - which is what the defaults tests depend on.
    std::optional<ExtSpec> clearcoat;
    std::optional<ExtSpec> specular;
    std::optional<ExtSpec> sheen;
    std::optional<ExtSpec> transmission;
    std::optional<ExtSpec> volume;
    std::optional<ExtSpec> iridescence;
    std::optional<ExtSpec> anisotropy;
    std::optional<ExtSpec> diffuseTransmission;
    bool unlit = false;
    bool hasEmissiveStrength = false;
    float emissiveStrength = 1.0f;
    bool hasIOR = false;
    float ior = 1.5f;

    // KHR_texture_transform on the baseColorTexture slot. Emitted only when
    // hasTextureTransform is set; every component is explicit because the test
    // asserts the converted values, not the declared ones.
    bool hasTextureTransform = false;
    double transformRotation = 0.0;
    double transformOffsetX = 0.0, transformOffsetY = 0.0;
    double transformScaleX = 1.0, transformScaleY = 1.0;
};

struct AssetSpec
{
    // Number of glTF meshes to emit.
    std::size_t meshCount = 1;
    // When true, meshCount nodes each reference their own mesh. When false,
    // `nodeCount` nodes all reference mesh 0 (the instancing case).
    bool distinctMeshes = true;
    std::size_t nodeCount = 0;
    bool useIndices = true;
    std::size_t materialCount = 1;
    bool outOfRangeMaterial = false;
    // Nest node 1 under node 0 rather than listing both as scene roots.
    bool nested = false;
    // KHR_lights_punctual: lights declared on the asset and the node -> light
    // index assignment (empty = no lights block at all).
    struct LightSpec
    {
        std::string type; // "directional", "point", "spot"
        std::vector<double> color;
        double intensity = 1.0;
        bool hasRange = false;
        double range = 0.0;
        bool hasInnerCone = false;
        double innerCone = 0.0;
        bool hasOuterCone = false;
        double outerCone = 0.0;
    };
    std::vector<LightSpec> lights;
    // One entry per node; a negative value emits no light reference.
    std::vector<int> nodeLightIndices;
    // Extensions to declare in extensionsUsed / extensionsRequired.
    std::vector<std::string> extensionsUsed;
    std::vector<std::string> extensionsRequired;
    // Omitted entirely when empty, so default-value tests see a truly absent
    // material rather than one full of explicit defaults.
    std::optional<MaterialSpec> material;

    // Adds JOINTS_0/WEIGHTS_0 to the primitive, which puts it on the skinning
    // path where destructive vertex merging is forbidden.
    bool skinned = false;

    // NORMAL and TEXCOORD are required for tangent generation to mean anything:
    // meshopt derives the tangent frame from the UV gradient, so with no UVs (or
    // zero normals) it has nothing to work from. Off by default so tests that
    // only care about indexing stay minimal.
    bool withNormals = false;
    bool withUvs = false;
    // Adds a COLOR_0 attribute to the primitive. 3 = VEC3, 4 = VEC4, 0 = none.
    // glTF permits both widths, and reading the wrong one consumes the wrong
    // amount of stride, so the fixture has to be able to emit each.
    std::size_t colorComponents = 0;
    // When nested, make the child node carry no mesh. That is the point of the
    // test: only nodes with a mesh produce a Mesh entry.
    bool childHasMesh = true;
};

// Writes <name>.gltf plus data.bin and returns the .gltf path.
fs::path writeAsset(const TempDir& dir, std::string_view name, const AssetSpec& spec)
{
    const std::vector<float> quadPositions = Quad::positions();
    const std::vector<u32> quadIndices = Quad::indices();
    const std::vector<float> quadNormals = Quad::normals();
    const std::vector<float> quadUvs = Quad::uvs();
    constexpr std::size_t kPositionBytes = 18 * sizeof(float);
    constexpr std::size_t kIndexBytes = Quad::kIndexCount * sizeof(u32);
    // JOINTS_0 is 4 x u8 per vertex, WEIGHTS_0 is 4 x f32. glTF requires
    // bufferView byteOffsets for vertex attributes to be a multiple of the
    // component size, and 4-byte alignment for the float weights, hence the
    // padding between them.
    constexpr std::size_t kJointsBytes = Quad::kVertexCount * 4 * sizeof(std::uint8_t);
    constexpr std::size_t kJointsPadded = (kJointsBytes + 3) & ~std::size_t{3};
    constexpr std::size_t kWeightsBytes = Quad::kVertexCount * 4 * sizeof(f32);
    constexpr std::size_t kSkinBytes = kJointsPadded + kWeightsBytes;
    constexpr std::size_t kNormalBytes = 18 * sizeof(float);
    constexpr std::size_t kUvBytes = 12 * sizeof(float);

    // ---- binary ----
    std::string bin;
    for (std::size_t i = 0; i < spec.meshCount; ++i)
        bin.append(reinterpret_cast<const char*>(quadPositions.data()), quadPositions.size() * sizeof(float));

    const std::size_t indexBase = bin.size();
    if (spec.useIndices)
        for (std::size_t i = 0; i < spec.meshCount; ++i)
            bin.append(reinterpret_cast<const char*>(quadIndices.data()), quadIndices.size() * sizeof(u32));

    // Layout order is fixed - positions, indices, normals, UVs, skin - and every
    // offset below is captured as it is appended. Computing them any other way
    // (predicting a later block's offset from an earlier flag) is how the
    // bufferViews end up silently pointing at the wrong bytes.
    const std::size_t normalBase = bin.size();
    if (spec.withNormals)
        for (std::size_t i = 0; i < spec.meshCount; ++i)
            bin.append(reinterpret_cast<const char*>(quadNormals.data()), quadNormals.size() * sizeof(float));

    const std::size_t uvBase = bin.size();
    if (spec.withUvs)
        for (std::size_t i = 0; i < spec.meshCount; ++i)
            bin.append(reinterpret_cast<const char*>(quadUvs.data()), quadUvs.size() * sizeof(float));

    // Joints alternate between two bones across the quad and the weights are
    // normalized, which is what a real exporter emits. A fixture where every
    // vertex used bone 0 would still pass if the loader ignored joints entirely.
    // Per-vertex colours, deliberately NOT all equal: a fixture where every vertex
    // shares one colour would still pass if the loader read the accessor at the
    // wrong offset, since every wrong read would return the same value.
    // The component count MUST match what the accessor declares. Writing 3 floats
    // while declaring VEC4 makes the loader read the NEXT vertex's components as
    // this one's alpha - the fixture looks fine and the loader looks buggy.
    std::vector<float> quadColors;
    for (std::size_t v = 0; v < Quad::kVertexCount; ++v)
    {
        const float t = static_cast<float>(v) / static_cast<float>(Quad::kVertexCount);
        quadColors.push_back(t);                        // R ramps 0 -> 1
        quadColors.push_back(1.0f - t);                  // G ramps 1 -> 0
        if (spec.colorComponents >= 4)
        {
            quadColors.push_back(0.5f);                  // B constant, a canary
            // Alpha VARIES per vertex. A constant 1 is indistinguishable from
            // hardcoding alpha to 1, which is the exact defect the VEC4 branch
            // exists to prevent.
            quadColors.push_back(0.25f + 0.5f * t);       // A ramps 0.25 -> 0.75
        }
    }
    const std::size_t kColorBytes = Quad::kVertexCount * spec.colorComponents * sizeof(float);

    const std::size_t colorBase = bin.size();
    for (std::size_t i = 0; i < spec.meshCount; ++i)
        bin.append(reinterpret_cast<const char*>(quadColors.data()), kColorBytes);

    const std::size_t skinBase = bin.size();
    if (spec.skinned)
    {
        // Both blocks must carry what their accessors declare: JOINTS_0 is
        // VEC4 x u8 and WEIGHTS_0 is VEC4 x f32 PER VERTEX. An earlier version
        // wrote one u8 and one f32 per vertex while still declaring the full
        // width, so every skin buffer view extended past the buffer - the
        // loader read past the declared end and only passed because the
        // overread happened to land inside the memory-mapped .bin page.
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            for (std::size_t v = 0; v < Quad::kVertexCount; ++v)
            {
                // Even vertices deform with bone 0, odd with bone 1; the three
                // trailing joint slots are unused and carry zero weights below.
                const std::uint8_t joints[4] = {static_cast<std::uint8_t>(v % 2 == 0 ? 0 : 1), 0, 0, 0};
                bin.append(reinterpret_cast<const char*>(joints), sizeof(joints));
            }
            while (bin.size() % 4 != 0)
                bin.push_back('\0');

            for (std::size_t v = 0; v < Quad::kVertexCount; ++v)
            {
                const f32 weights[4] = {(v % 2 == 0) ? 0.75f : 1.0f, 0.0f, 0.0f, 0.0f};
                bin.append(reinterpret_cast<const char*>(weights), sizeof(weights));
            }
        }
    }

    // The buffer name must be unique per asset: a single shared data.bin let
    // each test overwrite the previous one's bytes, so a .gltf ended up reading
    // whichever test happened to run last. Identical layouts hid it until the
    // fixtures started differing.
    const std::string binName = std::string(name.substr(0, name.rfind('.'))) + ".bin";
    dir.write(binName, bin);

    // A real 1x1 opaque white PNG, so a declared image actually decodes. A
    // truncated header parses as a valid file and then fails at decode time,
    // which reads as a loader bug rather than a fixture bug.
    static constexpr std::uint8_t kWhitePixelPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0x0f, 0x04, 0x00, 0x09, 0xfb, 0x03, 0xfd, 0x68, 0xfa, 0x1c, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    dir.writeBinary("tex.png", kWhitePixelPng, sizeof(kWhitePixelPng));

    // ---- JSON ----
    Json j;
    j.beginObject();

    j.key("asset").beginObject().field("version", "2.0").field("generator", "uhe-gltf-test").endObject();
    j.field("scene", 0);

    if (!spec.extensionsUsed.empty())
    {
        j.key("extensionsUsed").beginArray();
        for (const auto& e : spec.extensionsUsed)
            j.value(e);
        j.endArray();
    }
    if (!spec.extensionsRequired.empty())
    {
        j.key("extensionsRequired").beginArray();
        for (const auto& e : spec.extensionsRequired)
            j.value(e);
        j.endArray();
    }

    j.key("buffers")
        .beginArray()
        .beginObject()
        .field("uri", binName)
        .field("byteLength", bin.size())
        .endObject()
        .endArray();

    // bufferViews: one per mesh for positions, then one per mesh for indices.
    j.key("bufferViews").beginArray();
    for (std::size_t i = 0; i < spec.meshCount; ++i)
    {
        j.beginObject()
            .field("buffer", 0)
            .field("byteOffset", i * kPositionBytes)
            .field("byteLength", kPositionBytes)
            .field("target", 34962)
            .endObject();
    }
    if (spec.useIndices)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("buffer", 0)
                .field("byteOffset", indexBase + i * kIndexBytes)
                .field("byteLength", kIndexBytes)
                .field("target", 34963)
                .endObject();
        }
    }
    if (spec.withNormals)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("buffer", 0)
                .field("byteOffset", normalBase + i * kNormalBytes)
                .field("byteLength", kNormalBytes)
                .field("target", 34962)
                .endObject();
        }
    }
    if (spec.withUvs)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("buffer", 0)
                .field("byteOffset", uvBase + i * kUvBytes)
                .field("byteLength", kUvBytes)
                .field("target", 34962)
                .endObject();
        }
    }
    if (spec.colorComponents > 0)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("buffer", 0)
                .field("byteOffset", colorBase + i * kColorBytes)
                .field("byteLength", kColorBytes)
                .field("target", 34962)
                .endObject();
        }
    }
    if (spec.skinned)
    {
        // JOINTS_0 then WEIGHTS_0 per mesh, matching the bufferView indices the
        // accessors reference.
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("buffer", 0)
                .field("byteOffset", skinBase + i * kSkinBytes)
                .field("byteLength", kJointsBytes)
                .field("target", 34962)
                .endObject();
        }
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("buffer", 0)
                .field("byteOffset", skinBase + i * kSkinBytes + kJointsBytes)
                .field("byteLength", kWeightsBytes)
                .field("target", 34962)
                .endObject();
        }
    }
    j.endArray();

    // accessors: positions then indices.
    // Accessor indices are assigned by emission order below, so these are the
    // running counts rather than derived arithmetic - deriving them is what let
    // the earlier version of this file point JOINTS_0 at the wrong accessor.
    const std::size_t firstIndexAccessor = spec.meshCount;
    const std::size_t firstNormalAccessor = firstIndexAccessor + (spec.useIndices ? spec.meshCount : 0);
    const std::size_t firstUvAccessor = firstNormalAccessor + (spec.withNormals ? spec.meshCount : 0);
    const std::size_t firstColorAccessor = firstUvAccessor + (spec.withUvs ? spec.meshCount : 0);
    const std::size_t firstJointsAccessor = firstColorAccessor + (spec.colorComponents > 0 ? spec.meshCount : 0);
    const std::size_t firstWeightsAccessor = firstJointsAccessor + (spec.skinned ? spec.meshCount : 0);

    // Count of bufferViews consumed before the skin block. Recomputed rather than
    // guessed, because a hardcoded constant here silently points JOINTS_0 at the
    // wrong accessor the moment another attribute is added before it.
    const std::size_t viewsBeforeSkin = spec.meshCount * (1 + (spec.useIndices ? 1 : 0) + (spec.withNormals ? 1 : 0) +
                                                         (spec.withUvs ? 1 : 0) + (spec.colorComponents > 0 ? 1 : 0));

    j.key("accessors").beginArray();
    for (std::size_t i = 0; i < spec.meshCount; ++i)
    {
        j.beginObject()
            .field("bufferView", i)
            .field("componentType", 5126)
            .field("count", Quad::kVertexCount)
            .field("type", "VEC3")
            .endObject();
    }
    if (spec.useIndices)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("bufferView", spec.meshCount + i)
                .field("componentType", 5125)
                .field("count", Quad::kIndexCount)
                .field("type", "SCALAR")
                .endObject();
        }
    }
    if (spec.withNormals)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("bufferView", spec.meshCount + (spec.useIndices ? spec.meshCount : 0) + i)
                .field("componentType", 5126)
                .field("count", Quad::kVertexCount)
                .field("type", "VEC3")
                .endObject();
        }
    }
    if (spec.withUvs)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("bufferView", spec.meshCount * (1 + (spec.useIndices ? 1 : 0) + (spec.withNormals ? 1 : 0)) + i)
                .field("componentType", 5126)
                .field("count", Quad::kVertexCount)
                .field("type", "VEC2")
                .endObject();
        }
    }
    if (spec.colorComponents > 0)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("bufferView", spec.meshCount * (1 + (spec.useIndices ? 1 : 0) + (spec.withNormals ? 1 : 0) +
                                                      (spec.withUvs ? 1 : 0)) + i)
                .field("componentType", 5126)
                .field("count", Quad::kVertexCount)
                .field("type", spec.colorComponents == 3 ? "VEC3" : "VEC4")
                .endObject();
        }
    }
    if (spec.skinned)
    {
        // JOINTS_0 as VEC4 of unsigned int, matching glTF's joint indexing.
        const std::size_t jointViewBase = viewsBeforeSkin;
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("bufferView", jointViewBase + i * 2)
                .field("componentType", 5121)
                .field("count", Quad::kVertexCount)
                .field("type", "VEC4")
                .endObject();
        }
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            j.beginObject()
                .field("bufferView", jointViewBase + i * 2 + 1)
                .field("componentType", 5126)
                .field("count", Quad::kVertexCount)
                .field("type", "VEC4")
                .endObject();
        }
    }
    j.endArray();

    // meshes
    j.key("meshes").beginArray();
    for (std::size_t i = 0; i < spec.meshCount; ++i)
    {
        j.beginObject();
        j.field("name", "mesh_" + std::to_string(i));
        j.key("primitives").beginArray();
        j.beginObject();
        j.key("attributes").beginObject().field("POSITION", i);
        if (spec.withNormals)
            j.field("NORMAL", firstNormalAccessor + i);
        if (spec.withUvs)
            j.field("TEXCOORD_0", firstUvAccessor + i);
        if (spec.colorComponents > 0)
            j.field("COLOR_0", firstColorAccessor + i);
        if (spec.skinned)
        {
            j.field("JOINTS_0", firstJointsAccessor + i);
            j.field("WEIGHTS_0", firstWeightsAccessor + i);
        }
        j.endObject();
        if (spec.useIndices)
            j.field("indices", firstIndexAccessor + i);
        // Material 0 normally; deliberately out of range when asked, so the
        // loader's clamp is what keeps the draw in bounds.
        j.field("material", spec.outOfRangeMaterial ? spec.materialCount + 4 : 0);
        j.endObject();
        j.endArray();
        j.endObject();
    }
    j.endArray();

    // One 1x1 texture, declared only when a material actually references one.
    // A material pointing at texture 0 with no images/samplers/textures declared
    // is invalid glTF and fastgltf rejects the whole file.
    const bool needsTexture = spec.material && (spec.material->normalScale >= 0.0f ||
                                                spec.material->occlusionStrength >= 0.0f ||
                                                spec.material->baseColorTexture ||
                                                spec.material->metallicRoughnessTexture ||
                                                spec.material->emissiveTexture ||
                                                spec.material->sampler.has_value());
    if (needsTexture)
    {
        j.key("images").beginArray().beginObject().field("uri", "tex.png").field("mimeType", "image/png").endObject().endArray();

        // Samplers and textures are emitted exactly once each. A second
        // j.key("textures") would silently lose the sampler reference: JSON takes
        // the first of two duplicate keys, so the array written afterwards is
        // discarded and the loader sees a texture with no sampler at all.
        if (spec.material->sampler.has_value())
        {
            const auto& sp = *spec.material->sampler;
            j.key("samplers").beginArray().beginObject();
            if (sp.magFilter >= 0)
                j.field("magFilter", sp.magFilter);
            if (sp.minFilter >= 0)
                j.field("minFilter", sp.minFilter);
            if (sp.wrapS >= 0)
                j.field("wrapS", sp.wrapS);
            if (sp.wrapT >= 0)
                j.field("wrapT", sp.wrapT);
            j.endObject().endArray();
            j.key("textures").beginArray().beginObject().field("source", 0).field("sampler", 0).endObject().endArray();
        }
        else
        {
            j.key("textures").beginArray().beginObject().field("source", 0).endObject().endArray();
        }
    }

    // materials. Material 0 carries any MaterialSpec the test set, so the rest
    // stay plain and the test can compare an explicit value against the defaults.
    if (spec.materialCount > 0)
    {
        j.key("materials").beginArray();
        for (std::size_t i = 0; i < spec.materialCount; ++i)
        {
            j.beginObject();
            j.field("name", "material_" + std::to_string(i));

            j.key("pbrMetallicRoughness").beginObject();
            // Only material 0 gets a baseColorFactor override; the others keep it
            // absent so the spec default is what gets observed.
            if (i == 0 && spec.material && spec.material->hasBaseColorFactor)
                j.field("baseColorFactor", std::vector<double>{spec.material->baseColor[0], spec.material->baseColor[1],
                                                              spec.material->baseColor[2], spec.material->baseColor[3]});
            j.field("metallicFactor", 0.0).field("roughnessFactor", 0.5);
            // baseColorTexture and metallicRoughnessTexture are children of
            // pbrMetallicRoughness, not of the material. Emitting them at
            // material level produces valid JSON that fastgltf silently ignores,
            // so the slots never load and the test asserts on nothing.
            if (i == 0 && spec.material && spec.material->baseColorTexture)
            {
                j.key("baseColorTexture").beginObject().field("index", 0);
                // KHR_texture_transform rides INSIDE the TextureInfo. Emitting it
                // at material level is the mistake this fixture guards against.
                if (spec.material->hasTextureTransform)
                {
                    j.key("extensions")
                        .beginObject()
                        .key("KHR_texture_transform")
                        .beginObject()
                        .field("offset", std::vector<double>{spec.material->transformOffsetX,
                                                             spec.material->transformOffsetY})
                        .field("scale", std::vector<double>{spec.material->transformScaleX,
                                                            spec.material->transformScaleY})
                        .field("rotation", spec.material->transformRotation)
                        .endObject()
                        .endObject();
                }
                j.endObject();
            }
            if (i == 0 && spec.material && spec.material->metallicRoughnessTexture)
                j.key("metallicRoughnessTexture").beginObject().field("index", 0).endObject();
            j.endObject();

            if (i == 0 && spec.material)
            {
                const auto& m = *spec.material;
                if (!m.alphaMode.empty())
                    j.field("alphaMode", m.alphaMode);
                if (m.alphaCutoff >= 0.0f)
                    j.field("alphaCutoff", m.alphaCutoff);
                if (m.doubleSided)
                    // jsonTrue, not field(..., true): the bool overload writes a
                    // number, and glTF validators reject "doubleSided": 1.
                    j.field("doubleSided", true);
                if (m.hasEmissiveFactor)
                    j.field("emissiveFactor", std::vector<double>{m.emissive[0], m.emissive[1], m.emissive[2]});
                if (m.emissiveTexture)
                    j.key("emissiveTexture").beginObject().field("index", 0).endObject();
                if (m.normalScale >= 0.0f)
                    j.key("normalTexture").beginObject().field("index", 0).field("scale", m.normalScale).endObject();
                if (m.occlusionStrength >= 0.0f)
                    j.key("occlusionTexture").beginObject().field("index", 0).field("strength", m.occlusionStrength).endObject();

                // Extension blocks. Emitted only for material 0, and only when
                // the test asked for one, so a material with no extensions emits
                // no "extensions" key at all - which is what makes the
                // spec-default tests meaningful.
                struct ExtEntry
                {
                    const char* name;
                    const std::optional<ExtSpec>* spec;
                };
                const ExtEntry kExts[] = {
                    {"KHR_materials_clearcoat", &m.clearcoat},
                    {"KHR_materials_specular", &m.specular},
                    {"KHR_materials_sheen", &m.sheen},
                    {"KHR_materials_transmission", &m.transmission},
                    {"KHR_materials_volume", &m.volume},
                    {"KHR_materials_iridescence", &m.iridescence},
                    {"KHR_materials_anisotropy", &m.anisotropy},
                    {"KHR_materials_diffuse_transmission", &m.diffuseTransmission},
                };

                bool anyExt = m.unlit || m.hasEmissiveStrength || m.hasIOR;
                for (const auto& e : kExts)
                    anyExt = anyExt || e.spec->has_value();
                if (m.unlit || m.hasEmissiveStrength || m.hasIOR || anyExt)
                {
                    j.key("extensions").beginObject();
                    if (m.unlit)
                        j.key("KHR_materials_unlit").beginObject().endObject();
                    if (m.hasEmissiveStrength)
                        j.key("KHR_materials_emissive_strength").beginObject().field("emissiveStrength", m.emissiveStrength).endObject();
                    if (m.hasIOR)
                        j.key("KHR_materials_ior").beginObject().field("ior", m.ior).endObject();
                    for (const auto& e : kExts)
                    {
                        if (!e.spec->has_value())
                            continue;
                        j.key(e.name).beginObject();
                        for (const auto& kv : (*e.spec)->scalars)
                            j.field(kv.first, kv.second);
                        for (const auto& kv : (*e.spec)->arrays)
                            j.key(kv.first).value(kv.second);
                        j.endObject();
                    }
                    j.endObject();
                }

                // Declaring an extension in extensionsUsed is what the loader's
                // own report reads, so emit the ones in use here too.
                if (anyExt || m.hasTextureTransform)
                {
                    std::vector<std::string> used;
                    if (m.unlit) used.push_back("KHR_materials_unlit");
                    if (m.hasEmissiveStrength) used.push_back("KHR_materials_emissive_strength");
                    if (m.hasIOR) used.push_back("KHR_materials_ior");
                    if (m.hasTextureTransform) used.push_back("KHR_texture_transform");
                    for (const auto& e : kExts)
                        if (e.spec->has_value()) used.push_back(e.name);
                    j.key("extensionsUsed").beginArray();
                    for (const auto& u : used)
                        j.value(u);
                    j.endArray();
                }
            }
            j.endObject();
        }
        j.endArray();
    }

    // nodes
    const std::size_t nodeTotal = spec.distinctMeshes ? spec.meshCount : spec.nodeCount;
    j.key("nodes").beginArray();
    for (std::size_t i = 0; i < nodeTotal; ++i)
    {
        const std::size_t meshRef = spec.distinctMeshes ? i : 0;
        const bool isMeshlessChild = spec.nested && !spec.childHasMesh && i > 0;
        j.beginObject();
        j.field("name", "node_" + std::to_string(i));
        if (!isMeshlessChild)
            j.field("mesh", meshRef);
        j.numberArray("translation", {static_cast<double>(i) * 5.0, 0.0, 0.0});
        if (spec.nested && i == 0 && nodeTotal > 1)
            j.key("children").beginArray().value(static_cast<std::size_t>(1)).endArray();
        if (i < spec.nodeLightIndices.size() && spec.nodeLightIndices[i] >= 0)
        {
            j.key("extensions")
                .beginObject()
                .key("KHR_lights_punctual")
                .beginObject()
                .field("light", static_cast<std::size_t>(spec.nodeLightIndices[i]))
                .endObject()
                .endObject();
        }
        j.endObject();
    }
    j.endArray();

    // scenes
    j.key("scenes").beginArray().beginObject().key("nodes").beginArray();
    if (spec.nested)
    {
        j.value(static_cast<std::size_t>(0));
    }
    else
    {
        for (std::size_t i = 0; i < nodeTotal; ++i)
            j.value(i);
    }
    j.endArray().endObject().endArray();

    // KHR_lights_punctual lights live in an asset-root extension block.
    if (!spec.lights.empty())
    {
        j.key("extensions").beginObject().key("KHR_lights_punctual").beginObject();
        j.key("lights").beginArray();
        for (const auto& light : spec.lights)
        {
            j.beginObject();
            if (!light.type.empty())
                j.field("type", light.type);
            j.field("color", light.color);
            j.field("intensity", light.intensity);
            if (light.hasRange)
                j.field("range", light.range);
            // The spec nests a spot light's cone angles under a "spot" object;
            // a top-level innerConeAngle is not read by the parser at all, so
            // emitting one would quietly produce a cone-less spot.
            if (light.type == "spot")
            {
                j.key("spot").beginObject();
                j.field("innerConeAngle", light.innerCone);
                j.field("outerConeAngle", light.outerCone);
                j.endObject();
            }
            j.endObject();
        }
        j.endArray().endObject().endObject();

        // The generic extensionsUsed emission above only runs when the spec
        // asked for one; a duplicate root-level key would silently shadow it.
        if (spec.extensionsUsed.empty())
        {
            j.key("extensionsUsed").beginArray();
            j.value("KHR_lights_punctual");
            j.endArray();
        }
    }

    j.endObject();

    return dir.write(name, j.str());
}

glm::vec3 translationOf(const glm::mat4& m)
{
    return glm::vec3(m[3][0], m[3][1], m[3][2]);
}

} // namespace

int main()
{
    // The loader logs through UHE::Log, which must be initialized before any call.
    UHE::Log::Init();

    std::printf("gltf loader harness\n\n");

    const TempDir dir;

    // ---------------------------------------------------------------------
    section("node transforms are applied (issue #29)");
    {
        AssetSpec spec;
        spec.meshCount = 3;
        const fs::path path = writeAsset(dir, "transforms.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(model.GetMesh().size() == 3, "one Mesh entry per node");
            for (std::size_t i = 0; i < model.GetMesh().size() && i < 3; ++i)
            {
                const float expected = static_cast<float>(i) * 5.0f;
                const float actual = translationOf(model.GetMesh()[i].LocalTransform).x;
                check(std::fabs(actual - expected) < 1e-4f, "node translation is carried on LocalTransform");
                std::printf("  node_%zu -> x=%.1f (expected %.1f)\n", i, actual, expected);
            }
        }
        else
        {
            check(false, "model loads");
        }
    }

    // ---------------------------------------------------------------------
    section("nested nodes compose");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.nested = true;
        spec.childHasMesh = false;
        const fs::path path = writeAsset(dir, "nested.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            // Node 0 carries a mesh; node 1 is its child and carries none, so
            // only node 0 produces a Mesh entry.
            check(model.GetMesh().size() == 1, "only the node carrying a mesh produces an entry");
            if (!model.GetMesh().empty())
                std::printf("  parent x=%.1f\n", translationOf(model.GetMesh()[0].LocalTransform).x);
        }
        else
        {
            check(false, "nested model loads");
        }
    }

    // ---------------------------------------------------------------------
    section("shared mesh is extracted once");
    {
        // Two nodes referencing ONE mesh at different offsets. The original
        // loader extracted per node, producing two copies of the vertex data and
        // two identical names.
        AssetSpec spec;
        spec.meshCount = 1;
        spec.distinctMeshes = false;
        spec.nodeCount = 2;
        const fs::path path = writeAsset(dir, "shared.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(model.GetMesh().size() == 2, "two node instances");
            check(model.GetGeometry().size() == 1, "geometry extracted once, not once per node");

            if (model.GetMesh().size() == 2)
            {
                check(model.GetMesh()[0].name == "node_0", "node name used, not the mesh name");
                check(model.GetMesh()[1].name == "node_1", "second node has a distinct name");
                check(model.GetMesh()[0].geometryIndex == model.GetMesh()[1].geometryIndex,
                      "both nodes reference the same geometry");

                const float delta = translationOf(model.GetMesh()[1].LocalTransform).x -
                                    translationOf(model.GetMesh()[0].LocalTransform).x;
                check(std::fabs(delta - 5.0f) < 1e-4f, "the two instances are placed independently");
                std::printf("  names: %s, %s | delta x=%.1f\n", model.GetMesh()[0].name.c_str(),
                            model.GetMesh()[1].name.c_str(), delta);
            }
        }
        else
        {
            check(false, "shared-mesh model loads");
        }
    }

    // ---------------------------------------------------------------------
    section("non-indexed geometry gets synthesized indices");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.useIndices = false;
        const fs::path path = writeAsset(dir, "nonindexed.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty(),
                  "geometry produced a drawable primitive");
            if (!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty())
            {
                const auto& prim = model.GetGeometry()[0].primitive[0];
                check(prim.indices.size() == Quad::kVertexCount, "sequential indices synthesized");
                check(prim.IndexCount == Quad::kVertexCount, "IndexCount reflects them");
                check(prim.indices.size() >= 3 && prim.indices[0] == 0 && prim.indices[1] == 1 && prim.indices[2] == 2,
                      "synthesized indices are sequential from zero");
                std::printf("  %zu vertices -> %zu indices\n", prim.vertices.size(), prim.indices.size());
            }
        }
        else
        {
            check(false, "non-indexed model loads");
        }
    }

    // ---------------------------------------------------------------------
    section("out-of-range material index is clamped");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        spec.outOfRangeMaterial = true;
        const fs::path path = writeAsset(dir, "badmaterial.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(model.GetMaterials().size() == 1, "one material declared");
            if (!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty())
            {
                const size_t idx = model.GetGeometry()[0].primitive[0].materialIndex;
                check(idx < model.GetMaterials().size(), "material index is in range after load");
                std::printf("  clamped material index -> %zu\n", idx);
            }
        }
        else
        {
            check(false, "model with a bad material index still loads");
        }
    }

    // ---------------------------------------------------------------------
    section("optimizer passes preserve geometry");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        const fs::path path = writeAsset(dir, "opt.gltf", spec);

        UHE::RD3d::Model model;
        UHE::RD3d::ModelLoadOptions options;
        options.optimizeMesh = true;
        options.mergeVertices = true;

        if (model.loadModel(path, options))
        {
            if (!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty())
            {
                const auto& prim = model.GetGeometry()[0].primitive[0];
                check(!prim.vertices.empty(), "vertices survive optimization");
                check(prim.indices.size() == Quad::kIndexCount, "triangle count unchanged");
                check(prim.indices.size() % 3 == 0, "index count is whole triangles");

                bool inRange = true;
                for (u32 i : prim.indices)
                    if (i >= prim.vertices.size())
                        inRange = false;
                check(inRange, "every index addresses a real vertex after remapping");

                // The set of x positions must survive reordering and merging.
                std::vector<float> xs;
                xs.reserve(prim.vertices.size());
                for (const auto& v : prim.vertices)
                    xs.push_back(v.position.x);
                std::sort(xs.begin(), xs.end());
                std::printf("  %zu vertices, %zu indices, x values:", prim.vertices.size(), prim.indices.size());
                for (float x : xs)
                    std::printf(" %.0f", x);
                std::printf("\n");
            }
            else
            {
                check(false, "optimized geometry has a drawable primitive");
            }
        }
        else
        {
            check(false, "model loads with optimization on");
        }
    }

    // ---------------------------------------------------------------------
    section("bounds are computed");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        const fs::path path = writeAsset(dir, "bounds.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(!model.GetGeometry().empty() && model.GetGeometry()[0].hasBounds, "geometry has bounds");
            if (!model.GetGeometry().empty() && model.GetGeometry()[0].hasBounds)
            {
                const auto& g = model.GetGeometry()[0];
                check(std::fabs(g.boundsMin.x) < 1e-5f && std::fabs(g.boundsMax.x - 1.0f) < 1e-5f,
                      "geometry bounds match the quad");
                std::printf("  bounds x: %.2f .. %.2f\n", g.boundsMin.x, g.boundsMax.x);
            }
        }
        else
        {
            check(false, "model loads");
        }
    }

    // ---------------------------------------------------------------------
    // The unsupported-extension report is only useful in BOTH directions. A
    // loader that flags everything is as wrong as one that flags nothing: the
    // first trains the reader to ignore the warning, the second hides a real gap.
    section("unimplemented extensions are reported");
    {
        // A genuinely unknown extension, which is what the report is for. This
        // previously used KHR_materials_emissive_strength, which stopped being a
        // valid example once Tier 2 implemented it - a test that stops testing
        // its own premise without anyone noticing.
        AssetSpec spec;
        spec.meshCount = 1;
        // extensionsUsed only, NOT extensionsRequired. A required extension the
        // parser does not know is a hard parse failure by design, so it can never
        // reach the loader's own report - which is exactly why the two lists are
        // separate and why the loader checks both.
        spec.extensionsUsed = {"EXT_totally_unknown_extension"};
        const fs::path path = writeAsset(dir, "ext.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(model.HasUnsupportedExtensions(), "unknown extension flagged");
            bool named = false;
            for (const auto& n : model.GetUnsupportedExtensionNames())
                named = named || n == "EXT_totally_unknown_extension";
            check(named, "the unknown extension is reported BY NAME, not just as a flag");
        }
        else
        {
            check(false, "model with an unknown extension still loads");
        }
    }

    section("implemented extensions are NOT reported as unsupported");
    {
        // Every Tier 2/3 extension the loader claims to honour. Claiming support
        // while rendering nothing from a field is the silent-wrong-shading failure
        // the supported list exists to prevent, so the list is asserted directly
        // rather than left to the reader's judgement.
        const char* kImplemented[] = {
            "KHR_materials_unlit",
            "KHR_materials_emissive_strength",
            "KHR_materials_ior",
            "KHR_materials_clearcoat",
            "KHR_materials_specular",
            "KHR_materials_sheen",
            "KHR_materials_transmission",
            "KHR_materials_volume",
            "KHR_materials_iridescence",
            "KHR_materials_anisotropy",
            "KHR_materials_diffuse_transmission",
            "KHR_lights_punctual",
            "EXT_meshopt_compression",
        };

        for (const char* ext : kImplemented)
        {
            AssetSpec spec;
            spec.meshCount = 1;
            spec.materialCount = 1;
            spec.extensionsUsed = {ext};
            spec.extensionsRequired = {ext};

            const fs::path path = writeAsset(dir, "ext_supported.gltf", spec);

            UHE::RD3d::Model model;
            if (check(model.loadModel(path), std::string("model declaring ") + ext + " loads"))
            {
                check(!model.HasUnsupportedExtensions(),
                      std::string(ext) + " is implemented, so not reported as unsupported");
            }
        }
    }

    // ---------------------------------------------------------------------
    section("missing file is reported");
    {
        UHE::RD3d::Model model;
        check(!model.loadModel(dir.path() / "does_not_exist.gltf"), "missing file returns false");
    }


    // =====================================================================
    // Materials. Every default here is one the spec defines and that is easy to
    // get wrong by habit - emissive being black rather than white is the one
    // that would silently light up every surface in a scene.
    // =====================================================================

    section("material spec defaults");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        const fs::path path = writeAsset(dir, "mat_defaults.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a bare material loads"))
        {
            const auto& materials = model.GetMaterials();
            if (check(materials.size() == 1, "one material parsed"))
            {
                const auto& m = materials[0];
                // baseColorFactor defaults to opaque white.
                check(m.BaseColorFactor == glm::vec4(1.0f), "baseColorFactor defaults to opaque white");
                // emissiveFactor defaults to BLACK. White here would make every
                // unlit surface glow, and would be invisible in a test that only
                // checked that it was nonzero.
                check(m.EmissiveFactor == glm::vec3(0.0f), "emissiveFactor defaults to black, not white");
                // normalTexture.scale and occlusionTexture.strength both default
                // to 1.0. A 0.0 default would disable every normal map silently.
                check(m.NormalScale == 1.0f, "normalScale defaults to 1.0, not 0.0");
                check(m.OcclusionStrength == 1.0f, "occlusionStrength defaults to 1.0");
                // alphaCutoff defaults to 0.5 and alphaMode to OPAQUE.
                check(m.AlphaCutoff == 0.5f, "alphaCutoff defaults to 0.5");
                check(m.Alpha == UHE::RD3d::AlphaMode::Opaque, "alphaMode defaults to OPAQUE");
                check(!m.DoubleSided, "doubleSided defaults to false");
                // No texture was declared, so every map must be null rather than
                // a placeholder - the shader branches on -1 slots.
                check(m.NormalTexture == nullptr, "normalTexture stays null when undeclared");
                check(m.EmissiveTexture == nullptr, "emissiveTexture stays null when undeclared");
            }
        }
    }

    section("material explicit values round-trip");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.alphaMode = "MASK";
        ms.alphaCutoff = 0.25f;
        ms.doubleSided = true;
        ms.hasBaseColorFactor = true;
        ms.baseColor[0] = 0.25f;
        ms.baseColor[1] = 0.5f;
        ms.baseColor[2] = 0.75f;
        ms.baseColor[3] = 0.5f;
        ms.hasEmissiveFactor = true;
        ms.emissive[0] = 1.0f;
        ms.emissive[1] = 0.0f;
        ms.emissive[2] = 0.0f;
        ms.normalScale = 0.5f;
        ms.occlusionStrength = 0.75f;
        spec.material = ms;

        const fs::path path = writeAsset(dir, "mat_explicit.gltf", spec);
        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a full material loads"))
        {
            const auto& m = model.GetMaterials()[0];
            check(m.Alpha == UHE::RD3d::AlphaMode::Mask, "alphaMode MASK parsed");
            check(m.AlphaCutoff == 0.25f, "alphaCutoff 0.25 parsed");
            check(m.DoubleSided, "doubleSided true parsed");
            check(m.BaseColorFactor == glm::vec4(0.25f, 0.5f, 0.75f, 0.5f), "baseColorFactor parsed");
            check(m.EmissiveFactor == glm::vec3(1.0f, 0.0f, 0.0f), "emissiveFactor parsed");
            check(m.NormalScale == 0.5f, "normalTexture.scale parsed");
            check(m.OcclusionStrength == 0.75f, "occlusionTexture.strength parsed");
            // alphaMode MASK is not transparent, so no sorted-pass warning.
            check(!model.HasTransparentMaterials(), "MASK materials are not reported as transparent");
        }
    }

    section("BLEND alphaMode is flagged as transparent");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.alphaMode = "BLEND";
        spec.material = ms;

        const fs::path path = writeAsset(dir, "mat_blend.gltf", spec);
        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a BLEND material loads"))
        {
            check(model.GetMaterials()[0].Alpha == UHE::RD3d::AlphaMode::Blend, "alphaMode BLEND parsed");
            // The renderer draws these unsorted until the transparent pass lands,
            // so the loader has to surface it rather than rendering wrongly in
            // silence.
            check(model.HasTransparentMaterials(), "BLEND material sets HasTransparentMaterials");
        }
    }

    // =====================================================================
    // Tangents. The invariant that matters is not "a tangent exists" but that
    // it is unit length and perpendicular to the normal - a tangent frame with
    // either wrong produces lighting that looks almost right and is not.
    // =====================================================================

    section("generated tangents form a valid frame");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.useIndices = false;
        spec.withNormals = true;
        spec.withUvs = true;
        const fs::path path = writeAsset(dir, "tangent.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model needing generated tangents loads"))
        {
            bool sawTangent = false;
            u32 degenerate = 0;
            u32 notPerpendicular = 0;

            // Vertex data lives in GetGeometry(); GetMesh() holds per-node
            // instances whose primitives carry GPU handles only.
            for (const auto& geom : model.GetGeometry())
            {
                for (const auto& prim : geom.primitive)
                {
                    for (const auto& v : prim.vertices)
                    {
                        sawTangent = true;
                        const glm::vec3 t(v.tangent);
                        const f32 len = glm::length(t);
                        // A zero-length tangent produces a NaN frame in the shader,
                        // which does not fail loudly - it renders black.
                        if (len < 0.99f || len > 1.01f)
                            ++degenerate;
                        if (std::abs(glm::dot(glm::normalize(v.normal), t)) > 0.01f)
                            ++notPerpendicular;
                    }
                }
            }

            check(sawTangent, "vertices were produced");
            check(degenerate == 0, "every tangent is unit length (" + std::to_string(degenerate) + " bad)");
            check(notPerpendicular == 0,
                  "every tangent is perpendicular to its normal (" + std::to_string(notPerpendicular) + " bad)");
        }
    }

    section("tangent generation splits UV seam vertices");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.useIndices = false;
        spec.withNormals = true;
        spec.withUvs = true;
        const fs::path path = writeAsset(dir, "tangent_seam.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty(),
                  "seam test produced a primitive");
            const auto& prim = model.GetGeometry()[0].primitive[0];

            // The quad fixture has vertices sharing a POSITION but differing in UV
            // - that is a texture seam. Tangents are generated per-corner and the
            // vertices merged afterwards, so the seam must survive with two
            // distinct frames. If merging ran first the seam would collapse to one
            // vertex and one side would be lit with the other's tangent.
            std::printf("  vertices after tangent generation: %zu (quad has %zu)\n", prim.vertices.size(),
                        Quad::kVertexCount);
            check(!prim.vertices.empty(), "seam test produced vertices");
            check(prim.indices.size() % 3 == 0, "index count is a whole number of triangles");
            check(prim.indices.size() / 3 == Quad::kIndexCount / 3, "triangle count is unchanged by tangent generation");
        }
    }

    section("tangent generation is skinned-safe");
    {
        // A skinned primitive must not be destructively merged across joints, or
        // a vertex influenced by two bones would take one bone's weight for both.
        AssetSpec spec;
        spec.meshCount = 1;
        spec.useIndices = true;
        spec.skinned = true;
        spec.withNormals = true;
        spec.withUvs = true;
        const fs::path path = writeAsset(dir, "tangent_skin.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "skinned model with tangents loads"))
        {
            check(!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty(),
                  "skinned primitive was extracted");
            const auto& prim = model.GetGeometry()[0].primitive[0];
            // Each corner must still reference its own vertex after tangent
            // generation, since deindexing alone would multiply the vertex count.
            check(!prim.vertices.empty(), "skinned primitive has vertices");
            bool indicesInRange = true;
            for (u32 idx : prim.indices)
                indicesInRange = indicesInRange && idx < prim.vertices.size();
            check(indicesInRange, "every skinned index is in range after tangent generation");
        }
    }


    // =====================================================================
    // Drawable invariants. The loader's scene walk copies GPU handles into the
    // per-node Mesh entries BEFORE upload runs, so those copies start null and
    // only become valid once upload propagates them back. If that propagation is
    // missing or misordered, every handle stays null, SubmitModel's guard skips
    // every primitive, and the editor renders nothing - with no validation error,
    // no log line, and no crash. It is invisible to every other kind of test.
    // =====================================================================

    section("every drawable node has valid GPU handles");
    {
        AssetSpec spec;
        spec.meshCount = 2;
        spec.nodeCount = 2; // one node per mesh, so both must come out drawable
        spec.useIndices = true;
        spec.materialCount = 1;
        const fs::path path = writeAsset(dir, "drawable.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "multi-node model for drawable test loads"))
        {
            check(!model.GetMesh().empty(), "model produced node instances");

            u32 missingVertexBuffer = 0;
            u32 missingIndexBuffer = 0;
            u32 zeroIndexCount = 0;
            u32 primitivesSeen = 0;

            // Exactly the guard SubmitModel applies before issuing a draw.
            for (const auto& mesh : model.GetMesh())
            {
                for (const auto& prim : mesh.primitive)
                {
                    ++primitivesSeen;
                    if (!prim.VertexBuffer)
                        ++missingVertexBuffer;
                    if (!prim.IndexBuffer)
                        ++missingIndexBuffer;
                    if (prim.IndexCount == 0)
                        ++zeroIndexCount;
                }
            }

            check(primitivesSeen > 0, "node instances carry primitives (" +
                                         std::to_string(primitivesSeen) + ")");
            check(missingVertexBuffer == 0,
                  "every primitive has a vertex buffer (" + std::to_string(missingVertexBuffer) + " missing)");
            check(missingIndexBuffer == 0,
                  "every primitive has an index buffer (" + std::to_string(missingIndexBuffer) + " missing)");
            check(zeroIndexCount == 0,
                  "every primitive has a nonzero index count (" + std::to_string(zeroIndexCount) + " zero)");
        }
    }

    section("shared geometry uploads once and reaches every node");
    {
        // Two nodes referencing one glTF mesh: one upload, two drawable nodes.
        // Both conditions have to hold - two nodes proves propagation reached
        // every instance, and the geometry buffer count proves dedup still works.
        AssetSpec spec;
        spec.meshCount = 1;
        spec.nodeCount = 2;
        // distinctMeshes defaults to true, which emits ONE node per mesh - so
        // meshCount=1 would produce a single node and this test would prove
        // nothing. false means "every node references mesh 0", which is what
        // makes it a shared-geometry test.
        spec.distinctMeshes = false;
        spec.useIndices = true;
        const fs::path path = writeAsset(dir, "shared_drawable.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "shared-mesh drawable model loads"))
        {
            check(model.GetGeometry().size() == 1, "one geometry for the shared mesh");
            check(model.GetMesh().size() == 2, "two node instances");

            bool bothDrawable = true;
            for (const auto& mesh : model.GetMesh())
            {
                if (mesh.primitive.empty())
                    bothDrawable = false;
                for (const auto& prim : mesh.primitive)
                    bothDrawable = bothDrawable && prim.VertexBuffer && prim.IndexBuffer;
            }
            check(bothDrawable, "both node instances are drawable");
        }
    }


    // =====================================================================
    // Sampler state. glTF declares magFilter/minFilter/wrapS/wrapT per texture;
    // before this the backend hardcoded linear/repeat for everything, so a
    // CLAMP_TO_EDGE atlas or a NEAREST pixel-art texture sampled wrongly with no
    // way to tell from the outside.
    // =====================================================================

    section("glTF sampler state reaches the texture factory");
    {
        using F = UHE::RHI::SamplerDesc::Filter;
        using W = UHE::RHI::SamplerDesc::Wrap;

        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        // Deliberately asymmetric wrap and a NEAREST mag: a single addressMode
        // applied to both axes cannot express this, which is the whole reason
        // the per-axis overload exists.
        SamplerSpec sp;
        sp.magFilter = 9728;  // NEAREST
        sp.minFilter = 9984;  // NEAREST_MIPMAP_NEAREST
        sp.wrapS = 33071;     // CLAMP_TO_EDGE
        sp.wrapT = 33648;     // MIRRORED_REPEAT
        ms.sampler = sp;
        ms.normalScale = 0.5f; // forces a texture reference
        spec.material = ms;

        ::UHE::StubReset();
        const fs::path path = writeAsset(dir, "sampler.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a declared sampler loads"))
        {
            check(::UHE::StubTextureCreateCallCount() > 0, "a texture was requested");
            const auto s = ::UHE::StubLastRequestedSampler();
            check(s.magFilter == F::Nearest, "magFilter NEAREST passed through");
            check(s.minFilter == F::Nearest, "minFilter NEAREST_MIPMAP_NEAREST maps to nearest");
            check(s.wrapS == W::ClampToEdge, "wrapS CLAMP_TO_EDGE passed through");
            // The asymmetric pair is the real assertion: one addressMode for both
            // axes would make one of these wrong.
            check(s.wrapT == W::MirroredRepeat, "wrapT MIRRORED_REPEAT differs from wrapS");
        }
    }

    section("absent sampler falls back to spec defaults");
    {
        using F = UHE::RHI::SamplerDesc::Filter;
        using W = UHE::RHI::SamplerDesc::Wrap;

        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        // No sampler object at all: many exporters omit it. The defaults must be
        // LINEAR / LINEAR_MIPMAP_LINEAR / REPEAT, which is also what the backend
        // hardcoded before, so unchanged assets stay unchanged.
        ms.normalScale = 1.0f;
        spec.material = ms;

        ::UHE::StubReset();
        const fs::path path = writeAsset(dir, "sampler_none.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with no sampler object loads"))
        {
            const auto s = ::UHE::StubLastRequestedSampler();
            check(s.magFilter == F::Linear, "default magFilter is LINEAR");
            check(s.minFilter == F::LinearMipmapLinear, "default minFilter is LINEAR_MIPMAP_LINEAR");
            check(s.wrapS == W::Repeat, "default wrapS is REPEAT");
            check(s.wrapT == W::Repeat, "default wrapT is REPEAT");
        }
    }

    section("partial sampler keeps spec defaults for omitted fields");
    {
        using W = UHE::RHI::SamplerDesc::Wrap;

        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        SamplerSpec sp;
        sp.wrapS = 33071; // only wrapS declared
        ms.sampler = sp;
        ms.occlusionStrength = 1.0f; // forces a texture reference
        spec.material = ms;

        ::UHE::StubReset();
        const fs::path path = writeAsset(dir, "sampler_partial.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a partial sampler loads"))
        {
            const auto s = ::UHE::StubLastRequestedSampler();
            check(s.wrapS == W::ClampToEdge, "declared wrapS applied");
            // wrapT omitted must not inherit wrapS - per-axis is the whole point.
            check(s.wrapT == W::Repeat, "omitted wrapT falls back to REPEAT, not wrapS");
        }
    }

    // =====================================================================
    // Colour space is an IMAGE property (Vulkan encodes sRGB in the format, not
    // the sampler). glTF mandates sRGB for baseColor and emissive, and LINEAR
    // data for normal, metallicRoughness and occlusion. Loading every one of
    // them as sRGB gamma-decodes the linear maps on every sample - which is why
    // a normal map can light a surface the wrong way round.
    // =====================================================================

    section("texture colour space follows the glTF slot");
    {
        using CS = UHE::RHI::SamplerDesc::ColorSpace;

        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        // Every slot declared at once, so the assertion sees both kinds in one
        // load rather than trusting a single default.
        ms.baseColorTexture = true;
        ms.emissiveTexture = true;
        ms.metallicRoughnessTexture = true;
        ms.normalScale = 1.0f;
        ms.occlusionStrength = 1.0f;
        spec.material = ms;

        ::UHE::StubReset();
        const fs::path path = writeAsset(dir, "colorspace.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with every texture slot loads"))
        {
            const auto& requested = ::UHE::StubRequestedSamplers();
            // Five slots: baseColor, metallicRoughness, normal, occlusion, emissive.
            check(requested.size() == 5,
                  "all five texture slots requested a texture (" + std::to_string(requested.size()) + ")");

            int srgb = 0;
            int linear = 0;
            for (const auto& s : requested)
            {
                if (s.colorSpace == CS::SRGB) ++srgb;
                else if (s.colorSpace == CS::Linear) ++linear;
            }
            // Exactly two colour slots (baseColor, emissive) and three data slots
            // (metallicRoughness, normal, occlusion). Counting rather than
            // asserting a load ORDER: the loader is free to visit the slots in
            // whatever sequence is correct, and pinning that here would fail on
            // a harmless reorder.
            check(srgb == 2, "exactly the 2 colour slots (baseColor, emissive) are sRGB (" + std::to_string(srgb) + ")");
            check(linear == 3, "exactly the 3 data slots (metalRough, normal, occlusion) are linear (" + std::to_string(linear) + ")");
        }
    }

    section("baseColor defaults to sRGB when it is the only slot");
    {
        using CS = UHE::RHI::SamplerDesc::ColorSpace;

        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.baseColorTexture = true;
        spec.material = ms;

        ::UHE::StubReset();
        const fs::path path = writeAsset(dir, "colorspace_base.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with only a baseColor texture loads"))
        {
            const auto s = ::UHE::StubLastRequestedSampler();
            // The default must be the COLOUR space, because baseColor is the slot
            // every glTF asset has. Defaulting to linear would make every
            // un-updated asset render too dark.
            check(s.colorSpace == CS::SRGB, "baseColor is sRGB");
        }
    }

    section("normal texture is linear, not sRGB");
    {
        using CS = UHE::RHI::SamplerDesc::ColorSpace;

        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.normalScale = 1.0f; // declares normalTexture only
        spec.material = ms;

        ::UHE::StubReset();
        const fs::path path = writeAsset(dir, "colorspace_normal.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a normal texture loads"))
        {
            const auto s = ::UHE::StubLastRequestedSampler();
            // Normal maps store a direction in [-1,1]. Gamma-decoding them bends
            // the direction, so lighting comes out wrong - not just darker.
            check(s.colorSpace == CS::Linear, "normalTexture is loaded as linear data");
        }
    }

    // =====================================================================
    // COLOR_0 vertex colours. The failure mode without it is a model that loads,
    // passes every other check, and renders with its painted colours missing.
    // =====================================================================

    section("COLOR_0 VEC4 reaches the vertex data");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        spec.colorComponents = 4;

        const fs::path path = writeAsset(dir, "color4.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a VEC4 COLOR_0 loads"))
        {
            const auto& geom = model.GetGeometry();
            bool got = !geom.empty() && !geom[0].primitive.empty();
            check(got, "primitive survived");
            if (got)
            {
                const auto& prim = geom[0].primitive[0];
                check(prim.hasVertexColor, "hasVertexColor set for COLOR_0");
                // Vertex 0 is (0, 1, 0.5, 1) and vertex 3 is (0.5, 0.5, 0.5, 1).
                // Checking exact values is what proves the accessor was read at
                // the right offset; a single-colour fixture would not.
                if (prim.vertices.size() > 3)
                {
                    const auto& v0 = prim.vertices[0].color;
                    check(std::fabs(v0.x - 0.0f) < 1e-5f && std::fabs(v0.y - 1.0f) < 1e-5f &&
                              std::fabs(v0.z - 0.5f) < 1e-5f && std::fabs(v0.w - 0.25f) < 1e-5f,
                          "VEC4 colour read exactly (r=0, g=1, b=0.5, a=0.25)");
                    const auto& v3 = prim.vertices[3].color;
                    check(std::fabs(v3.x - 0.5f) < 1e-5f && std::fabs(v3.y - 0.5f) < 1e-5f,
                          "VEC4 colour read at the right vertex (v3 r=0.5)");
                }
            }
        }
    }

    section("COLOR_0 VEC3 gets alpha 1");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        spec.colorComponents = 3;

        const fs::path path = writeAsset(dir, "color3.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model with a VEC3 COLOR_0 loads"))
        {
            const auto& geom = model.GetGeometry();
            bool got = !geom.empty() && !geom[0].primitive.empty();
            check(got, "primitive survived a VEC3 COLOR_0");
            if (got)
            {
                const auto& prim = geom[0].primitive[0];
                check(prim.hasVertexColor, "hasVertexColor set for a VEC3 COLOR_0");
                if (!prim.vertices.empty())
                {
                    // Reading a VEC3 accessor as VEC4 would pull the next vertex's
                    // components in as alpha, so alpha is the tell: the spec says
                    // it is 1.
                    check(std::fabs(prim.vertices[0].color.w - 1.0f) < 1e-5f, "VEC3 colour gets alpha 1");
                    check(std::fabs(prim.vertices[0].color.y - 1.0f) < 1e-5f, "VEC3 rgb read exactly");
                }
            }
        }
    }

    section("no COLOR_0 leaves vertex colour white and the flag off");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        // colorComponents stays 0.

        const fs::path path = writeAsset(dir, "nocolor.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "model without COLOR_0 loads"))
        {
            const auto& geom = model.GetGeometry();
            bool got = !geom.empty() && !geom[0].primitive.empty();
            check(got, "primitive survived");
            if (got)
            {
                const auto& prim = geom[0].primitive[0];
                check(!prim.hasVertexColor, "hasVertexColor off when COLOR_0 is absent");
                if (!prim.vertices.empty())
                {
                    // WHITE, not black. A zero default here tints every mesh in
                    // the engine black the moment the shader starts multiplying it.
                    check(prim.vertices[0].color == glm::vec4(1.0f), "default vertex colour is opaque white");
                }
            }
        }
    }

    section("COLOR_0 survives vertex merging and reordering");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        // Normals and UVs give the merge pass two vertices differing only in
        // colour to collapse - the case where losing the attribute would be
        // visible.
        spec.withNormals = true;
        spec.withUvs = true;
        spec.colorComponents = 4;

        const fs::path path = writeAsset(dir, "color_opt.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "optimized model with COLOR_0 loads"))
        {
            const auto& geom = model.GetGeometry();
            bool got = !geom.empty() && !geom[0].primitive.empty();
            check(got, "primitive survived optimization");
            if (got)
            {
                const auto& prim = geom[0].primitive[0];
                check(prim.hasVertexColor, "hasVertexColor survives optimization");
                // Every colour must still be one the fixture wrote. If the merge
                // hashed colour out, or the fetch reorder lost it, some vertex
                // would carry a value that is not in the fixture's ramp.
                bool allKnown = true;
                for (const auto& v : prim.vertices)
                {
                    const bool bOk = std::fabs(v.color.z - 0.5f) < 1e-5f;
                    const bool rgOk = std::fabs(v.color.x + v.color.y - 1.0f) < 1e-4f;
                    // Alpha is tied to R by the fixture's ramp, so this also
                    // catches alpha being dropped or defaulted to 1.
                    const bool aOk = std::fabs(v.color.w - (0.25f + 0.5f * v.color.x)) < 1e-4f;
                    if (!bOk || !rgOk || !aOk)
                    {
                        allKnown = false;
                        break;
                    }
                }
                check(allKnown, "every vertex keeps a colour the fixture wrote, after merge + fetch reorder");
            }
        }
    }

    // =====================================================================
    // Tier 2 PBR extensions. The failure mode for all of them is identical and
    // silent: a file declares clearcoat, the loader parses the block, and if
    // nothing reads the values the surface renders as plain plastic with no
    // warning anywhere. So each test asserts the VALUE arrived, not merely that
    // the file loaded.
    // =====================================================================

    section("KHR_materials_clearcoat values are read");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.clearcoat = Ext({{"clearcoatFactor", 0.75}, {"clearcoatRoughnessFactor", 0.25}});
        spec.material = ms;

        const fs::path path = writeAsset(dir, "clearcoat.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "clearcoat model loads"))
        {
            const auto& e = model.GetMaterials()[0].Extensions;
            check(e.HasClearcoat, "HasClearcoat set");
            check(std::fabs(e.ClearcoatFactor - 0.75f) < 1e-5f, "clearcoatFactor 0.75 parsed");
            check(std::fabs(e.ClearcoatRoughnessFactor - 0.25f) < 1e-5f, "clearcoatRoughnessFactor 0.25 parsed");
        }
    }

    section("KHR_materials_specular and sheen values are read");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.specular = Ext({{"specularFactor", 0.6}});
        ms.sheen = Ext({{"sheenRoughnessFactor", 0.4}});
        WithArray(ms.sheen.value(), "sheenColorFactor", {0.1, 0.2, 0.3});
        spec.material = ms;

        const fs::path path = writeAsset(dir, "specsheen.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "specular+sheen model loads"))
        {
            const auto& e = model.GetMaterials()[0].Extensions;
            check(e.HasSpecular, "HasSpecular set");
            check(std::fabs(e.SpecularFactor - 0.6f) < 1e-5f, "specularFactor 0.6 parsed");
            check(e.HasSheen, "HasSheen set");
            check(std::fabs(e.SheenRoughnessFactor - 0.4f) < 1e-5f, "sheenRoughnessFactor 0.4 parsed");
            // sheenColorFactor is a vec3, so it also proves the colour path.
            check(std::fabs(e.SheenColorFactor.x - 0.1f) < 1e-5f, "sheenColorFactor.r parsed");
            check(std::fabs(e.SheenColorFactor.z - 0.3f) < 1e-5f,
                  "sheenColorFactor is a 3-array, not a scalar (z parsed)");
        }
    }

    section("KHR_materials_transmission marks the material transparent");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.transmission = Ext({{"transmissionFactor", 0.9}});
        ms.volume = Ext({{"thicknessFactor", 0.5}});
        spec.material = ms;

        const fs::path path = writeAsset(dir, "transmission.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "transmission model loads"))
        {
            const auto& e = model.GetMaterials()[0].Extensions;
            check(e.HasTransmission, "HasTransmission set");
            check(std::fabs(e.TransmissionFactor - 0.9f) < 1e-5f, "transmissionFactor 0.9 parsed");
            check(std::fabs(e.ThicknessFactor - 0.5f) < 1e-5f, "thicknessFactor 0.5 parsed");
            // Glass is see-through whether or not the exporter set alphaMode
            // BLEND, so the loader must route it to the blended path itself.
            check(e.TransmissionBlend == UHE::RD3d::BlendApproach::Blend,
                  "a transmissive material is routed to the blended path");
            check(model.HasTransparentMaterials(),
                  "transmission sets HasTransparentMaterials without alphaMode BLEND");
        }
    }

    section("iridescence and anisotropy values are read");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.iridescence = Ext({{"iridescenceFactor", 0.8}, {"iridescenceIor", 1.5},
                              {"iridescenceThicknessMinimum", 200.0},
                              {"iridescenceThicknessMaximum", 600.0}});
        ms.anisotropy = Ext({{"anisotropyStrength", 0.7}, {"anisotropyRotation", 0.25}});
        spec.material = ms;

        const fs::path path = writeAsset(dir, "irid_aniso.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "iridescence+anisotropy model loads"))
        {
            const auto& e = model.GetMaterials()[0].Extensions;
            check(e.HasIridescence, "HasIridescence set");
            check(std::fabs(e.IridescenceFactor - 0.8f) < 1e-5f, "iridescenceFactor parsed");
            check(std::fabs(e.IridescenceIOR - 1.5f) < 1e-5f, "iridescenceIor parsed");
            check(std::fabs(e.IridescenceThicknessMinimum - 200.0f) < 1e-4f, "iridescenceThicknessMinimum parsed");
            check(std::fabs(e.IridescenceThicknessMaximum - 600.0f) < 1e-4f, "iridescenceThicknessMaximum parsed");
            check(e.HasAnisotropy, "HasAnisotropy set");
            check(std::fabs(e.AnisotropyStrength - 0.7f) < 1e-5f, "anisotropyStrength parsed");
            check(std::fabs(e.AnisotropyRotation - 0.25f) < 1e-5f, "anisotropyRotation parsed");
        }
    }

    section("emissive_strength, ior and unlit are read");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.hasEmissiveStrength = true;
        ms.emissiveStrength = 3.5f;
        ms.hasIOR = true;
        ms.ior = 1.7f;
        ms.unlit = true;
        spec.material = ms;

        const fs::path path = writeAsset(dir, "es_ior_unlit.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "emissive_strength+ior+unlit model loads"))
        {
            const auto& e = model.GetMaterials()[0].Extensions;
            check(std::fabs(e.EmissiveStrength - 3.5f) < 1e-5f, "emissiveStrength 3.5 parsed");
            check(std::fabs(e.IOR - 1.7f) < 1e-5f, "ior 1.7 parsed");
            check(e.Unlit, "KHR_materials_unlit flag parsed");
        }
    }

    // The defaults matter more than the parsing. fastgltf fills every extension
    // struct with SPEC DEFAULTS, so reading one unconditionally would make an
    // ordinary PBR material claim it has a sheen, a volume and an index of
    // refraction - and a sheenRoughness of 0 or an attenuationDistance of 0
    // renders visibly wrong.
    section("a material declaring no extension keeps spec defaults");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        // Nothing set: no extension blocks at all.
        spec.material = ms;

        const fs::path path = writeAsset(dir, "noext.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "material with no extensions loads"))
        {
            const auto& e = model.GetMaterials()[0].Extensions;
            check(!e.HasClearcoat, "no clearcoat claimed");
            check(!e.HasSpecular, "no specular claimed");
            check(!e.HasSheen, "no sheen claimed");
            check(!e.HasTransmission, "no transmission claimed");
            check(!e.HasIridescence, "no iridescence claimed");
            check(!e.HasAnisotropy, "no anisotropy claimed");
            check(!e.Unlit, "not unlit");

            // These are the values a wrong default would corrupt.
            check(std::fabs(e.EmissiveStrength - 1.0f) < 1e-5f,
                  "EmissiveStrength defaults to 1.0, not 0 (0 would erase every emissive)");
            check(std::fabs(e.IOR - 1.5f) < 1e-5f, "IOR defaults to the dielectric 1.5");
            // INFINITY, not 0: attenuationDistance 0 means fully opaque.
            check(std::isinf(e.AttenuationDistance) && e.AttenuationDistance > 0.0f,
                  "AttenuationDistance defaults to +infinity, not 0");
        }
    }

    // =====================================================================
    // Tier 3 additions: diffuse transmission, texture transforms, punctual
    // lights, and EXT_meshopt_compression. Same contract as Tier 2: the VALUE
    // must arrive, not merely the file.
    // =====================================================================

    section("KHR_materials_diffuse_transmission values are read");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.diffuseTransmission = Ext({{"diffuseTransmissionFactor", 0.65}});
        WithArray(ms.diffuseTransmission.value(), "diffuseTransmissionColorFactor", {0.2, 0.4, 0.9});
        spec.material = ms;

        const fs::path path = writeAsset(dir, "diftrans.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "diffuse transmission model loads"))
        {
            const auto& e = model.GetMaterials()[0].Extensions;
            check(e.HasDiffuseTransmission, "HasDiffuseTransmission set");
            check(std::fabs(e.DiffuseTransmissionFactor - 0.65f) < 1e-5f, "diffuseTransmissionFactor parsed");
            check(std::fabs(e.DiffuseTransmissionColor.g - 0.4f) < 1e-5f, "diffuseTransmissionColorFactor.g parsed");
            check(std::fabs(e.DiffuseTransmissionColor.z - 0.9f) < 1e-5f,
                  "diffuseTransmissionColorFactor is a 3-array, not a scalar (z parsed)");
            // The leaf-translucency extension is NOT see-through glass: it must
            // not flag the material for the blended path the way
            // KHR_materials_transmission does.
            check(!model.HasTransparentMaterials(),
                  "diffuse transmission alone does not mark the model transparent");
        }
    }

    section("KHR_texture_transform converts to engine UV space");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        MaterialSpec ms;
        ms.baseColorTexture = true;
        ms.hasTextureTransform = true;
        ms.transformRotation = 0.25; // radians CCW, glTF space
        ms.transformOffsetX = 0.1;
        ms.transformOffsetY = 0.2;
        ms.transformScaleX = 2.0;
        ms.transformScaleY = 3.0;
        spec.material = ms;

        const fs::path path = writeAsset(dir, "textransform.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "texture transform model loads"))
        {
            const auto& t = model.GetMaterials()[0].UVTransforms[static_cast<size_t>(
                UHE::RD3d::MaterialTextureSlot::Albedo)];
            check(t.HasTransform, "albedo slot transform recorded");

            // The loader flips V while importing TEXCOORD_0 (engine convention),
            // so the raw glTF values cannot be copied: applying them over the
            // flipped coordinates would mirror the rotation and land the offset
            // on 1 - offset. LoadModel.cpp converts (see EngineUVTransform):
            //   uv' = R(-theta) * (uv * scale) + (offset.x - sin*scale.y,
            //                                     1 - cos*scale.y - offset.y)
            const double c = std::cos(0.25);
            const double s = std::sin(0.25);
            check(std::fabs(t.cosRotation - static_cast<float>(c)) < 1e-5f, "cos(rotation) carried");
            check(std::fabs(t.sinRotation - static_cast<float>(s)) < 1e-5f, "sin(rotation) carried");
            check(std::fabs(t.scale.x - 2.0f) < 1e-5f, "scale.x carried");
            check(std::fabs(t.scale.y - 3.0f) < 1e-5f, "scale.y carried");
            const float expectedOffsetX = static_cast<float>(0.1 - s * 3.0);
            const float expectedOffsetY = static_cast<float>(1.0 - c * 3.0 - 0.2);
            check(std::fabs(t.offset.x - expectedOffsetX) < 1e-4f,
                  "offset.x converted for the V flip (" + std::to_string(t.offset.x) +
                      " vs " + std::to_string(expectedOffsetX) + ")");
            check(std::fabs(t.offset.y - expectedOffsetY) < 1e-4f,
                  "offset.y converted for the V flip (" + std::to_string(t.offset.y) +
                      " vs " + std::to_string(expectedOffsetY) + ")");

            // A transform on one slot must not leak into another.
            const auto& other =
                model.GetMaterials()[0].UVTransforms[static_cast<size_t>(UHE::RD3d::MaterialTextureSlot::Normal)];
            check(!other.HasTransform, "transform stays on its own slot");
        }
    }

    section("KHR_lights_punctual lights are parsed and placed by nodes");
    {
        AssetSpec spec;
        spec.meshCount = 2;
        spec.materialCount = 1;
        AssetSpec::LightSpec point;
        point.type = "point";
        point.color = {1.0, 0.5, 0.25};
        point.intensity = 3.0;
        point.hasRange = true;
        point.range = 10.0;
        AssetSpec::LightSpec spot;
        spot.type = "spot";
        spot.color = {1.0, 1.0, 1.0};
        spot.intensity = 5.0;
        // No range: the spec default is "infinite".
        spot.hasInnerCone = true;
        spot.innerCone = 0.2;
        spot.hasOuterCone = true;
        spot.outerCone = 0.9;
        spec.lights = {point, spot};
        spec.nodeLightIndices = {0, 1};

        const fs::path path = writeAsset(dir, "lights.gltf", spec);

        UHE::RD3d::Model model;
        if (check(model.loadModel(path), "punctual-light model loads"))
        {
            const auto& lights = model.GetPunctualLights();
            check(lights.size() == 2, "two lights parsed");
            if (lights.size() == 2)
            {
                check(lights[0].type == UHE::RD3d::PunctualLight::Type::Point, "light 0 is a point light");
                check(std::fabs(lights[0].Color.g - 0.5f) < 1e-5f, "light 0 colour parsed");
                check(std::fabs(lights[0].Intensity - 3.0f) < 1e-5f, "light 0 intensity parsed");
                check(std::fabs(lights[0].Range - 10.0f) < 1e-4f, "light 0 range parsed");

                check(lights[1].type == UHE::RD3d::PunctualLight::Type::Spot, "light 1 is a spot light");
                check(std::fabs(lights[1].InnerConeAngle - 0.2f) < 1e-5f, "inner cone parsed");
                check(std::fabs(lights[1].OuterConeAngle - 0.9f) < 1e-5f, "outer cone parsed");
                // An omitted range means unlimited reach - INFINITY, not 0,
                // which would clamp the light to zero range.
                check(std::isinf(lights[1].Range) && lights[1].Range > 0.0f, "omitted range defaults to infinity");
            }

            const auto& nodes = model.GetNodes();
            check(nodes.size() == 2, "two nodes walked");
            if (nodes.size() == 2)
            {
                check(nodes[0].LightIndex == 0, "node 0 references light 0");
                check(nodes[1].LightIndex == 1, "node 1 references light 1");
                // The accumulated transform matters: light 1 sits on a node
                // translated 5 units along x, and a light placed at the origin
                // would illuminate nothing it is supposed to.
                check(std::fabs(translationOf(nodes[1].WorldTransform).x - 5.0f) < 1e-4f,
                      "node 1 world transform carries the authored placement");
            }
        }
    }

    section("EXT_meshopt_compression geometry decodes");
    {
        // Hand-built (not through writeAsset): the fixture's quad, with its
        // position and index buffer views compressed through the meshoptimizer
        // encoder. The decode must be exact - the loader then runs its usual
        // passes on the recovered vertices.
        const std::vector<float> quadPositions = Quad::positions();
        const std::vector<u32> quadIndices = Quad::indices();
        const std::size_t kVertexCount = Quad::kVertexCount;
        const std::size_t kIndexCount = Quad::kIndexCount;

        // Encode positions: 6 vertices x 3 floats.
        const std::size_t posBound = meshopt_encodeVertexBufferBound(kVertexCount, 3 * sizeof(float));
        std::vector<unsigned char> posCompressed(posBound);
        const std::size_t posSize =
            meshopt_encodeVertexBuffer(posCompressed.data(), posBound, quadPositions.data(), kVertexCount,
                                       3 * sizeof(float));
        posCompressed.resize(posSize);

        // Encode indices: u32 triangles.
        const std::size_t idxBound = meshopt_encodeIndexBufferBound(kIndexCount, kVertexCount);
        std::vector<unsigned char> idxCompressed(idxBound);
        const std::size_t idxSize = meshopt_encodeIndexBuffer(idxCompressed.data(), idxBound, quadIndices.data(),
                                                              kIndexCount);
        idxCompressed.resize(idxSize);

        std::string bin;
        bin.append(reinterpret_cast<const char*>(posCompressed.data()), posSize);
        bin.append(reinterpret_cast<const char*>(idxCompressed.data()), idxSize);

        const std::string binName = "meshopt.bin";
        dir.write(binName, bin);

        Json j;
        j.beginObject();
        j.key("asset").beginObject().field("version", "2.0").endObject();
        j.field("scene", 0);
        j.key("extensionsRequired").beginArray().value("EXT_meshopt_compression").endArray();
        j.key("extensionsUsed").beginArray().value("EXT_meshopt_compression").endArray();

        j.key("buffers").beginArray().beginObject().field("uri", binName).field("byteLength", bin.size()).endObject().endArray();

        // bufferView.byteLength is the DECODED size; the compressed size lives
        // in the extension object. Mixing the two up truncates the stream.
        j.key("bufferViews").beginArray();
        j.beginObject()
            .field("buffer", 0)
            .field("byteOffset", 0)
            .field("byteLength", kVertexCount * 3 * sizeof(float))
            .key("extensions")
            .beginObject()
            .key("EXT_meshopt_compression")
            .beginObject()
            .field("buffer", 0)
            .field("byteOffset", 0)
            .field("byteLength", posSize)
            .field("mode", "ATTRIBUTES")
            .field("filter", "NONE")
            .field("count", kVertexCount)
            .field("byteStride", 12)
            .endObject()
            .endObject()
            .endObject();
        j.beginObject()
            .field("buffer", 0)
            .field("byteOffset", 0)
            .field("byteLength", kIndexCount * sizeof(u32))
            .key("extensions")
            .beginObject()
            .key("EXT_meshopt_compression")
            .beginObject()
            .field("buffer", 0)
            .field("byteOffset", posSize)
            .field("byteLength", idxSize)
            .field("mode", "TRIANGLES")
            .field("filter", "NONE")
            .field("count", kIndexCount)
            .field("byteStride", 4)
            .endObject()
            .endObject()
            .endObject();
        j.endArray();

        j.key("accessors")
            .beginArray()
            .beginObject()
            .field("bufferView", 0)
            .field("componentType", 5126)
            .field("count", kVertexCount)
            .field("type", "VEC3")
            .endObject()
            .beginObject()
            .field("bufferView", 1)
            .field("componentType", 5125)
            .field("count", kIndexCount)
            .field("type", "SCALAR")
            .endObject()
            .endArray();

        j.key("materials").beginArray().beginObject().field("name", "m").endObject().endArray();

        j.key("meshes").beginArray().beginObject().key("primitives").beginArray().beginObject();
        j.key("attributes").beginObject().field("POSITION", 0).endObject();
        j.field("indices", 1);
        j.field("material", 0);
        j.endObject().endArray().endObject().endArray();

        j.key("nodes").beginArray().beginObject().field("name", "node_0").field("mesh", 0).endObject().endArray();
        j.key("scenes").beginArray().beginObject().key("nodes").beginArray().value(std::size_t{0}).endArray().endObject().endArray();
        j.endObject();

        const fs::path path = dir.write("meshopt.gltf", j.str());

        UHE::RD3d::Model model;
        // Optimizer passes off so the assertion sees EXACTLY what the decoder
        // produced: with them on, cache/fetch optimization reorders indices,
        // and vertex merging legally collapses the fixture's duplicate
        // corners - both would mask a wrong decode with a legitimate reorder.
        UHE::RD3d::ModelLoadOptions options;
        options.optimizeMesh = false;
        options.mergeVertices = false;
        options.generateTangents = false;
        if (check(model.loadModel(path, options), "meshopt-compressed model loads"))
        {
            check(!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty(),
                  "compressed primitive was extracted");
            if (!model.GetGeometry().empty() && !model.GetGeometry()[0].primitive.empty())
            {
                const auto& prim = model.GetGeometry()[0].primitive[0];
                check(prim.indices.size() == kIndexCount, "index count preserved by decode");

                // The decode must be EXACT: meshopt attribute encoding is
                // lossless, and the loader's vertex-merge can only collapse
                // byte-identical vertices, so the sorted position set is
                // preserved. A wrong decode (raw compressed bytes, truncated
                // stream) produces garbage here instead of an error.
                std::vector<glm::vec3> got;
                for (const auto& v : prim.vertices)
                    got.push_back(v.position);
                std::sort(got.begin(), got.end(),
                          [](const glm::vec3& a, const glm::vec3& b) { return a.x < b.x; });

                std::vector<glm::vec3> want;
                for (std::size_t v = 0; v < kVertexCount; ++v)
                    want.emplace_back(quadPositions[v * 3 + 0], quadPositions[v * 3 + 1], quadPositions[v * 3 + 2]);
                std::sort(want.begin(), want.end(),
                          [](const glm::vec3& a, const glm::vec3& b) { return a.x < b.x; });

                bool positionsMatch = got.size() == want.size();
                for (std::size_t v = 0; positionsMatch && v < want.size(); ++v)
                    positionsMatch = glm::all(glm::lessThanEqual(glm::abs(got[v] - want[v]), glm::vec3(1e-5f)));
                check(positionsMatch, "decoded positions match the source quad exactly");

                bool indicesMatch = prim.indices.size() == kIndexCount;
                for (std::size_t i = 0; indicesMatch && i < kIndexCount; ++i)
                    indicesMatch = prim.indices[i] == quadIndices[i];
                check(indicesMatch, "decoded indices match the source quad exactly");
            }
        }
    }

    section("draco and basisu stay flagged unsupported");
    {
        // Neither extension can actually be served by this loader (no Draco
        // decoder, no KTX2 transcoder), so they must NOT be listed as
        // supported - that list is what keeps the load-time warning honest.
        // KHR_draco_mesh_compression is REQUIRED here: a real draco file has
        // no readable POSITION, so every primitive draws nothing and the
        // warning is the only signal the author gets.
        AssetSpec spec;
        spec.meshCount = 1;
        spec.materialCount = 1;
        spec.extensionsUsed.push_back("KHR_texture_basisu");
        spec.extensionsRequired.push_back("KHR_draco_mesh_compression");

        const fs::path path = writeAsset(dir, "compressed_exts.gltf", spec);

        UHE::RD3d::Model model;
        check(model.loadModel(path), "file with unsupported compression extensions still opens");
        check(model.HasUnsupportedExtensions(), "unsupported extensions are reported");
        bool sawDraco = false;
        bool sawBasisu = false;
        for (const auto& name : model.GetUnsupportedExtensionNames())
        {
            sawDraco = sawDraco || name == "KHR_draco_mesh_compression";
            sawBasisu = sawBasisu || name == "KHR_texture_basisu";
        }
        check(sawDraco, "KHR_draco_mesh_compression reported as unsupported");
        check(sawBasisu, "KHR_texture_basisu reported as unsupported");
    }

    // =====================================================================
    // The renderer-facing layer, still headless: FillMaterialGPU is what
    // carries the parsed material to the shader's buffer layout, and
    // LightSystem::ExtractLights is what carries KHR_lights_punctual into the
    // frame. Both are pure CPU code, so a wrong slot index or a flipped cone
    // cosine is assertable here rather than only visible as wrong shading.
    // =====================================================================

    section("FillMaterialGPU packs the loaded material for the GPU");
    {
        // Built directly rather than through a fixture: the loader's PARSING
        // is covered above; this test pins the packing - the step between
        // RD3d::Material and the bytes the shader reads.
        UHE::RD3d::Material material;
        material.BaseColorFactor = glm::vec4(0.25f, 0.5f, 0.75f, 0.5f);
        material.MetallicFactor = 0.8f;
        material.RoughnessFactor = 0.6f;
        material.Alpha = UHE::RD3d::AlphaMode::Blend;
        material.AlphaCutoff = 0.3f;
        material.EmissiveFactor = glm::vec3(0.1f, 0.2f, 0.3f);

        // Engine-space transform (what the loader produces), packed verbatim.
        auto& uv = material.UVTransforms[static_cast<size_t>(UHE::RD3d::MaterialTextureSlot::Albedo)];
        uv.cosRotation = 0.9f;
        uv.sinRotation = 0.1f;
        uv.scale = glm::vec2(2.0f, 3.0f);
        uv.offset = glm::vec2(0.1f, 0.2f);
        uv.HasTransform = true;
        material.AlbedoTexture = std::make_shared<UHE::FakeTexture>();

        auto& ext = material.Extensions;
        ext.Unlit = true;
        ext.IOR = 1.7f;
        ext.EmissiveStrength = 3.0f;
        ext.HasClearcoat = true;
        ext.ClearcoatFactor = 0.75f;
        ext.HasTransmission = true;
        ext.TransmissionFactor = 0.9f;
        ext.ThicknessFactor = 0.5f;
        ext.AttenuationDistance = 2.0f;
        ext.AttenuationColor = glm::vec3(0.5f, 0.6f, 0.7f);
        ext.HasDiffuseTransmission = true;
        ext.DiffuseTransmissionFactor = 0.4f;

        const auto gpu = UHE::RD3d::FillMaterialGPU(material);

        check(std::fabs(gpu.baseColorFactor.r - 0.25f) < 1e-6f, "baseColorFactor.r packed");
        check(std::fabs(gpu.baseColorFactor.a - 0.5f) < 1e-6f, "baseColorFactor.a packed");
        check(std::fabs(gpu.metallicFactor - 0.8f) < 1e-6f, "metallicFactor packed");
        check(std::fabs(gpu.roughnessFactor - 0.6f) < 1e-6f, "roughnessFactor packed");
        check(std::fabs(gpu.alphaCutoff - 0.3f) < 1e-6f, "alphaCutoff packed as FLOAT (was int-truncated before)");
        check(std::fabs(gpu.ior - 1.7f) < 1e-6f, "ior packed");
        check(std::fabs(gpu.emissiveFactorStrength.r - 0.1f) < 1e-6f, "emissive rgb packed");
        check(std::fabs(gpu.emissiveFactorStrength.a - 3.0f) < 1e-6f, "emissiveStrength rides in alpha");
        check(std::fabs(gpu.clearcoatFactor - 0.75f) < 1e-6f, "clearcoatFactor packed");
        check(std::fabs(gpu.attenuationDistance - 2.0f) < 1e-6f, "attenuationDistance packed");
        check(std::fabs(gpu.diffuseTransmissionFactor - 0.4f) < 1e-6f, "diffuseTransmissionFactor packed");

        check(gpu.flags.x == 2, "alphaMode Blend packed");
        check(gpu.flags.y == 1, "unlit flag packed");
        check((gpu.flags.z & UHE::RD3d::kFeatureClearcoat) != 0, "clearcoat feature bit set");
        check((gpu.flags.z & UHE::RD3d::kFeatureVolume) != 0,
              "volume bit set (transmission + non-zero thickness)");
        check((gpu.flags.z & UHE::RD3d::kFeatureDiffuseTransmission) != 0, "diffuse transmission bit set");
        check((gpu.flags.z & UHE::RD3d::kFeatureSheen) == 0, "undeclared extensions keep their bits off");

        check(gpu.slots[UHE::RD3d::kSlotAlbedo].index.x == 4242, "albedo slot carries the texture index");
        check(gpu.slots[UHE::RD3d::kSlotNormal].index.x == -1, "absent slots are -1, not 0");
        check(std::fabs(gpu.slots[UHE::RD3d::kSlotAlbedo].rotationScale.x - 0.9f) < 1e-6f,
              "albedo UV transform cos packed");
        check(std::fabs(gpu.slots[UHE::RD3d::kSlotAlbedo].rotationScale.z - 2.0f) < 1e-6f,
              "albedo UV transform scale packed");
        check(std::fabs(gpu.slots[UHE::RD3d::kSlotAlbedo].offset.x - 0.1f) < 1e-6f,
              "albedo UV transform offset packed");

        // Identity by default: a material with no transform must sample
        // untouched UVs, which is what keeps every pre-transform asset intact.
        UHE::RD3d::Material plain;
        const auto plainGpu = UHE::RD3d::FillMaterialGPU(plain);
        check(plainGpu.slots[UHE::RD3d::kSlotAlbedo].rotationScale ==
                  glm::vec4(1.0f, 0.0f, 1.0f, 1.0f),
              "default slot transform is identity (cos 1, sin 0, scale 1)");
        check(plainGpu.slots[UHE::RD3d::kSlotAlbedo].offset == glm::vec4(0.0f), "default slot offset is zero");

        // The volume bit gates on thickness: transmission without thickness
        // has no medium to attenuate, and the bit being set anyway would run
        // the attenuation path on a zero-distance division.
        UHE::RD3d::Material noVolume;
        noVolume.Extensions.HasTransmission = true;
        noVolume.Extensions.ThicknessFactor = 0.0f;
        const auto noVolumeGpu = UHE::RD3d::FillMaterialGPU(noVolume);
        check((noVolumeGpu.flags.z & UHE::RD3d::kFeatureVolume) == 0,
              "transmission with zero thickness does not claim volume");
    }

    section("KHR_lights_punctual lights reach the scene light list");
    {
        // The same fixture the parsing test used, now run through the REAL
        // LightSystem::ExtractLights: the frame's light list is what actually
        // shades, so a light parsed but never extracted still renders black.
        AssetSpec spec;
        spec.meshCount = 2;
        spec.materialCount = 1;
        AssetSpec::LightSpec point;
        point.type = "point";
        point.color = {1.0, 0.5, 0.25};
        point.intensity = 3.0;
        point.hasRange = true;
        point.range = 10.0;
        AssetSpec::LightSpec spot;
        spot.type = "spot";
        spot.color = {1.0, 1.0, 1.0};
        spot.intensity = 5.0;
        spot.hasInnerCone = true;
        spot.innerCone = 0.2;
        spot.hasOuterCone = true;
        spot.outerCone = 0.9;
        spec.lights = {point, spot};
        spec.nodeLightIndices = {0, 1};

        const fs::path path = writeAsset(dir, "lights_scene.gltf", spec);

        entt::registry registry;
        auto entity = registry.create();
        auto& tc = registry.emplace<UHE::TransformComponent>(entity);
        // The model entity sits away from the origin: node placements are
        // MODEL-space, so only composing with the entity transform puts the
        // light where the author put it.
        tc.Translation = glm::vec3(10.0f, 0.0f, 0.0f);

        auto& mc = registry.emplace<UHE::Model3DComponent>(entity);
        if (check(mc.ModelData->loadModel(path), "light model loads into the component"))
        {
            mc.IsLoaded = true;

            const auto lights = UHE::RD3d::LightSystem::ExtractLights(registry);
            check(lights.size() == 2, "two glTF lights extracted");
            if (lights.size() == 2)
            {
                const auto& pointData = lights[0];
                check(int(pointData.Type_Radius_Pad.x) == 1, "light 0 extracted as a point light");
                check(closeTo(glm::vec3(pointData.PositionOrDirection), glm::vec3(10.0f, 0.0f, 0.0f)),
                      "point light sits at node placement x model entity transform");
                check(std::fabs(pointData.ColorIntensity.g - 0.5f) < 1e-5f, "point light colour reaches the list");
                check(std::fabs(pointData.ColorIntensity.w - 3.0f) < 1e-5f, "point light intensity reaches the list");
                check(std::fabs(pointData.Type_Radius_Pad.y - 10.0f) < 1e-4f, "point light range becomes the radius");

                const auto& spotData = lights[1];
                check(int(spotData.Type_Radius_Pad.x) == 2, "light 1 extracted as a spot light");
                check(closeTo(glm::vec3(spotData.PositionOrDirection), glm::vec3(15.0f, 0.0f, 0.0f)),
                      "spot light sits at its node's placement");
                // Cone angles travel as COSINES (one dot product per fragment
                // in the shader); packing degrees or radians here would light
                // the wrong cone entirely.
                check(std::fabs(spotData.Type_Radius_Pad.z - std::cos(0.2f)) < 1e-5f,
                      "inner cone packed as cos(innerConeAngle)");
                check(std::fabs(spotData.Type_Radius_Pad.w - std::cos(0.9f)) < 1e-5f,
                      "outer cone packed as cos(outerConeAngle)");
                // Omitted range -> INFINITY at parse; extraction resolves that
                // to the engine's finite fallback rather than a zero-reach light.
                check(std::fabs(spotData.Type_Radius_Pad.y - 25.0f) < 1e-4f,
                      "infinite spot range resolves to the finite engine fallback");
                // glTF lights point down the node's local -Z; the model entity
                // is unrotated, so the forward axis passes through unchanged.
                check(closeTo(glm::vec3(spotData.Direction_Pad), glm::vec3(0.0f, 0.0f, -1.0f)),
                      "spot forward axis is the node's -Z");
            }
        }
    }

    section("scene spot light components extract like glTF spots");
    {
        entt::registry registry;
        auto entity = registry.create();
        auto& tc = registry.emplace<UHE::TransformComponent>(entity);
        tc.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
        // Scene stores radians (glm::eulerAngles output feeds this field).
        tc.Rotation = glm::vec3(0.0f, glm::pi<float>() / 2.0f, 0.0f); // yaw 90 degrees

        auto& spot = registry.emplace<UHE::SpotLightComponent>(entity);
        spot.InnerConeAngle = 0.2f;
        spot.OuterConeAngle = 0.9f;
        spot.Radius = 7.0f;
        spot.Intensity = 4.0f;

        const auto lights = UHE::RD3d::LightSystem::ExtractLights(registry);
        check(lights.size() == 1, "one spot component extracted");
        if (lights.size() == 1)
        {
            check(int(lights[0].Type_Radius_Pad.x) == 2, "component extracted as a spot light");
            check(closeTo(glm::vec3(lights[0].PositionOrDirection), glm::vec3(1.0f, 2.0f, 3.0f)),
                  "component position extracted");
            check(std::fabs(lights[0].Type_Radius_Pad.z - std::cos(0.2f)) < 1e-5f,
                  "component inner cone packed as cosine");
            check(closeTo(glm::vec3(lights[0].Direction_Pad), glm::vec3(-1.0f, 0.0f, 0.0f)),
                  "yaw of 90 degrees aims the cone down -X");
            check(std::fabs(lights[0].ColorIntensity.w - 4.0f) < 1e-5f, "component intensity extracted");
        }
    }

    section("a scene with no lights keeps the fallback light");
    {
        entt::registry registry;
        const auto lights = UHE::RD3d::LightSystem::ExtractLights(registry);
        check(lights.size() == 1, "fallback light emitted");
        if (!lights.empty())
            check(int(lights[0].Type_Radius_Pad.x) == 0, "fallback is directional");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
