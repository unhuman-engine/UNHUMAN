#include "uhepch.h"
#include "RenderGraphTypes.h"
#include <cstring>
#include <functional>

namespace UHE::RHI
{

namespace
{
// Order-dependent FNV-1a mixing. Every hash contribution goes through these so
// field order changes do not silently change topology hashes mid-review.
constexpr u64 kHashSeed = 0xcbf29ce484222325ull;

void HashCombine(u64& hash, u64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
}

void HashCombine(u64& hash, u32 value)
{
    HashCombine(hash, static_cast<u64>(value));
}

void HashCombine(u64& hash, bool value)
{
    HashCombine(hash, value ? 1u : 0u);
}

// Bit-hash floats: equal bits hash equally, and -0.0f normalizes to 0.0f so
// clear colors written either way hash the same (docs §7 hash-comparability).
void HashCombine(u64& hash, float value)
{
    float normalized = value == 0.0f ? 0.0f : value;
    u32 bits = 0;
    static_assert(sizeof(bits) == sizeof(normalized), "float must be 32 bits for bit-hashing");
    std::memcpy(&bits, &normalized, sizeof(bits));
    HashCombine(hash, bits);
}

u64 HashString(const std::string& text)
{
    return std::hash<std::string>{}(text);
}
} // namespace

// ─── RGTextureDesc ──────────────────────────────────────────────────────────

u64 RGTextureDesc::Hash() const
{
    u64 hash = kHashSeed;
    HashCombine(hash, static_cast<u32>(format));
    HashCombine(hash, width);
    HashCombine(hash, height);
    HashCombine(hash, depth);
    HashCombine(hash, mipLevels);
    HashCombine(hash, layers);
    HashCombine(hash, sampleCount);
    HashCombine(hash, static_cast<u32>(usage));
    HashCombine(hash, static_cast<u32>(initialState));
    HashCombine(hash, static_cast<u32>(finalState));
    return hash;
}

bool RGTextureDesc::MateriallyEquals(const RGTextureDesc& other) const
{
    return format == other.format && width == other.width && height == other.height && depth == other.depth &&
           mipLevels == other.mipLevels && layers == other.layers && sampleCount == other.sampleCount &&
           usage == other.usage && initialState == other.initialState && finalState == other.finalState;
}

// ─── RGBufferDesc ───────────────────────────────────────────────────────────

u64 RGBufferDesc::Hash() const
{
    u64 hash = kHashSeed;
    HashCombine(hash, size);
    HashCombine(hash, static_cast<u32>(usage));
    HashCombine(hash, hostVisible);
    return hash;
}

bool RGBufferDesc::MateriallyEquals(const RGBufferDesc& other) const
{
    return size == other.size && usage == other.usage && hostVisible == other.hostVisible;
}

// ─── RGImportedDesc ─────────────────────────────────────────────────────────
// Included in topology hashing per §9.1.4: imported resources (swapchain,
// sensor buffers) change the schedule when their material identity changes.

u64 RGImportedDesc::Hash() const
{
    u64 hash = kHashSeed;
    HashCombine(hash, hashPayload);
    HashCombine(hash, width);
    HashCombine(hash, height);
    HashCombine(hash, depth);
    HashCombine(hash, sampleCount);
    return hash;
}

bool RGImportedDesc::MateriallyEquals(const RGImportedDesc& other) const
{
    return hashPayload == other.hashPayload && width == other.width && height == other.height && depth == other.depth &&
           sampleCount == other.sampleCount;
}

// ─── Logging helpers ────────────────────────────────────────────────────────
// These are debug/QA-facing strings (JSON dump, validation messages), not API
// surface — coverage beyond the used enum values is intentionally omitted.

const char* RGToString(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8_UNORM:
            return "RGBA8_UNORM";
        case TextureFormat::RGBA8_SRGB:
            return "RGBA8_SRGB";
        case TextureFormat::BGRA8_UNORM:
            return "BGRA8_UNORM";
        case TextureFormat::BGRA8_SRGB:
            return "BGRA8_SRGB";
        case TextureFormat::R8_UNORM:
            return "R8_UNORM";
        case TextureFormat::RG8_UNORM:
            return "RG8_UNORM";
        case TextureFormat::RGBA16F:
            return "RGBA16F";
        case TextureFormat::RGBA32F:
            return "RGBA32F";
        case TextureFormat::R32_SINT:
            return "R32_SINT";
        case TextureFormat::D24_UNORM_S8:
            return "D24_UNORM_S8";
        case TextureFormat::D32_FLOAT:
            return "D32_FLOAT";
        case TextureFormat::Undefined:
            break;
    }
    return "Undefined";
}

const char* RGToString(ImageState state)
{
    switch (state)
    {
        case ImageState::ColorAttachment:
            return "ColorAttachment";
        case ImageState::DepthAttachment:
            return "DepthAttachment";
        case ImageState::ShaderRead:
            return "ShaderRead";
        case ImageState::TransferSrc:
            return "TransferSrc";
        case ImageState::TransferDst:
            return "TransferDst";
        case ImageState::Present:
            return "Present";
        case ImageState::Storage:
            return "Storage";
        case ImageState::Undefined:
            break;
    }
    return "Undefined";
}

const char* RGToString(RGPassType type)
{
    switch (type)
    {
        case RGPassType::Graphics:
            return "Graphics";
        case RGPassType::Compute:
            return "Compute";
        case RGPassType::Transfer:
            return "Transfer";
        case RGPassType::RayTracing:
            return "RayTracing";
    }
    return "Graphics";
}

const char* RGToString(RGQueue queue)
{
    switch (queue)
    {
        case RGQueue::Graphics:
            return "Graphics";
        case RGQueue::Compute:
            return "Compute";
        case RGQueue::Transfer:
            return "Transfer";
    }
    return "Graphics";
}

const char* RGToString(LoadOp op)
{
    switch (op)
    {
        case LoadOp::Load:
            return "Load";
        case LoadOp::Clear:
            return "Clear";
        case LoadOp::DontCare:
            return "DontCare";
    }
    return "DontCare";
}

const char* RGToString(StoreOp op)
{
    switch (op)
    {
        case StoreOp::Store:
            return "Store";
        case StoreOp::DontCare:
            return "DontCare";
    }
    return "DontCare";
}

} // namespace UHE::RHI
