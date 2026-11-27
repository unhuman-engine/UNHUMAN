#pragma once

/**
 * @file ModelCookOptions.h
 * @brief The model cook settings the editor's Export, uhe_export and uhe_cook share (issue #45).
 *
 * Header-only glue between libUHE's model cook (RD3d::CookOptions) and the
 * texture encoder here, so all three cook a model the same way.
 */

#include <fstream>
#include <iterator>
#include "TextureCook.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace UHE::Cook
{

/// @brief Model cook options that encode every image to KTX2 with \p textures.
inline RD3d::CookOptions MakeModelCookOptions(const TextureCookOptions& textures)
{
    RD3d::CookOptions options;
    if (textures.Codec != TextureCodec::Keep)
    {
        options.EncodeTexture = [textures](std::vector<u8>& bytes, RD3d::CookTextureRole role, std::string& error)
        {
            const TextureRole mapped = role == RD3d::CookTextureRole::Color    ? TextureRole::Color
                                       : role == RD3d::CookTextureRole::Normal ? TextureRole::Normal
                                                                               : TextureRole::Data;
            std::vector<u8> encoded;
            if (!EncodeKTX2(bytes, mapped, textures, encoded, error))
                return false;
            bytes = std::move(encoded);
            return true;
        };
    }
    return options;
}

/**
 * @brief Cooks one standalone image file to KTX2 (ExportOptions::CookTexture).
 *
 * Sprites and UI images are looked at up close, so Auto picks UASTC for them,
 * not ETC1S; an explicit codec is kept.
 */
inline bool CookTextureFile(const std::filesystem::path& source, const std::filesystem::path& cooked,
                            TextureCookOptions options, std::string& error)
{
    std::ifstream in(source, std::ios::binary);
    if (!in)
    {
        error = "it cannot be read";
        return false;
    }
    const std::vector<u8> bytes(std::istreambuf_iterator<char>(in), {});
    if (options.Codec == TextureCodec::Auto)
        options.Codec = TextureCodec::UASTC;
    std::vector<u8> ktx2;
    if (!EncodeKTX2(bytes, TextureRole::Color, options, ktx2, error))
        return false;
    std::ofstream out(cooked, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ktx2.data()), static_cast<std::streamsize>(ktx2.size()));
    if (!out)
    {
        error = "the KTX2 file cannot be written";
        return false;
    }
    return true;
}

} // namespace UHE::Cook
