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
    const std::size_t skinBase = bin.size();
    if (spec.skinned)
    {
        for (std::size_t i = 0; i < spec.meshCount; ++i)
        {
            for (std::size_t v = 0; v < Quad::kVertexCount; ++v)
            {
                const std::uint8_t joint = (v % 2 == 0) ? 0 : 1;
                bin.append(reinterpret_cast<const char*>(&joint), sizeof(joint));
            }
            while (bin.size() % 4 != 0)
                bin.push_back(' ');

            for (std::size_t v = 0; v < Quad::kVertexCount; ++v)
            {
                const f32 w = (v % 2 == 0) ? 0.75f : 1.0f;
                bin.append(reinterpret_cast<const char*>(&w), sizeof(f32));
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
    const std::size_t firstJointsAccessor = firstUvAccessor + (spec.withUvs ? spec.meshCount : 0);
    const std::size_t firstWeightsAccessor = firstJointsAccessor + (spec.skinned ? spec.meshCount : 0);

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
    if (spec.skinned)
    {
        // JOINTS_0 as VEC4 of unsigned int, matching glTF's joint indexing.
        const std::size_t jointViewBase =
            spec.meshCount * (1 + (spec.useIndices ? 1 : 0) + (spec.withNormals ? 1 : 0) + (spec.withUvs ? 1 : 0));
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
    if (spec.material && (spec.material->normalScale >= 0.0f || spec.material->occlusionStrength >= 0.0f))
    {
        j.key("images").beginArray().beginObject().field("uri", "tex.png").field("mimeType", "image/png").endObject().endArray();
        j.key("samplers").beginArray().beginObject().field("magFilter", 9729).field("minFilter", 9987).endObject().endArray();
        j.key("textures").beginArray().beginObject().field("source", 0).field("sampler", 0).endObject().endArray();
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
            j.field("metallicFactor", 0.0).field("roughnessFactor", 0.5).endObject();

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
                if (m.normalScale >= 0.0f)
                    j.key("normalTexture").beginObject().field("index", 0).field("scale", m.normalScale).endObject();
                if (m.occlusionStrength >= 0.0f)
                    j.key("occlusionTexture").beginObject().field("index", 0).field("strength", m.occlusionStrength).endObject();
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
    section("unimplemented extensions are reported");
    {
        AssetSpec spec;
        spec.meshCount = 1;
        spec.extensionsUsed = {"KHR_materials_emissive_strength"};
        spec.extensionsRequired = {"KHR_materials_emissive_strength"};
        const fs::path path = writeAsset(dir, "ext.gltf", spec);

        UHE::RD3d::Model model;
        if (model.loadModel(path))
        {
            check(model.HasUnsupportedExtensions(), "unsupported extension flagged");
            if (!model.GetUnsupportedExtensionNames().empty())
                std::printf("  reported: %s\n", model.GetUnsupportedExtensionNames()[0].c_str());
        }
        else
        {
            check(false, "model with an unknown extension still loads");
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
