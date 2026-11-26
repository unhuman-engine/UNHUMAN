#include "uhepch.h"
#include "UHE/AssetsManager/PrimitiveGltf.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace UHE
{
namespace
{
struct Vertex
{
    std::array<float, 3> Position;
    std::array<float, 3> Normal;
    std::array<float, 2> Uv;
};

/// One quad facing \p normal, its corners `centre ± u ± v`, wound counter-clockwise seen from the front.
void AddQuad(std::vector<Vertex>& vertices, std::vector<std::uint16_t>& indices, const std::array<float, 3>& centre,
             const std::array<float, 3>& u, const std::array<float, 3>& v, const std::array<float, 3>& normal)
{
    const auto base = static_cast<std::uint16_t>(vertices.size());
    const std::array<std::array<float, 2>, 4> corners = {{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
    for (const auto& corner : corners)
    {
        Vertex vertex{};
        for (int axis = 0; axis < 3; ++axis)
            vertex.Position[axis] = centre[axis] + corner[0] * u[axis] + corner[1] * v[axis];
        vertex.Normal = normal;
        vertex.Uv = {(corner[0] + 1.0f) * 0.5f, 1.0f - (corner[1] + 1.0f) * 0.5f};
        vertices.push_back(vertex);
    }
    for (const int i : {0, 1, 2, 0, 2, 3})
        indices.push_back(static_cast<std::uint16_t>(base + i));
}

void BuildShape(PrimitiveShape shape, std::vector<Vertex>& vertices, std::vector<std::uint16_t>& indices)
{
    // u x v = normal for every face, so each quad faces outwards.
    if (shape == PrimitiveShape::Plane)
    {
        AddQuad(vertices, indices, {0, 0, 0}, {0.5f, 0, 0}, {0, 0, -0.5f}, {0, 1, 0});
        return;
    }
    AddQuad(vertices, indices, {0.5f, 0, 0}, {0, 0, -0.5f}, {0, 0.5f, 0}, {1, 0, 0});
    AddQuad(vertices, indices, {-0.5f, 0, 0}, {0, 0, 0.5f}, {0, 0.5f, 0}, {-1, 0, 0});
    AddQuad(vertices, indices, {0, 0.5f, 0}, {0.5f, 0, 0}, {0, 0, -0.5f}, {0, 1, 0});
    AddQuad(vertices, indices, {0, -0.5f, 0}, {0.5f, 0, 0}, {0, 0, 0.5f}, {0, -1, 0});
    AddQuad(vertices, indices, {0, 0, 0.5f}, {0.5f, 0, 0}, {0, 0.5f, 0}, {0, 0, 1});
    AddQuad(vertices, indices, {0, 0, -0.5f}, {-0.5f, 0, 0}, {0, 0.5f, 0}, {0, 0, -1});
}

std::string Floats(const std::array<float, 3>& values)
{
    std::ostringstream out;
    out << '[' << values[0] << ',' << values[1] << ',' << values[2] << ']';
    return out.str();
}

void SetError(std::string* error, std::string message)
{
    if (error)
        *error = std::move(message);
}
} // namespace

bool WritePrimitiveGltf(const std::filesystem::path& path, PrimitiveShape shape, const std::array<float, 4>& baseColor,
                        float roughness, std::string* error)
{
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    BuildShape(shape, vertices, indices);

    // Layout of the .bin: interleaved vertices, then the indices.
    const std::size_t vertexBytes = vertices.size() * sizeof(Vertex);
    const std::size_t indexBytes = indices.size() * sizeof(std::uint16_t);
    std::vector<char> bin(vertexBytes + indexBytes);
    std::memcpy(bin.data(), vertices.data(), vertexBytes);
    std::memcpy(bin.data() + vertexBytes, indices.data(), indexBytes);

    std::array<float, 3> min{1e9f, 1e9f, 1e9f};
    std::array<float, 3> max{-1e9f, -1e9f, -1e9f};
    for (const Vertex& vertex : vertices)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            min[axis] = std::min(min[axis], vertex.Position[axis]);
            max[axis] = std::max(max[axis], vertex.Position[axis]);
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path binPath = std::filesystem::path(path).replace_extension(".bin");
    {
        std::ofstream file(binPath, std::ios::binary);
        file.write(bin.data(), static_cast<std::streamsize>(bin.size()));
        if (!file)
        {
            SetError(error, "could not write '" + binPath.string() + "'");
            return false;
        }
    }

    const std::string name = path.stem().string();
    std::ostringstream json;
    json
        << "{\n"
        << R"(  "asset": {"version": "2.0", "generator": "UHE starter content"},)" << '\n'
        << R"(  "scene": 0, "scenes": [{"nodes": [0]}],)" << '\n'
        << R"(  "nodes": [{"mesh": 0, "name": ")" << name << R"("}],)" << '\n'
        << R"(  "meshes": [{"name": ")" << name
        << R"(", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],)"
        << '\n'
        << R"(  "materials": [{"name": ")" << name << R"(", "pbrMetallicRoughness": {"baseColorFactor": [)"
        << baseColor[0] << ',' << baseColor[1] << ',' << baseColor[2] << ',' << baseColor[3]
        << R"(], "metallicFactor": 0, "roughnessFactor": )" << roughness << "}}],\n"
        << R"(  "buffers": [{"uri": ")" << binPath.filename().string() << R"(", "byteLength": )" << bin.size()
        << "}],\n"
        << R"(  "bufferViews": [)" << '\n'
        << R"(    {"buffer": 0, "byteOffset": 0, "byteLength": )" << vertexBytes << R"(, "byteStride": )"
        << sizeof(Vertex) << R"(, "target": 34962},)" << '\n'
        << R"(    {"buffer": 0, "byteOffset": )" << vertexBytes << R"(, "byteLength": )" << indexBytes
        << R"(, "target": 34963}],)" << '\n'
        << R"(  "accessors": [)" << '\n'
        << R"(    {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": )" << vertices.size()
        << R"(, "type": "VEC3", "min": )" << Floats(min) << R"(, "max": )" << Floats(max) << "},\n"
        << R"(    {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": )" << vertices.size()
        << R"(, "type": "VEC3"},)" << '\n'
        << R"(    {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": )" << vertices.size()
        << R"(, "type": "VEC2"},)" << '\n'
        << R"(    {"bufferView": 1, "byteOffset": 0, "componentType": 5123, "count": )" << indices.size()
        << R"(, "type": "SCALAR"}]})" << '\n';

    std::ofstream file(path);
    file << json.str();
    if (!file)
    {
        SetError(error, "could not write '" + path.string() + "'");
        return false;
    }
    return true;
}
} // namespace UHE
