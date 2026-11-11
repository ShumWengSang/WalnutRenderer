#pragma once

#include "gfx/MipChain.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace walnut
{
    // Reads a whole file into memory. UTF-8 safe (wide-character path). Returns nullopt if it can't be read.
    std::optional<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& path);

    // Decodes a JPEG or PNG into 8-bit RGBA (images with fewer channels are expanded: gray -> RGB, alpha = 255).
    // The bytes are returned as stored: no sRGB conversion happens here; the texture format decides that.
    std::optional<ImageRgba8> DecodeImage(std::span<const uint8_t> encoded, std::string& error);
}
