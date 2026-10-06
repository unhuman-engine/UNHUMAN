// EXT_meshopt_compression buffer decoding (issue #29 Tier 4).
//
// The vendored fastgltf parses and validates meshopt-compressed buffer views
// but hands out the RAW COMPRESSED bytes through its default buffer data
// adapter - reading geometry from one of those without this translation unit
// produces garbage vertices, silently. The engine already links meshoptimizer
// for its optimization passes, so the decode is a matter of calling it with
// the metadata fastgltf stores on the buffer view.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <fastgltf/types.hpp>

namespace UHE::RD3d
{

// Returns the bytes of a buffer view, decoding EXT_meshopt_compression on the
// fly when the view is compressed. Compressed data is expanded into `scratch`,
// which must outlive the returned span; a plain view is returned directly with
// scratch untouched.
//
// A malformed compressed view logs an error and returns zero-filled bytes of
// the declared size: the accessor iteration that consumes this span indexes it
// by element count, so an empty or short span would read out of bounds.
std::span<const std::byte> GetBufferViewBytes(const fastgltf::Asset& asset, std::size_t bufferViewIndex,
                                              std::vector<std::byte>& scratch);

} // namespace UHE::RD3d
