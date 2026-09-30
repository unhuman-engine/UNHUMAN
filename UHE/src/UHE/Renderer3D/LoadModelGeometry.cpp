// CPU-side geometry extraction from a glTF mesh, plus the meshoptimizer passes.
//
// Owns the expensive part of loading: accessor iteration, index synthesis and
// the vertex/index reordering passes. Touches no GPU API - buffer handles stay
// null until LoadModelUpload.cpp fills them - which keeps this translation unit
// compilable into a headless test harness.

#include "uhepch.h"
#include "LoadModel.h"
#include <fastgltf/glm_element_traits.hpp>
#include "fastgltf/math.hpp"
#include "fastgltf/types.hpp"
#include "meshoptimizer.h"

namespace UHE::RD3d
{

namespace
{

bool IsSkinned(const fastgltf::Primitive& primitive)
{
    return primitive.findAttribute("JOINTS_0") != primitive.attributes.end() ||
           primitive.findAttribute("WEIGHTS_0") != primitive.attributes.end();
}

} // namespace

void Model::ExtractGeometry(const fastgltf::Asset& asset, size_t meshIndex, const ModelLoadOptions& options)
{
    const auto& gltfMesh = asset.meshes[meshIndex];

    Geometry outGeom;
    outGeom.name = gltfMesh.name.empty() ? "Unnamed_Mesh" : std::string(gltfMesh.name);

    for (auto& primitive : gltfMesh.primitives)
    {
        Primitive outPrim;
        outPrim.geometryIndex = m_Geometry.size();

        // A material index is optional in glTF and defaults to 0, but a file may
        // still reference one past the end of its material list. Clamp here: the
        // fault would otherwise surface at draw time, far from the load.
        if (primitive.materialIndex.has_value() && primitive.materialIndex.value() >= m_LoadedMaterials.size())
        {
            UHE_CORE_WARN("Primitive references material {0} but the file declares {1}; clamping to 0",
                          primitive.materialIndex.value(), m_LoadedMaterials.size());
            outPrim.materialIndex = 0;
        }
        else
        {
            outPrim.materialIndex = primitive.materialIndex.value_or(0);
        }

        const auto* posAttribute = primitive.findAttribute("POSITION");
        if (posAttribute == primitive.attributes.end())
            continue;

        auto& posAccessor = asset.accessors[posAttribute->accessorIndex];
        outPrim.vertices.resize(posAccessor.count);

        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, posAccessor,
                                                                  [&](fastgltf::math::fvec3 pos, size_t idx)
                                                                  {
                                                                      outPrim.vertices[idx].position =
                                                                          glm::vec3(pos.x(), pos.y(), pos.z());
                                                                      outPrim.vertices[idx].normal =
                                                                          glm::vec3(0.0f, 1.0f, 0.0f); // Default
                                                                      outPrim.vertices[idx].uv = glm::vec2(0.0f);
                                                                  });

        const auto* normalAttribute = primitive.findAttribute("NORMAL");
        if (normalAttribute != primitive.attributes.end())
        {
            auto& normalAccessor = asset.accessors[normalAttribute->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                asset, normalAccessor, [&](fastgltf::math::fvec3 norm, size_t idx)
                { outPrim.vertices[idx].normal = glm::vec3(norm.x(), norm.y(), norm.z()); });
        }

        const auto* uvAttribute = primitive.findAttribute("TEXCOORD_0");
        if (uvAttribute != primitive.attributes.end())
        {
            auto& uvAccessor = asset.accessors[uvAttribute->accessorIndex];

            // NOTE: the V flip is hardcoded. It is correct only for assets from an
            // exporter that flips, and it conflicts with KHR_texture_transform.
            // One place should own UV convention; this is that place until the
            // material work lands.
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                asset, uvAccessor,
                [&](fastgltf::math::fvec2 uv, size_t idx) { outPrim.vertices[idx].uv = glm::vec2(uv.x(), 1.0f - uv.y()); });
        }

        const auto* jointsAttribute = primitive.findAttribute("JOINTS_0");
        if (jointsAttribute != primitive.attributes.end())
        {
            auto& jointsAccessor = asset.accessors[jointsAttribute->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::uvec4>(
                asset, jointsAccessor,
                [&](fastgltf::math::uvec4 joints, size_t idx) { outPrim.vertices[idx].jointIndices = glm::ivec4(joints.x(), joints.y(), joints.z(), joints.w()); });
        }

        const auto* weightsAttribute = primitive.findAttribute("WEIGHTS_0");
        if (weightsAttribute != primitive.attributes.end())
        {
            auto& weightsAccessor = asset.accessors[weightsAttribute->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                asset, weightsAccessor,
                [&](fastgltf::math::fvec4 weights, size_t idx) { outPrim.vertices[idx].jointWeights = glm::vec4(weights.x(), weights.y(), weights.z(), weights.w()); });
        }

        if (primitive.indicesAccessor.has_value())
        {
            auto& indicesAccessor = asset.accessors[primitive.indicesAccessor.value()];
            outPrim.indices.reserve(indicesAccessor.count);

            fastgltf::iterateAccessor<u32>(asset, indicesAccessor,
                                           [&](u32 indexValue) { outPrim.indices.push_back(indexValue); });
        }
        else
        {
            // Non-indexed geometry is legal glTF. Without synthesized indices the
            // index count stays zero and the draw is silently skipped.
            outPrim.indices.resize(outPrim.vertices.size());
            for (size_t i = 0; i < outPrim.vertices.size(); ++i)
                outPrim.indices[i] = static_cast<u32>(i);
        }

        // Drop indices pointing past the vertex array: a malformed or partially
        // decoded file would otherwise read out of bounds at draw time.
        const size_t vertexCount = outPrim.vertices.size();
        outPrim.indices.erase(std::remove_if(outPrim.indices.begin(), outPrim.indices.end(),
                                             [vertexCount](u32 i) { return i >= vertexCount; }),
                              outPrim.indices.end());
        // A triangle list needs whole triples; a trailing partial triangle is
        // dropped rather than read as garbage.
        outPrim.indices.resize(outPrim.indices.size() - (outPrim.indices.size() % 3));

        const bool skinned = IsSkinned(primitive);

        if (options.optimizeMesh && !outPrim.vertices.empty() && !outPrim.indices.empty())
        {
            // 1. Cache optimization: pure permutation, never changes geometry.
            std::vector<u32> cached(outPrim.indices.size());
            meshopt_optimizeVertexCache(cached.data(), outPrim.indices.data(), outPrim.indices.size(),
                                        outPrim.vertices.size());
            outPrim.indices = std::move(cached);

            // 2. Vertex merging. meshopt_generateVertexRemap hashes every byte of
            //    a vertex, so this is only safe where merging cannot change
            //    meaning - never on a skinned primitive, where two vertices on
            //    opposite sides of a joint must stay distinct or the mesh deforms
            //    wrongly.
            if (options.mergeVertices && !skinned)
            {
                std::vector<u32> remap(outPrim.vertices.size(), 0xFFFFFFFFu);
                const size_t unique = meshopt_generateVertexRemap(remap.data(), outPrim.indices.data(),
                                                                outPrim.indices.size(), outPrim.vertices.data(),
                                                                outPrim.vertices.size(), sizeof(Vertex));
                if (unique > 0 && unique < outPrim.vertices.size())
                {
                    std::vector<u32> remapped(outPrim.indices.size());
                    meshopt_remapIndexBuffer(remapped.data(), outPrim.indices.data(), outPrim.indices.size(),
                                             remap.data());

                    std::vector<Vertex> compact(unique);
                    for (size_t i = 0; i < outPrim.vertices.size(); ++i)
                        if (remap[i] != 0xFFFFFFFFu)
                            compact[remap[i]] = outPrim.vertices[i];

                    outPrim.vertices = std::move(compact);
                    outPrim.indices = std::move(remapped);
                }
            }

            // 3. Fetch optimization. Must run on cache-optimized indices.
            //    This is the pass that matters most on GFX9, where vertex fetch
            //    rather than triangle count is the binding constraint.
            std::vector<u32> fetchRemap(outPrim.vertices.size(), 0xFFFFFFFFu);
            const size_t uniqueFetch =
                meshopt_optimizeVertexFetchRemap(fetchRemap.data(), outPrim.indices.data(), outPrim.indices.size(),
                                                 outPrim.vertices.size());
            if (uniqueFetch > 0 && uniqueFetch < outPrim.vertices.size())
            {
                std::vector<u32> fetchIndices(outPrim.indices.size());
                meshopt_remapIndexBuffer(fetchIndices.data(), outPrim.indices.data(), outPrim.indices.size(),
                                         fetchRemap.data());

                std::vector<Vertex> fetchCompact(uniqueFetch);
                for (size_t i = 0; i < outPrim.vertices.size(); ++i)
                    if (fetchRemap[i] != 0xFFFFFFFFu)
                        fetchCompact[fetchRemap[i]] = outPrim.vertices[i];

                outPrim.vertices = std::move(fetchCompact);
                outPrim.indices = std::move(fetchIndices);
            }
        }

        outPrim.IndexCount = static_cast<u32>(outPrim.indices.size());

        if (outPrim.vertices.empty() || outPrim.indices.empty())
            continue;

        outGeom.primitive.push_back(std::move(outPrim));
    }

    // Always push, even when no primitive was drawable, so a geometryIndex
    // recorded in the cache stays valid for a later node that hits the same mesh.
    m_Geometry.push_back(std::move(outGeom));
}

} // namespace UHE::RD3d
