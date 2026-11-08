#include "gfx/MipChain.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace walnut
{
    namespace
    {
        // The exact sRGB transfer functions (piecewise: a short linear segment near black, then a ~2.4 power).
        float SrgbToLinear(float c)
        {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(float c)
        {
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        // Decoding only ever sees 256 distinct byte values, so precompute them.
        const std::array<float, 256>& SrgbDecodeTable()
        {
            static const std::array<float, 256> table = [] {
                std::array<float, 256> t{};
                for (int i = 0; i < 256; ++i)
                    t[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
                return t;
            }();
            return table;
        }

        uint8_t ToByte(float value)
        {
            return static_cast<uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        ImageRgba8 Downsample(const ImageRgba8& source, bool srgb)
        {
            ImageRgba8 result;
            result.width = std::max(1u, source.width / 2);
            result.height = std::max(1u, source.height / 2);
            result.pixels.resize(static_cast<size_t>(result.width) * result.height * 4);

            const auto& decode = SrgbDecodeTable();
            auto texel = [&](uint32_t x, uint32_t y, int channel) -> float {
                // Clamp so odd sizes (e.g. 5 -> 2) and 1-texel-wide images still read valid texels.
                x = std::min(x, source.width - 1);
                y = std::min(y, source.height - 1);
                const uint8_t byte = source.pixels[(static_cast<size_t>(y) * source.width + x) * 4 + channel];
                return (srgb && channel < 3) ? decode[byte] : static_cast<float>(byte) / 255.0f;
            };

            for (uint32_t y = 0; y < result.height; ++y)
            {
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        const float average = 0.25f * (texel(2 * x, 2 * y, channel) + texel(2 * x + 1, 2 * y, channel) +
                                                       texel(2 * x, 2 * y + 1, channel) + texel(2 * x + 1, 2 * y + 1, channel));
                        const float encoded = (srgb && channel < 3) ? LinearToSrgb(average) : average;
                        result.pixels[(static_cast<size_t>(y) * result.width + x) * 4 + channel] = ToByte(encoded);
                    }
                }
            }
            return result;
        }
    }

    uint16_t CalcMipCount(uint32_t width, uint32_t height)
    {
        return static_cast<uint16_t>(std::bit_width(std::max(width, height))); // floor(log2(n)) + 1
    }

    std::vector<ImageRgba8> GenerateMipChain(ImageRgba8 mip0, bool srgb)
    {
        const uint16_t mipCount = CalcMipCount(mip0.width, mip0.height);
        std::vector<ImageRgba8> chain;
        chain.reserve(mipCount);
        chain.push_back(std::move(mip0));
        while (chain.size() < mipCount)
            chain.push_back(Downsample(chain.back(), srgb));
        return chain;
    }
}
