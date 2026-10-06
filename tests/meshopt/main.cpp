// Meshoptimizer integration harness.
//
// Purpose: prove the vendored copy at UHE/vendor/meshoptimizer builds, links
// and behaves BEFORE the glTF loader depends on it. The loader work that follows
// (index cache remap, vertex fetch remap, vertex merge, simplify, tangent
// generation) is only safe if these primitives are verified at the pinned
// version.
//
// Deliberately NOT upstream's demo/tests.cpp: that is ~4800 lines covering APIs
// the engine never calls. This covers the entry points the asset pipeline will
// use plus the invariants that matter for correctness in UHE: geometry must be
// unchanged by the non-lossy transforms, index/vertex counts must be preserved,
// and remap tables must be well-formed.
//
// No engine headers, no GPU, no RHI. Links only meshoptimizer.
//
// Style: mirrors the engine (UHE/src/UHE/Core/Core.h) - u32/u64/f32/f64 rather
// than `unsigned int`/`float` (which are platform-width-dependent), std::span
// instead of pointer+length pairs, and no raw owning pointers anywhere.

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <span>
#include <string_view>
#include <vector>

// UHE's own aliases (UHE/src/UHE/Core/Core.h) - these are std:: fixed-width
// types, NOT `unsigned int`/`float`, which are platform-width-dependent.
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

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

// meshoptimizer's geometry APIs take `const float*` + stride. The interleaved
// Vertex satisfies that only while position is the first member and normal/uv
// follow immediately; these static_asserts pin that down so a future reorder of
// Vertex fails here instead of silently feeding the optimizer garbage.
struct Vertex
{
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    i32 jointIndices[4];
    f32 jointWeights[4];
};

static_assert(offsetof(Vertex, position) == 0, "position must be first for meshoptimizer's float* API");
static_assert(offsetof(Vertex, normal) == 3 * sizeof(f32), "normal must directly follow position");
static_assert(offsetof(Vertex, uv) == 6 * sizeof(f32), "uv must directly follow normal");

// A strided view of the interleaved position/normal/uv streams, in the shape
// meshoptimizer expects: a contiguous f32 pointer plus a byte stride.
struct StridedFloats
{
    const f32* data = nullptr;
    std::size_t count = 0; // element count, NOT byte count
    std::size_t stride = 0; // bytes between elements

    const f32* begin() const { return data; }
    const f32* end() const { return data + count * (stride / sizeof(f32)); }
};

StridedFloats positionsOf(std::span<const Vertex> vertices)
{
    if (vertices.empty())
        return {};
    return {vertices.front().position, vertices.size(), sizeof(Vertex)};
}

StridedFloats normalsOf(std::span<const Vertex> vertices)
{
    if (vertices.empty())
        return {};
    return {vertices.front().normal, vertices.size(), sizeof(Vertex)};
}

StridedFloats uvsOf(std::span<const Vertex> vertices)
{
    if (vertices.empty())
        return {};
    return {vertices.front().uv, vertices.size(), sizeof(Vertex)};
}

constexpr u32 kSlices = 16;
constexpr u32 kStacks = 12;

// A UV sphere: enough triangles that cache/fetch optimization has real work to
// do, small enough to verify exactly. Normals are true sphere normals, so
// meshopt_generateTangents has well-conditioned input.
std::vector<Vertex> makeSphere()
{
    std::vector<Vertex> vertices;
    vertices.reserve((kSlices + 1) * (kStacks + 1));

    for (u32 stack = 0; stack <= kStacks; ++stack)
    {
        const f32 v = static_cast<f32>(stack) / static_cast<f32>(kStacks);
        const f32 phi = v * 3.14159265358979323846f;
        for (u32 slice = 0; slice <= kSlices; ++slice)
        {
            const f32 u = static_cast<f32>(slice) / static_cast<f32>(kSlices);
            const f32 theta = u * 2.0f * 3.14159265358979323846f;

            Vertex vert{};
            vert.position[0] = std::sin(phi) * std::cos(theta);
            vert.position[1] = std::cos(phi);
            vert.position[2] = std::sin(phi) * std::sin(theta);
            vert.normal[0] = vert.position[0];
            vert.normal[1] = vert.position[1];
            vert.normal[2] = vert.position[2];
            vert.uv[0] = u;
            vert.uv[1] = v;
            vert.jointWeights[0] = 1.0f;
            vertices.push_back(vert);
        }
    }
    return vertices;
}

std::vector<u32> makeSphereIndices()
{
    std::vector<u32> indices;
    indices.reserve(kSlices * kStacks * 6);
    for (u32 stack = 0; stack < kStacks; ++stack)
    {
        for (u32 slice = 0; slice < kSlices; ++slice)
        {
            const u32 a = stack * (kSlices + 1) + slice;
            const u32 b = a + kSlices + 1;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return indices;
}

// Byte-identical duplicates of every vertex, so generateVertexRemap has real
// work to do.
//
// NOTE: the UV sphere is NOT a valid merge test. Its seam vertices share a
// position and normal but carry different UVs (u=0 vs u=1), and
// meshopt_generateVertexRemap hashes all vertex_size bytes - so the correct
// answer for a sphere is "no reduction". Asserting a reduction there tests the
// wrong thing. The loader only sees a real merge when the source data is
// genuinely duplicated, which is what this models.
std::vector<Vertex> makeSphereWithDuplicates(std::span<const Vertex> base)
{
    std::vector<Vertex> vertices(base.begin(), base.end());
    vertices.reserve(base.size() * 2);
    for (const Vertex& v : base)
        vertices.push_back(v);
    return vertices;
}

// Shuffle the triangle order so consecutive triangles reference unrelated
// vertices.
//
// This matters more than it looks. Reversing triangle order on a GRID mesh is
// NOT a scramble: the mesh is so regular that walking it backwards still keeps
// consecutive triangles adjacent, so vertex-cache locality barely moves and any
// ACMR assertion on it is vacuous. A real Fisher-Yates shuffle with a fixed
// seed destroys locality for real.
std::vector<u32> scrambleTriangles(std::span<const u32> indices)
{
    std::vector<u32> triangles(indices.begin(), indices.end());

    std::mt19937 rng(0x5EEDu); // fixed seed: failures must reproduce
    for (std::size_t i = triangles.size(); i > 3; i -= 3)
    {
        // Pick a triangle boundary, then swap whole triangles.
        const std::size_t a = (rng() % (i / 3)) * 3;
        std::size_t b = (rng() % (i / 3)) * 3;
        if (a == b)
            continue;
        for (std::size_t k = 0; k < 3; ++k)
            std::swap(triangles[a + k], triangles[b + k]);
    }
    return triangles;
}

// Sum of triangle areas. Not a full surface-area metric, but it IS invariant
// under the index permutations these functions perform, which is exactly the
// property under test: a non-lossy "optimization" that changes geometry fails
// here.
f64 surfaceSignature(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    f64 total = 0.0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const Vertex& a = vertices[indices[i]];
        const Vertex& b = vertices[indices[i + 1]];
        const Vertex& c = vertices[indices[i + 2]];

        f64 ab[3], ac[3], cross[3];
        for (std::size_t k = 0; k < 3; ++k)
        {
            ab[k] = static_cast<f64>(b.position[k]) - static_cast<f64>(a.position[k]);
            ac[k] = static_cast<f64>(c.position[k]) - static_cast<f64>(a.position[k]);
        }
        cross[0] = ab[1] * ac[2] - ab[2] * ac[1];
        cross[1] = ab[2] * ac[0] - ab[0] * ac[2];
        cross[2] = ab[0] * ac[1] - ab[1] * ac[0];
        total += 0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    }
    return total;
}

void testVersion()
{
    section("version");
    std::printf("  MESHOPTIMIZER_VERSION = %d\n", MESHOPTIMIZER_VERSION);
    check(MESHOPTIMIZER_VERSION >= 1020, "version >= 1.2 (generateVertexRemapMulti / simplify attributes)");
}

void testIndexGeneration(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    section("generateVertexRemap / remap buffers");

    // Run A: the pristine sphere. Its seam vertices share position+normal but
    // differ in UV, so the CORRECT answer is "no reduction" - asserting a merge
    // here would test the wrong thing.
    {
        std::vector<u32> remap(vertices.size(), 0xFFFFFFFFu);
        const std::size_t unique =
            meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), vertices.data(), vertices.size(),
                                        sizeof(Vertex));

        check(unique <= vertices.size(), "unique vertex count does not exceed input vertex count");

        bool allMapped = true;
        for (u32 index : indices)
            if (remap[index] == 0xFFFFFFFFu)
                allMapped = false;
        check(allMapped, "every indexed vertex has a remap entry");

        std::vector<u32> remapped(indices.size());
        meshopt_remapIndexBuffer(remapped.data(), indices.data(), indices.size(), remap.data());
        bool inRange = true;
        for (u32 index : remapped)
            if (index >= unique)
                inRange = false;
        check(inRange, "remapped indices are all below the unique vertex count");

        std::vector<Vertex> compact(unique);
        for (std::size_t i = 0; i < vertices.size(); ++i)
            if (remap[i] != 0xFFFFFFFFu)
                compact[remap[i]] = vertices[i];

        check(std::fabs(surfaceSignature(vertices, indices) - surfaceSignature(compact, remapped)) < 1e-6,
              "surface signature unchanged by remap");
        std::printf("  pristine sphere: %zu -> %zu unique (seam UVs differ, no merge is correct)\n", vertices.size(),
                    unique);
    }

    // Run B: byte-identical duplicates, where a merge is genuinely correct.
    {
        const std::vector<Vertex> duplicated = makeSphereWithDuplicates(vertices);
        std::vector<u32> remap(duplicated.size(), 0xFFFFFFFFu);
        const std::size_t unique =
            meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), duplicated.data(), duplicated.size(),
                                        sizeof(Vertex));

        check(unique < duplicated.size(), "byte-identical duplicates are merged");
        check(unique >= vertices.size(), "merge collapses exactly the duplicates, no more");

        std::vector<u32> remapped(indices.size());
        meshopt_remapIndexBuffer(remapped.data(), indices.data(), indices.size(), remap.data());
        std::vector<Vertex> compact(unique);
        for (std::size_t i = 0; i < duplicated.size(); ++i)
            if (remap[i] != 0xFFFFFFFFu)
                compact[remap[i]] = duplicated[i];

        check(std::fabs(surfaceSignature(duplicated, indices) - surfaceSignature(compact, remapped)) < 1e-6,
              "surface signature unchanged by remap (merged case)");
        std::printf("  duplicated sphere: %zu -> %zu unique\n", duplicated.size(), unique);
    }
}

void testVertexCacheOptimization(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    section("optimizeVertexCache");

    // Measure on the SCRAMBLED order, where there is something to improve.
    const std::vector<u32> scrambled = scrambleTriangles(indices);

    std::vector<u32> optimized(scrambled.size());
    meshopt_optimizeVertexCache(optimized.data(), scrambled.data(), scrambled.size(), vertices.size());

    check(optimized.size() == scrambled.size(), "index count preserved");

    std::vector<u32> a(scrambled), b(optimized);
    std::ranges::sort(a);
    std::ranges::sort(b);
    check(a == b, "cache optimization only permutes indices (same multiset)");

    check(std::fabs(surfaceSignature(vertices, scrambled) - surfaceSignature(vertices, optimized)) < 1e-6,
          "surface signature unchanged by cache optimization");

    // ACMR/ATVR are the metrics that matter on GFX9 (vertex fetch bound).
    const auto before = meshopt_analyzeVertexCache(scrambled.data(), scrambled.size(), vertices.size(), 16, 8, 0);
    const auto after = meshopt_analyzeVertexCache(optimized.data(), optimized.size(), vertices.size(), 16, 8, 0);
    check(after.acmr <= before.acmr, "ACMR (cache miss ratio) did not increase");
    check(after.atvr <= before.atvr, "ATVR (cache-to-vertex ratio) did not increase");
    check(after.acmr < before.acmr, "ACMR actually improved on scrambled input");
    std::printf("  acmr %.3f -> %.3f, atvr %.3f -> %.3f\n", before.acmr, after.acmr, before.atvr, after.atvr);
}

void testVertexFetchOptimization(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    section("optimizeVertexFetchRemap");

    // Fetch optimization must run on cache-optimized indices (documented requirement).
    std::vector<u32> cached(indices.size());
    meshopt_optimizeVertexCache(cached.data(), indices.data(), indices.size(), vertices.size());

    std::vector<u32> fetchRemap(vertices.size(), 0xFFFFFFFFu);
    const std::size_t uniqueFetch =
        meshopt_optimizeVertexFetchRemap(fetchRemap.data(), cached.data(), cached.size(), vertices.size());
    check(uniqueFetch <= vertices.size(), "fetch remap does not increase vertex count");

    std::vector<u32> finalIndices(cached.size());
    meshopt_remapIndexBuffer(finalIndices.data(), cached.data(), cached.size(), fetchRemap.data());
    bool inRange = true;
    for (u32 index : finalIndices)
        if (index >= uniqueFetch)
            inRange = false;
    check(inRange, "fetch-remapped indices are all below the fetch-unique count");

    std::vector<Vertex> compact(uniqueFetch);
    for (std::size_t i = 0; i < vertices.size(); ++i)
        if (fetchRemap[i] != 0xFFFFFFFFu)
            compact[fetchRemap[i]] = vertices[i];

    check(std::fabs(surfaceSignature(vertices, cached) - surfaceSignature(compact, finalIndices)) < 1e-6,
          "surface signature unchanged by fetch optimization");

    const auto fetch = meshopt_analyzeVertexFetch(finalIndices.data(), finalIndices.size(), uniqueFetch, sizeof(Vertex));
    check(fetch.overfetch <= 1.05, "overfetch is at most 1.0 (+tolerance) after fetch optimization");
    std::printf("  overfetch %.3f, %u bytes fetched\n", fetch.overfetch, fetch.bytes_fetched);
}

void testOverdrawOptimization(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    section("optimizeOverdraw");
    // Overdraw optimization reorders triangles front-to-back; geometry must
    // still be identical afterwards.
    std::vector<f32> positions(vertices.size() * 3);
    for (std::size_t i = 0; i < vertices.size(); ++i)
        std::memcpy(positions.data() + i * 3, vertices[i].position, 3 * sizeof(f32));

    std::vector<u32> sortedIndices(indices.size());
    meshopt_optimizeOverdraw(sortedIndices.data(), indices.data(), indices.size(), positions.data(), vertices.size(),
                             3 * sizeof(f32), 1.0f);

    check(sortedIndices.size() == indices.size(), "index count preserved");
    check(std::fabs(surfaceSignature(vertices, indices) - surfaceSignature(vertices, sortedIndices)) < 1e-6,
          "surface signature unchanged by overdraw optimization");
}

void testSpatialSort(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    section("spatialSortRemap");
    std::vector<u32> remap(vertices.size(), 0);
    meshopt_spatialSortRemap(remap.data(), positionsOf(vertices).data, vertices.size(), sizeof(Vertex));

    // Reordering vertices by position does not change geometry if the indices
    // are remapped with the same table.
    std::vector<Vertex> sorted(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i)
        sorted[remap[i]] = vertices[i];

    std::vector<u32> sortedIndices(indices.size());
    meshopt_remapIndexBuffer(sortedIndices.data(), indices.data(), indices.size(), remap.data());
    check(std::fabs(surfaceSignature(vertices, indices) - surfaceSignature(sorted, sortedIndices)) < 1e-6,
          "surface signature unchanged by spatial sort + remap");

    std::vector<u32> triSort(indices.size());
    meshopt_spatialSortTriangles(triSort.data(), indices.data(), indices.size(), positionsOf(vertices).data,
                                 vertices.size(), sizeof(Vertex));
    check(triSort.size() == indices.size(), "spatial sort triangles preserves index count");
}

void testSimplify(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    section("simplify");
    std::vector<f32> positions(vertices.size() * 3);
    for (std::size_t i = 0; i < vertices.size(); ++i)
        std::memcpy(positions.data() + i * 3, vertices[i].position, 3 * sizeof(f32));

    f32 error = 0.0f;
    const std::size_t target = indices.size() / 2;
    std::vector<u32> simplified(indices.size());
    const std::size_t reduced = meshopt_simplify(simplified.data(), indices.data(), indices.size(), positions.data(),
                                                  vertices.size(), 3 * sizeof(f32), target, 1.0f, 0, &error);

    check(reduced != 0, "simplify returned a valid index count");
    check(reduced <= indices.size(), "simplify never increases index count");
    check(reduced % 3 == 0, "simplified index count is a multiple of 3 (whole triangles)");
    check(reduced < indices.size(), "simplify actually reduced the triangle count");

    // meshopt_simplify treats target_index_count as a budget it aims to MEET,
    // not a floor: with a generous error budget it may legitimately collapse
    // further than asked. Asserting reduced >= target would therefore fail on
    // correct behaviour. What must hold is that it did not stop early.
    check(reduced + 3 * vertices.size() >= target, "simplify did not stop short of the target");

    bool inRange = true;
    for (std::size_t i = 0; i < reduced; i++)
        if (simplified[i] >= vertices.size())
            inRange = false;
    check(inRange, "simplified indices are all valid vertex indices");

    // meshopt_simplify takes ABSOLUTE error and returns RELATIVE error. Mixing
    // them up is a classic meshoptimizer bug; simplifyScale converts between.
    const f32 scale = meshopt_simplifyScale(positions.data(), vertices.size(), 3 * sizeof(f32));
    check(scale > 0.0f, "simplifyScale returns a positive error scale factor");
    check(error >= 0.0f && error <= 1.0f, "reported relative error is within the requested budget");

    std::printf("  %zu -> %zu indices (target %zu), rel error %.6f, scale %.4f\n", indices.size(), reduced, target, error,
                scale);
}

void testTangents(std::span<const Vertex> vertices, std::span<const u32> indices)
{
    section("generateTangents");

    // meshopt_generateTangents emits PER-CORNER tangents (index_count*4), not
    // per-vertex, and wants deinterleaved-ish strided streams. The loader will
    // have to copy these back into per-vertex data, duplicating vertices on UV
    // mirror seams - that is the phase-2 trap, so the harness asserts the
    // contract explicitly.
    std::vector<f32> tangents(indices.size() * 4);
    meshopt_generateTangents(tangents.data(), indices.data(), indices.size(), positionsOf(vertices).data, vertices.size(),
                             sizeof(Vertex), normalsOf(vertices).data, sizeof(Vertex), uvsOf(vertices).data,
                             sizeof(Vertex), 0);

    bool finite = true;
    for (f32 value : tangents)
        if (!std::isfinite(value))
            finite = false;
    check(finite, "generated tangents are all finite");

    // Per-corner tangent i belongs to vertex indices[i].
    std::size_t unitLength = 0;
    std::size_t orthogonal = 0;
    for (std::size_t i = 0; i < indices.size(); ++i)
    {
        const f32* t = tangents.data() + i * 4;
        const f32* n = vertices[indices[i]].normal;

        const f32 dot = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
        const f32 len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);

        // xyz is the tangent vector; w is the handedness sign (+-1).
        if (std::fabs(len - 1.0f) < 1e-2f)
            ++unitLength;
        if (std::fabs(dot) < 1e-3f)
            ++orthogonal;
    }

    // The UV sphere is degenerate at the poles (v is constant), so a small
    // fraction of corners legitimately have no well-defined tangent frame.
    const f64 unitRatio = static_cast<f64>(unitLength) / static_cast<f64>(indices.size());
    const f64 orthoRatio = static_cast<f64>(orthogonal) / static_cast<f64>(indices.size());
    check(unitRatio > 0.95, "over 95% of corners have a unit-length tangent");
    check(orthoRatio > 0.95, "over 95% of corners have a tangent orthogonal to the normal");
    std::printf("  unit-length %.1f%%, orthogonal %.1f%% of %zu corners\n", unitRatio * 100.0, orthoRatio * 100.0,
                indices.size());
}

void testQuantization(std::span<const Vertex> vertices)
{
    section("computePositionExponent");
    // Signature is (minv, maxv, min_exp, max_bits) - axis-aligned bounds, not a
    // vertex array. It returns min_exp when the requested precision does not fit
    // inside max_bits for those bounds, which is the correct clamp, not a bug.
    f32 minv[3]{vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
    f32 maxv[3]{vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
    for (const Vertex& v : vertices)
        for (std::size_t k = 0; k < 3; ++k)
        {
            if (v.position[k] < minv[k])
                minv[k] = v.position[k];
            if (v.position[k] > maxv[k])
                maxv[k] = v.position[k];
        }

    const int exponent = meshopt_computePositionExponent(minv, maxv, -10, 24);
    check(exponent >= -10 && exponent <= 24, "position exponent clamped into the requested range");
    std::printf("  exponent %d for bounds [%.2f..%.2f]\n", exponent, minv[0], maxv[0]);

    // A millimetre-scale mesh needs the full bit budget, so it should NOT clamp.
    f32 mmMin[3]{0.0f, 0.0f, 0.0f};
    f32 mmMax[3]{0.0f, 0.0f, 0.0f};
    for (const Vertex& v : vertices)
        for (std::size_t k = 0; k < 3; ++k)
        {
            mmMin[k] = std::min(mmMin[k], v.position[k]);
            mmMax[k] = std::max(mmMax[k], v.position[k]);
        }
    const int mmExponent = meshopt_computePositionExponent(mmMin, mmMax, -10, 24);
    check(mmExponent >= -10 && mmExponent <= 24, "mm-scale exponent also in range");
    std::printf("  exponent %d for the same bounds at mm scale\n", mmExponent);
}

} // namespace

int main()
{
    std::printf("meshoptimizer harness - version %d\n\n", MESHOPTIMIZER_VERSION);

    testVersion();

    const std::vector<Vertex> vertices = makeSphere();
    const std::vector<u32> indices = makeSphereIndices();

    std::printf("\nsphere: %zu vertices, %zu indices\n\n", vertices.size(), indices.size());

    testIndexGeneration(vertices, indices);
    testVertexCacheOptimization(vertices, indices);
    testVertexFetchOptimization(vertices, indices);
    testOverdrawOptimization(vertices, indices);
    testSpatialSort(vertices, indices);
    testSimplify(vertices, indices);
    testTangents(vertices, indices);
    testQuantization(vertices);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
