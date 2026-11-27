#pragma once

/**
 * @file ModelCookOptions.h
 * @brief The model cook settings the editor's Export, uhe_export and uhe_cook share (issue #45).
 *
 * Header-only glue between libUHE's model cook (RD3d::CookOptions) and the
 * texture encoder here, so all three cook a model the same way.
 */

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

} // namespace UHE::Cook
