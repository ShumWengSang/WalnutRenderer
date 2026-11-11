#pragma once

#include <DirectXMath.h>

namespace walnut
{
    // The scene's light: one sun (a directional light: infinitely far away, so every point receives it from the
    // same direction) and a constant ambient term standing in for all the light bouncing around.
    struct SceneLighting
    {
        // Where the sun is in the sky. Elevation: angle above the horizon. Azimuth: angle around +Y, 0 = towards +Z.
        float sunAzimuth = DirectX::XMConvertToRadians(-35.0f);
        float sunElevation = DirectX::XMConvertToRadians(65.0f);
        DirectX::XMFLOAT3 sunColor = { 1.0f, 0.95f, 0.85f }; // linear, slightly warm
        float sunIntensity = 8.0f;
        DirectX::XMFLOAT3 ambientColor = { 0.55f, 0.65f, 0.85f }; // linear, sky blue
        float ambientIntensity = 0.25f;

        // Unit vector from a surface towards the sun.
        DirectX::XMFLOAT3 GetSunDirection() const;
    };
}
