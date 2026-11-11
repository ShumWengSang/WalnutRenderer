#include "scene/ImageDecoder.h"

#include <stb_image.h>

#include <climits>
#include <cstring>
#include <fstream>

namespace walnut
{
    std::optional<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return std::nullopt;
        std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return std::nullopt;
        return bytes;
    }

    std::optional<ImageRgba8> DecodeImage(std::span<const uint8_t> encoded, std::string& error)
    {
        if (encoded.size() > INT_MAX)
        {
            error = "file too large";
            return std::nullopt;
        }
        int width = 0, height = 0, channelsInFile = 0;
        // Last argument 4: always give back RGBA, whatever the file stores.
        stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height,
                                                &channelsInFile, 4);
        if (!pixels)
        {
            error = stbi_failure_reason();
            return std::nullopt;
        }

        ImageRgba8 image{ .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height) };
        image.pixels.resize(static_cast<size_t>(width) * height * 4);
        std::memcpy(image.pixels.data(), pixels, image.pixels.size());
        stbi_image_free(pixels);
        return image;
    }
}
