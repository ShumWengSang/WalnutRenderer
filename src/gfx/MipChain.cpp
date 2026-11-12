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

        ImageRgba8 Downsample(const ImageRgba8& source, MipContent content)
        {
            const bool srgb = content == MipContent::Srgb;
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
                    float average[4];
                    for (int channel = 0; channel < 4; ++channel)
                        average[channel] = 0.25f * (texel(2 * x, 2 * y, channel) + texel(2 * x + 1, 2 * y, channel) +
                                                    texel(2 * x, 2 * y + 1, channel) + texel(2 * x + 1, 2 * y + 1, channel));
                    if (content == MipContent::NormalMap)
                    {
                        // 0..1 -> -1..1, back to length 1, -> 0..1. (A zero-length average keeps "straight out".)
                        float n[3] = { average[0] * 2.0f - 1.0f, average[1] * 2.0f - 1.0f, average[2] * 2.0f - 1.0f };
                        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (length > 1e-6f)
                            for (float& c : n)
                                c /= length;
                        else
                            n[0] = n[1] = 0.0f, n[2] = 1.0f;
                        for (int c = 0; c < 3; ++c)
                            average[c] = n[c] * 0.5f + 0.5f;
                    }
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        const float encoded = (srgb && channel < 3) ? LinearToSrgb(average[channel]) : average[channel];
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

    std::vector<ImageRgba8> GenerateMipChain(ImageRgba8 mip0, MipContent content)
    {
        const uint16_t mipCount = CalcMipCount(mip0.width, mip0.height);
        std::vector<ImageRgba8> chain;
        chain.reserve(mipCount);
        chain.push_back(std::move(mip0));
        while (chain.size() < mipCount)
            chain.push_back(Downsample(chain.back(), content));
        return chain;
    }
}
