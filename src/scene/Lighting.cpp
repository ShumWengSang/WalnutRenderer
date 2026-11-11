#include "scene/Lighting.h"

#include <cmath>

namespace walnut
{
    DirectX::XMFLOAT3 SceneLighting::GetSunDirection() const
    {
        // Spherical angles to a direction, the same way Camera turns yaw/pitch into a forward vector.
        const float cosElevation = std::cos(sunElevation);
        return { cosElevation * std::sin(sunAzimuth), std::sin(sunElevation), cosElevation * std::cos(sunAzimuth) };
    }
}
