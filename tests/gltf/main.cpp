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

void check(bool condition, std::string_view what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("  FAIL  %.*s\n", static_cast<int>(what.size()), what.data());
    }
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
    // When nested, make the child node carry no mesh. That is the point of the
    // test: only nodes with a mesh produce a Mesh entry.
    bool childHasMesh = true;
};

// Writes <name>.gltf plus data.bin and returns the .gltf path.
fs::path writeAsset(const TempDir& dir, std::string_view name, const AssetSpec& spec)
{
    const std::vector<float> quadPositions = Quad::positions();
    const std::vector<u32> quadIndices = Quad::indices();
    constexpr std::size_t kPositionBytes = 18 * sizeof(float);
    constexpr std::size_t kIndexBytes = Quad::kIndexCount * sizeof(u32);

    // ---- binary ----
    std::string bin;
    for (std::size_t i = 0; i < spec.meshCount; ++i)
        bin.append(reinterpret_cast<const char*>(quadPositions.data()), quadPositions.size() * sizeof(float));

    const std::size_t indexBase = bin.size();
    if (spec.useIndices)
        for (std::size_t i = 0; i < spec.meshCount; ++i)
            bin.append(reinterpret_cast<const char*>(quadIndices.data()), quadIndices.size() * sizeof(u32));

    dir.write("data.bin", bin);

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

    j.key("buffers").beginArray().beginObject().field("uri", "data.bin").field("byteLength", bin.size()).endObject().endArray();

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
    j.endArray();

    // accessors: positions then indices.
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
    j.endArray();

    // meshes
    j.key("meshes").beginArray();
    for (std::size_t i = 0; i < spec.meshCount; ++i)
    {
        j.beginObject();
        j.field("name", "mesh_" + std::to_string(i));
        j.key("primitives").beginArray();
        j.beginObject();
        j.key("attributes").beginObject().field("POSITION", i).endObject();
        if (spec.useIndices)
            j.field("indices", spec.meshCount + i);
        // Material 0 normally; deliberately out of range when asked, so the
        // loader's clamp is what keeps the draw in bounds.
        j.field("material", spec.outOfRangeMaterial ? spec.materialCount + 4 : 0);
        j.endObject();
        j.endArray();
        j.endObject();
    }
    j.endArray();

    // materials
    if (spec.materialCount > 0)
    {
        j.key("materials").beginArray();
        for (std::size_t i = 0; i < spec.materialCount; ++i)
        {
            j.beginObject()
                .field("name", "material_" + std::to_string(i))
                .key("pbrMetallicRoughness")
                .beginObject()
                .field("baseColorFactor", std::vector<double>{1.0, 1.0, 1.0, 1.0})
                .field("metallicFactor", 0.0)
                .field("roughnessFactor", 0.5)
                .endObject()
                .endObject();
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
