#include "uhepch.h"
#include "UHE/Scene/Terrain.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stb_image.h>
#include "UHE/Scene/Components.h"

namespace UHE::Terrain
{
namespace
{
constexpr int kMinResolution = 8;
constexpr int kMaxResolution = 1024;

int ClampedResolution(const TerrainComponent& terrain)
{
    return std::clamp(terrain.Resolution, kMinResolution, kMaxResolution);
}

/// 64-bit FNV-1a over raw bytes.
void Mix(u64& hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

template <class T> void Mix(u64& hash, const T& value)
{
    Mix(hash, &value, sizeof(T));
}

/// A repeatable pseudo-random value in [0, 1) for lattice point (x, z) and seed.
float Lattice(int x, int z, int seed)
{
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(z) * 668265263u + static_cast<u32>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xffffffu) / static_cast<float>(0x1000000u);
}

/// Smooth value noise in [0, 1].
float ValueNoise(float x, float z, int seed)
{
    const float fx = std::floor(x);
    const float fz = std::floor(z);
    const int ix = static_cast<int>(fx);
    const int iz = static_cast<int>(fz);
    const float tx = x - fx;
    const float tz = z - fz;
    // Quintic fade: no visible grid in the slopes.
    const auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float u = fade(tx);
    const float v = fade(tz);
    const float a = Lattice(ix, iz, seed);
    const float b = Lattice(ix + 1, iz, seed);
    const float c = Lattice(ix, iz + 1, seed);
    const float d = Lattice(ix + 1, iz + 1, seed);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

Heightfield Noise(const TerrainComponent& terrain, u32 samples, float size)
{
    Heightfield field;
    field.Samples = samples;
    field.Size = size;
    field.Heights.resize(static_cast<std::size_t>(samples) * samples);
    const int octaves = std::clamp(terrain.Octaves, 1, 8);
    const float roughness = std::clamp(terrain.Roughness, 0.0f, 1.0f);
    const float frequency = 1.0f / std::max(terrain.FeatureSize, 0.5f);
    const float step = size / static_cast<float>(samples - 1);
    float lowest = 1e30f;
    float highest = -1e30f;
    for (u32 z = 0; z < samples; ++z)
    {
        for (u32 x = 0; x < samples; ++x)
        {
            const float wx = static_cast<float>(x) * step;
            const float wz = static_cast<float>(z) * step;
            float value = 0.0f;
            float amplitude = 1.0f;
            float scale = frequency;
            for (int octave = 0; octave < octaves; ++octave)
            {
                value += ValueNoise(wx * scale, wz * scale, terrain.Seed + octave * 1013) * amplitude;
                amplitude *= roughness;
                scale *= 2.0f;
            }
            field.Heights[static_cast<std::size_t>(z) * samples + x] = value;
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
    }
    // Lowest point at 0, highest at Height; squared so valleys open out and
    // peaks stand up, as eroded ground does.
    const float range = std::max(highest - lowest, 1e-6f);
    for (float& h : field.Heights)
    {
        const float t = (h - lowest) / range;
        h = t * t * terrain.Height;
    }
    return field;
}

bool Heightmap(const std::string& path, const TerrainComponent& terrain, u32 samples, float size, Heightfield& out)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(0);
    stbi_us* pixels = stbi_load_16(path.c_str(), &width, &height, &channels, 1);
    if (pixels == nullptr || width < 2 || height < 2)
    {
        if (pixels != nullptr)
            stbi_image_free(pixels);
        return false;
    }
    // Sampled bilinearly over the whole image, each axis on its own: a
    // rectangular image is stretched to the square terrain, not cropped.
    out.Samples = samples;
    out.Size = size;
    out.Heights.resize(static_cast<std::size_t>(samples) * samples);
    const auto pixel = [&](int px, int py)
    {
        px = std::clamp(px, 0, width - 1);
        py = std::clamp(py, 0, height - 1);
        // Image rows run top to bottom; the top row is the far (-Z) edge.
        return static_cast<float>(pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) +
                                         static_cast<std::size_t>(px)]) /
               65535.0f;
    };
    const float last = static_cast<float>(samples - 1);
    for (u32 z = 0; z < samples; ++z)
    {
        const float v = static_cast<float>(z) / last * static_cast<float>(height - 1);
        const int py = static_cast<int>(v);
        const float ty = v - static_cast<float>(py);
        for (u32 x = 0; x < samples; ++x)
        {
            const float u = static_cast<float>(x) / last * static_cast<float>(width - 1);
            const int px = static_cast<int>(u);
            const float tx = u - static_cast<float>(px);
            const float top = pixel(px, py) + (pixel(px + 1, py) - pixel(px, py)) * tx;
            const float bottom = pixel(px, py + 1) + (pixel(px + 1, py + 1) - pixel(px, py + 1)) * tx;
            out.Heights[static_cast<std::size_t>(z) * samples + x] = (top + (bottom - top) * ty) * terrain.Height;
        }
    }
    stbi_image_free(pixels);
    return true;
}

glm::vec3 SrgbToLinear(const glm::vec3& c)
{
    const auto channel = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {channel(c.r), channel(c.g), channel(c.b)};
}
} // namespace

u64 BuildKey(const TerrainComponent& terrain)
{
    u64 hash = 14695981039346656037ull;
    Mix(hash, terrain.HeightmapPath.data(), terrain.HeightmapPath.size());
    Mix(hash, terrain.Size);
    Mix(hash, terrain.Height);
    Mix(hash, terrain.Resolution);
    Mix(hash, terrain.Seed);
    Mix(hash, terrain.FeatureSize);
    Mix(hash, terrain.Octaves);
    Mix(hash, terrain.Roughness);
    Mix(hash, terrain.SandColor);
    Mix(hash, terrain.GrassColor);
    Mix(hash, terrain.RockColor);
    Mix(hash, terrain.SnowColor);
    Mix(hash, terrain.SandLevel);
    Mix(hash, terrain.SnowLevel);
    Mix(hash, terrain.RockSlope);
    return hash == 0 ? 1 : hash; // 0 means "never built"
}

Heightfield BuildHeightfield(const TerrainComponent& terrain, const std::string& resolvedHeightmap)
{
    const u32 samples = static_cast<u32>(ClampedResolution(terrain)) + 1;
    const float size = std::max(terrain.Size, 0.1f);
    Heightfield field;
    if (!resolvedHeightmap.empty())
    {
        if (Heightmap(resolvedHeightmap, terrain, samples, size, field))
            return field;
        UHE_CORE_ERROR("Terrain: the heightmap '{0}' did not load; using generated noise", resolvedHeightmap);
    }
    return Noise(terrain, samples, size);
}

float HeightAt(const Heightfield& field, float x, float z)
{
    if (field.Samples < 2 || field.Heights.empty())
        return 0.0f;
    const float last = static_cast<float>(field.Samples - 1);
    const float gx = std::clamp((x / field.Size + 0.5f) * last, 0.0f, last);
    const float gz = std::clamp((z / field.Size + 0.5f) * last, 0.0f, last);
    const u32 x0 = std::min(static_cast<u32>(gx), field.Samples - 2);
    const u32 z0 = std::min(static_cast<u32>(gz), field.Samples - 2);
    const float tx = gx - static_cast<float>(x0);
    const float tz = gz - static_cast<float>(z0);
    const auto at = [&](u32 ix, u32 iz) { return field.Heights[static_cast<std::size_t>(iz) * field.Samples + ix]; };
    const float top = at(x0, z0) + (at(x0 + 1, z0) - at(x0, z0)) * tx;
    const float bottom = at(x0, z0 + 1) + (at(x0 + 1, z0 + 1) - at(x0, z0 + 1)) * tx;
    return top + (bottom - top) * tz;
}

Heightfield Resample(const Heightfield& field, u32 samples)
{
    Heightfield out;
    out.Samples = std::max(samples, 2u);
    out.Size = field.Size;
    out.Heights.resize(static_cast<std::size_t>(out.Samples) * out.Samples);
    const float step = field.Size / static_cast<float>(out.Samples - 1);
    for (u32 z = 0; z < out.Samples; ++z)
        for (u32 x = 0; x < out.Samples; ++x)
            out.Heights[static_cast<std::size_t>(z) * out.Samples + x] =
                HeightAt(field, static_cast<float>(x) * step - field.Size * 0.5f,
                         static_cast<float>(z) * step - field.Size * 0.5f);
    return out;
}

RD3d::Primitive BuildPrimitive(const Heightfield& field, const TerrainComponent& terrain)
{
    RD3d::Primitive prim;
    const u32 n = field.Samples;
    if (n < 2)
        return prim;
    const float step = field.Size / static_cast<float>(n - 1);
    const float half = field.Size * 0.5f;
    const auto h = [&](int ix, int iz)
    {
        ix = std::clamp(ix, 0, static_cast<int>(n) - 1);
        iz = std::clamp(iz, 0, static_cast<int>(n) - 1);
        return field.Heights[static_cast<std::size_t>(iz) * n + static_cast<std::size_t>(ix)];
    };

    const glm::vec3 sand = SrgbToLinear(terrain.SandColor);
    const glm::vec3 grass = SrgbToLinear(terrain.GrassColor);
    const glm::vec3 rock = SrgbToLinear(terrain.RockColor);
    const glm::vec3 snow = SrgbToLinear(terrain.SnowColor);
    const float height = std::max(terrain.Height, 1e-4f);
    const float rockCos = std::cos(glm::radians(std::clamp(terrain.RockSlope, 1.0f, 89.0f)));
    const auto blend = [](float edge, float width, float value)
    { return std::clamp((value - edge) / width + 0.5f, 0.0f, 1.0f); };

    prim.vertices.resize(static_cast<std::size_t>(n) * n);
    for (u32 z = 0; z < n; ++z)
    {
        for (u32 x = 0; x < n; ++x)
        {
            const int ix = static_cast<int>(x);
            const int iz = static_cast<int>(z);
            RD3d::Vertex& v = prim.vertices[static_cast<std::size_t>(z) * n + x];
            const float y = h(ix, iz);
            v.position = glm::vec3(static_cast<float>(x) * step - half, y, static_cast<float>(z) * step - half);
            // Central differences inside; one-sided at the edges, over the one
            // cell they actually span.
            const int last = static_cast<int>(n) - 1;
            const float spanX = static_cast<float>(std::min(ix + 1, last) - std::max(ix - 1, 0)) * step;
            const float spanZ = static_cast<float>(std::min(iz + 1, last) - std::max(iz - 1, 0)) * step;
            const float dx = (h(ix + 1, iz) - h(ix - 1, iz)) / spanX;
            const float dz = (h(ix, iz + 1) - h(ix, iz - 1)) / spanZ;
            v.normal = glm::normalize(glm::vec3(-dx, 1.0f, -dz));
            v.uv = glm::vec2(v.position.x, v.position.z) * 0.25f;
            v.tangent = glm::vec4(glm::normalize(glm::vec3(1.0f, dx, 0.0f)), 1.0f);

            // Layers: sand low, grass, snow high; rock wherever it is steep.
            const float level = y / height;
            glm::vec3 color = glm::mix(sand, grass, blend(terrain.SandLevel, 0.04f, level));
            color = glm::mix(color, snow, blend(terrain.SnowLevel, 0.06f, level));
            color = glm::mix(color, rock, blend(rockCos, -0.08f, v.normal.y));
            // A little variation, so wide areas of one layer are not flat paint.
            const float jitter = 0.92f + 0.16f * ValueNoise(v.position.x * 0.35f, v.position.z * 0.35f, 977);
            v.color = glm::vec4(color * jitter, 1.0f);
        }
    }
    prim.hasVertexColor = true;

    prim.indices.reserve(static_cast<std::size_t>(n - 1) * (n - 1) * 6);
    for (u32 z = 0; z + 1 < n; ++z)
    {
        for (u32 x = 0; x + 1 < n; ++x)
        {
            const u32 v00 = z * n + x;
            const u32 v10 = v00 + 1;
            const u32 v01 = v00 + n;
            const u32 v11 = v01 + 1;
            // Counter-clockwise seen from above (+Y), as glTF front faces are.
            prim.indices.insert(prim.indices.end(), {v00, v01, v10, v10, v01, v11});
        }
    }
    return prim;
}
} // namespace UHE::Terrain
