#pragma once

#include <filesystem>

namespace walnut
{
    // The scene loaded when no --scene= is given (relative to the asset directory).
    inline constexpr const char8_t* kDefaultScene = u8"Sponza/Sponza.gltf";

    // <repo>/assets, where tools/fetch-assets.ps1 downloads models. Baked in by CMake (WALNUT_ASSET_DIR).
    std::filesystem::path GetAssetDirectory();

    // Absolute paths are returned as they are; relative ones are taken relative to the asset directory.
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& path);
}
