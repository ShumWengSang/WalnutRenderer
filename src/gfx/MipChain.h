#pragma once

#include <cstdint>
#include <vector>

namespace walnut
{
    // An 8-bit RGBA image in CPU memory, rows tightly packed (width * 4 bytes per row).
    struct ImageRgba8
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
    };

    // Number of mips in a full chain down to 1x1: floor(log2(max(width, height))) + 1. E.g. 256x256 -> 9.
    uint16_t CalcMipCount(uint32_t width, uint32_t height);

    // What the texels mean, which decides how they may be averaged.
    enum class MipContent
    {
        Linear,    // plain numbers (roughness, occlusion, ...): average as they are
        Srgb,      // sRGB colors: decode to linear light, average, re-encode (alpha stays linear)
        NormalMap, // unit vectors encoded as 0..1: average, then rescale to length 1 (see below)
    };

    // Builds the full mip chain [mip0, mip1, ..., 1x1] with a 2x2 box filter: each texel of the next mip is the
    // average of the 2x2 block of texels it covers.
    //
    // sRGB bytes are not proportional to light (128 is only about 22% of the light of 255), so averaging them
    // directly makes every mip darker than it should be; Srgb averages in linear light instead.
    //
    // Normal maps hold directions. The average of two different unit vectors is shorter than 1 (the average of
    // "left" and "right" is zero-length), so plain averaging makes far-away bumpy surfaces lose their normals'
    // length. NormalMap decodes each texel to a -1..1 vector, averages, and normalizes the result before encoding.
    std::vector<ImageRgba8> GenerateMipChain(ImageRgba8 mip0, MipContent content);
}
