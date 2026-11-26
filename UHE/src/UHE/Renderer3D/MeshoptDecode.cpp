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

namespace
{
/// The loaded size of buffer \p index; nothing when its data was not loaded.
std::optional<std::size_t> LoadedBufferSize(const fastgltf::Asset& asset, std::size_t index)
{
    return std::visit(
        fastgltf::visitor{
            [](const fastgltf::sources::Array& array) -> std::optional<std::size_t> { return array.bytes.size(); },
            [](const fastgltf::sources::Vector& vector) -> std::optional<std::size_t> { return vector.bytes.size(); },
            [](const fastgltf::sources::ByteView& view) -> std::optional<std::size_t> { return view.bytes.size(); },
            [](const auto&) -> std::optional<std::size_t> { return std::nullopt; }},
        asset.buffers[index].data);
}

/// \p count elements of \p element bytes, \p stride apart, from \p offset, all
/// end by \p limit. Written without the multiplication a hostile count overflows.
bool Fits(std::size_t offset, std::size_t count, std::size_t stride, std::size_t element, std::size_t limit)
{
    if (offset > limit)
        return false;
    if (count == 0)
        return true;
    if (element > limit - offset)
        return false;
    const std::size_t room = limit - offset - element;
    return stride == 0 || count - 1 <= room / stride;
}
} // namespace

bool ValidateBufferRanges(const fastgltf::Asset& asset, std::string& error)
{
    for (std::size_t i = 0; i < asset.bufferViews.size(); ++i)
    {
        const auto& view = asset.bufferViews[i];
        // A compressed view's own range is its decoded size; the bytes read
        // from the buffer are the extension's.
        const std::size_t bufferIndex =
            view.meshoptCompression ? view.meshoptCompression->bufferIndex : view.bufferIndex;
        const std::size_t offset = view.meshoptCompression ? view.meshoptCompression->byteOffset : view.byteOffset;
        const std::size_t length = view.meshoptCompression ? view.meshoptCompression->byteLength : view.byteLength;
        if (bufferIndex >= asset.buffers.size())
        {
            error = "buffer view " + std::to_string(i) + " names buffer " + std::to_string(bufferIndex) +
                    ", which does not exist";
            return false;
        }
        const std::optional<std::size_t> size = LoadedBufferSize(asset, bufferIndex);
        if (!size)
        {
            error = "buffer " + std::to_string(bufferIndex) + " was not loaded";
            return false;
        }
        if (!Fits(offset, 1, 0, length, *size))
        {
            error = "buffer view " + std::to_string(i) + " (" + std::to_string(length) + " bytes at " +
                    std::to_string(offset) + ") ends past its buffer (" + std::to_string(*size) +
                    " bytes); is the .bin truncated?";
            return false;
        }
    }

    const auto viewLength = [&](std::size_t index) -> std::optional<std::size_t>
    {
        if (index >= asset.bufferViews.size())
            return std::nullopt;
        return asset.bufferViews[index].byteLength;
    };
    for (std::size_t i = 0; i < asset.accessors.size(); ++i)
    {
        const auto& accessor = asset.accessors[i];
        const std::size_t element = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
        const std::string name = "accessor " + std::to_string(i);
        if (accessor.bufferViewIndex)
        {
            const std::optional<std::size_t> length = viewLength(*accessor.bufferViewIndex);
            if (!length)
            {
                error = name + " names a buffer view that does not exist";
                return false;
            }
            const auto& stride = asset.bufferViews[*accessor.bufferViewIndex].byteStride;
            if (!Fits(accessor.byteOffset, accessor.count, stride ? *stride : element, element, *length))
            {
                error = name + " (" + std::to_string(accessor.count) + " elements) ends past its buffer view";
                return false;
            }
        }
        if (accessor.sparse)
        {
            const auto& sparse = *accessor.sparse;
            const std::size_t index = fastgltf::getComponentByteSize(sparse.indexComponentType);
            const std::optional<std::size_t> indices = viewLength(sparse.indicesBufferView);
            const std::optional<std::size_t> values = viewLength(sparse.valuesBufferView);
            if (!indices || !values || !Fits(sparse.indicesByteOffset, sparse.count, index, index, *indices) ||
                !Fits(sparse.valuesByteOffset, sparse.count, element, element, *values))
            {
                error = name + "'s sparse indices or values end past their buffer views";
                return false;
            }
        }
    }
    return true;
}

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
