#include "uhepch.h"
#include "MeshoptDecode.h"
#include <fastgltf/tools.hpp>
#include <meshoptimizer.h>

namespace UHE::RD3d
{

namespace
{

// meshopt_decodeVertexBuffer requires a destination aligned to at least 4
// bytes; 16 is what the codec actually touches with wide stores. std::vector
// gives no alignment guarantee, so the scratch is over-allocated and the
// returned span starts at the first aligned offset inside it.
constexpr std::size_t kDecodeAlignment = 16;

std::byte* AlignedPointer(std::byte* base)
{
    auto address = reinterpret_cast<std::uintptr_t>(base);
    auto misalign = address % kDecodeAlignment;
    return base + ((kDecodeAlignment - misalign) % kDecodeAlignment);
}

} // namespace

std::span<const std::byte> GetBufferViewBytes(const fastgltf::Asset& asset, std::size_t bufferViewIndex,
                                              std::vector<std::byte>& scratch)
{
    if (bufferViewIndex >= asset.bufferViews.size())
    {
        UHE_CORE_ERROR("Buffer view {0} is out of range", bufferViewIndex);
        return {};
    }

    const auto& bufferView = asset.bufferViews[bufferViewIndex];

    if (!bufferView.meshoptCompression)
    {
        if (bufferView.bufferIndex >= asset.buffers.size())
        {
            UHE_CORE_ERROR("Buffer view {0} references buffer {1} which does not exist", bufferViewIndex,
                           bufferView.bufferIndex);
            return {};
        }

        const auto data = std::visit(
            fastgltf::visitor{
                [](const fastgltf::sources::Array& array) -> std::span<const std::byte> {
                    return std::span(reinterpret_cast<const std::byte*>(array.bytes.data()), array.bytes.size());
                },
                [](const fastgltf::sources::Vector& vector) -> std::span<const std::byte> {
                    return std::span(reinterpret_cast<const std::byte*>(vector.bytes.data()), vector.bytes.size());
                },
                [](const fastgltf::sources::ByteView& byteView) -> std::span<const std::byte> {
                    return std::span(reinterpret_cast<const std::byte*>(byteView.bytes.data()),
                                     byteView.bytes.size());
                },
                [](const auto&) -> std::span<const std::byte> {
                    UHE_CORE_ERROR("Buffer has no loaded data (external buffers were not loaded?)");
                    return {};
                }},
            asset.buffers[bufferView.bufferIndex].data);

        if (bufferView.byteOffset > data.size() ||
            bufferView.byteLength > data.size() - bufferView.byteOffset)
        {
            UHE_CORE_ERROR("Buffer view {0} extends past the end of its buffer", bufferViewIndex);
            return {};
        }
        return data.subspan(bufferView.byteOffset, bufferView.byteLength);
    }

    // ── Compressed view ─────────────────────────────────────────────────────
    //
    // The meshopt extension object carries its own buffer/byteOffset/byteLength
    // pointing at the compressed bytes; the OUTER view's byteLength is the size
    // of the DECODED data. The two must not be confused - decoding into the
    // compressed size truncates every vertex stream.
    const auto& compression = *bufferView.meshoptCompression;

    const std::size_t decodedSize = bufferView.byteLength;
    if (compression.mode == fastgltf::MeshoptCompressionMode::Attributes &&
        compression.count * compression.byteStride != decodedSize)
    {
        UHE_CORE_ERROR("Meshopt view {0} declares {1} elements of stride {2} but a decoded size of {3}",
                       bufferViewIndex, compression.count, compression.byteStride, decodedSize);
    }

    scratch.assign(decodedSize + kDecodeAlignment, std::byte{0});
    std::byte* destination = AlignedPointer(scratch.data());

    // The compressed bytes sit in the buffer named by the extension object, not
    // in a nested view - there is no recursion in EXT_meshopt_compression, so
    // that region is read directly here rather than through this function.
    if (compression.bufferIndex >= asset.buffers.size())
    {
        UHE_CORE_ERROR("Meshopt view {0} references buffer {1} which does not exist", bufferViewIndex,
                       compression.bufferIndex);
        return {};
    }

    const auto sourceData = std::visit(
        fastgltf::visitor{
            [](const fastgltf::sources::Array& array) -> std::span<const std::byte> {
                return std::span(reinterpret_cast<const std::byte*>(array.bytes.data()), array.bytes.size());
            },
            [](const fastgltf::sources::Vector& vector) -> std::span<const std::byte> {
                return std::span(reinterpret_cast<const std::byte*>(vector.bytes.data()), vector.bytes.size());
            },
            [](const fastgltf::sources::ByteView& byteView) -> std::span<const std::byte> {
                return std::span(reinterpret_cast<const std::byte*>(byteView.bytes.data()), byteView.bytes.size());
            },
            [](const auto&) -> std::span<const std::byte> {
                return {};
            }},
        asset.buffers[compression.bufferIndex].data);

    if (compression.byteOffset > sourceData.size() ||
        compression.byteLength > sourceData.size() - compression.byteOffset)
    {
        UHE_CORE_ERROR("Meshopt view {0} compressed region extends past the end of its buffer", bufferViewIndex);
        return {};
    }

    const auto* source = reinterpret_cast<const unsigned char*>(sourceData.data() + compression.byteOffset);
    const std::size_t sourceSize = compression.byteLength;

    int error = 0;
    switch (compression.mode)
    {
        case fastgltf::MeshoptCompressionMode::Attributes:
            error = meshopt_decodeVertexBuffer(destination, compression.count, compression.byteStride, source,
                                               sourceSize);
            break;
        case fastgltf::MeshoptCompressionMode::Triangles:
            // Index buffers come back in the index type the file declared
            // (byteStride 2 or 4); the caller's accessor decides how to read it.
            error = meshopt_decodeIndexBuffer(destination, compression.count, compression.byteStride, source,
                                              sourceSize);
            break;
        case fastgltf::MeshoptCompressionMode::Indices:
            error = meshopt_decodeIndexSequence(destination, compression.count, compression.byteStride, source,
                                                sourceSize);
            break;
    }

    if (error != 0)
    {
        UHE_CORE_ERROR("meshopt decode failed for buffer view {0} (mode {1}); zero-filling", bufferViewIndex,
                       static_cast<int>(compression.mode));
        std::memset(destination, 0, decodedSize);
        return std::span<const std::byte>(destination, decodedSize);
    }

    // Filters run in-place on the decoded stream and only apply to attribute
    // data; an index buffer with a filter is malformed, but applying nothing is
    // the safe response.
    switch (compression.filter)
    {
        case fastgltf::MeshoptCompressionFilter::Octahedral:
            meshopt_decodeFilterOct(destination, compression.count, compression.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Quaternion:
            meshopt_decodeFilterQuat(destination, compression.count, compression.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Exponential:
            meshopt_decodeFilterExp(destination, compression.count, compression.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::None:
        default:
            break;
    }

    return std::span<const std::byte>(destination, decodedSize);
}

} // namespace UHE::RD3d
