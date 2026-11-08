#include "scene/Assets.h"

namespace walnut
{
    std::filesystem::path GetAssetDirectory()
    {
        // u8 prefix: the macro is UTF-8 (/utf-8), and a char8_t path is never converted through the ANSI code page.
        return std::filesystem::path(u8"" WALNUT_ASSET_DIR).lexically_normal();
    }

    std::filesystem::path ResolveAssetPath(const std::filesystem::path& path)
    {
        if (path.is_absolute())
            return path.lexically_normal();
        return (GetAssetDirectory() / path).lexically_normal();
    }
}
